/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2024 Jon Evans <jon@craftyjon.com>
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <api/api_handler_editor.h>

#include <api/api_enums.h>
#include <api/api_undo_stack.h>
#include <api/api_utils.h>
#include <eda_base_frame.h>
#include <tool/actions.h>
#include <eda_item.h>
#include <title_block.h>
#include <tool/action_manager.h>
#include <tool/tool_action.h>
#include <tool/tool_manager.h>
#include <wx/wx.h>

using namespace kiapi::common::commands;


API_HANDLER_EDITOR::API_HANDLER_EDITOR( EDA_BASE_FRAME* aFrame ) :
        API_HANDLER(),
        m_frame( aFrame ),
        m_revision( 0 )
{
    registerHandler<BeginCommit, BeginCommitResponse>( &API_HANDLER_EDITOR::handleBeginCommit );
    registerHandler<EndCommit, EndCommitResponse>( &API_HANDLER_EDITOR::handleEndCommit );
    registerHandler<CreateItems, CreateItemsResponse>( &API_HANDLER_EDITOR::handleCreateItems );
    registerHandler<UpdateItems, UpdateItemsResponse>( &API_HANDLER_EDITOR::handleUpdateItems );
    registerHandler<DeleteItems, DeleteItemsResponse>( &API_HANDLER_EDITOR::handleDeleteItems );
    registerHandler<HitTest, HitTestResponse>( &API_HANDLER_EDITOR::handleHitTest );
    registerHandler<GetTitleBlockInfo, types::TitleBlockInfo>( &API_HANDLER_EDITOR::handleGetTitleBlockInfo );
    registerHandler<SetTitleBlockInfo, google::protobuf::Empty>( &API_HANDLER_EDITOR::handleSetTitleBlockInfo );
    registerHandler<RefreshEditor, google::protobuf::Empty>( &API_HANDLER_EDITOR::handleRefreshEditor );
    registerHandler<FocusOnItem, FocusOnItemResponse>( &API_HANDLER_EDITOR::handleFocusOnItem );
    registerHandler<GetDocumentRevision, DocumentRevisionResponse>(
            &API_HANDLER_EDITOR::handleGetDocumentRevision );
    registerHandler<RunAction, RunActionResponse>( &API_HANDLER_EDITOR::handleRunAction );
    registerHandler<GetActions, GetActionsResponse>( &API_HANDLER_EDITOR::handleGetActions );
    registerHandler<GetItemCounts, GetItemCountsResponse>( &API_HANDLER_EDITOR::handleGetItemCounts );

    // Since 11.0
    registerHandler<Undo, UndoRedoResponse>( &API_HANDLER_EDITOR::handleUndo );
    registerHandler<Redo, UndoRedoResponse>( &API_HANDLER_EDITOR::handleRedo );
    registerHandler<GetUndoStack, UndoStackResponse>( &API_HANDLER_EDITOR::handleGetUndoStack );
}


HANDLER_RESULT<GetItemCountsResponse> API_HANDLER_EDITOR::handleGetItemCounts(
        const HANDLER_CONTEXT<GetItemCounts>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    GetItemCountsResponse response;
    response.set_revision( m_revision );

    // Dimension subtypes are one API type
    std::map<types::KiCadObjectType, uint32_t> counts;

    for( const auto& [type, count] : countItems( aCtx.Request.document() ) )
    {
        types::KiCadObjectType protoType;

        switch( type )
        {
        case PCB_DIM_ALIGNED_T:
        case PCB_DIM_CENTER_T:
        case PCB_DIM_RADIAL_T:
        case PCB_DIM_ORTHOGONAL_T:
        case PCB_DIM_LEADER_T:
            protoType = types::KiCadObjectType::KOT_PCB_DIMENSION;
            break;

        default:
            try
            {
                protoType = ToProtoEnum<KICAD_T, types::KiCadObjectType>( type );
            }
            catch( ... )
            {
                continue;
            }
        }

        counts[protoType] += count;
    }

    for( const auto& [type, count] : counts )
    {
        ItemCount* entry = response.add_counts();
        entry->set_type( type );
        entry->set_count( count );
    }

    return response;
}


void API_HANDLER_EDITOR::advanceRevision( bool aComplete, const COMMIT* aCommit )
{
    REVISION_CHANGES changes;
    changes.Complete = aComplete;

    if( aCommit )
    {
        COMMIT_CHANGES commitChanges = classifyCommit( *aCommit );
        changes.Changed = std::move( commitChanges.Created );
        changes.Changed.insert( changes.Changed.end(), commitChanges.Updated.begin(),
                                commitChanges.Updated.end() );
        changes.Deleted = std::move( commitChanges.Deleted );
    }

    advanceRevision( std::move( changes ) );
}


void API_HANDLER_EDITOR::advanceRevision( REVISION_CHANGES aChanges )
{
    ++m_revision;
    aChanges.Revision = m_revision;

    m_revisionChanges.push_back( std::move( aChanges ) );

    while( m_revisionChanges.size() > MAX_REVISION_CHANGES )
        m_revisionChanges.pop_front();
}


void API_HANDLER_EDITOR::publishDocumentChanged( const std::string& aClientName, const wxString& aMessage,
                                                 const std::vector<KIID>& aCreated, const std::vector<KIID>& aUpdated,
                                                 const std::vector<KIID>& aDeleted )
{
    REVISION_CHANGES changes;
    changes.Complete = true;
    changes.Changed.insert( changes.Changed.end(), aCreated.begin(), aCreated.end() );
    changes.Changed.insert( changes.Changed.end(), aUpdated.begin(), aUpdated.end() );
    changes.Deleted = aDeleted;
    advanceRevision( std::move( changes ) );

    if( !Server() )
        return;

    events::Event event;
    events::DocumentChanged& changed = *event.mutable_document_changed();
    fillDocumentChanged( changed, aClientName, aMessage, nullptr, nullptr );

    for( const KIID& id : aCreated )
        changed.add_created()->set_value( id.AsStdString() );

    for( const KIID& id : aUpdated )
        changed.add_updated()->set_value( id.AsStdString() );

    for( const KIID& id : aDeleted )
        changed.add_deleted()->set_value( id.AsStdString() );

    publish( event );
}


bool API_HANDLER_EDITOR::undoRedoInFrame( bool aRedo )
{
    TOOL_MANAGER* toolMgr = editorToolManager();

    if( !m_frame || !toolMgr )
        return false;

    if( aRedo ? m_frame->GetRedoCommandCount() == 0 : m_frame->GetUndoCommandCount() == 0 )
        return false;

    return toolMgr->RunAction( aRedo ? ACTIONS::redo : ACTIONS::undo, true );
}


HANDLER_RESULT<UndoRedoResponse> API_HANDLER_EDITOR::undoRedo( const DocumentSpecifier& aDocument,
                                                                const std::string& aClientName, bool aRedo,
                                                                uint32_t aCount )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aDocument );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    // A client that called BeginCommit is mid-edit even if it has staged nothing yet, and staged
    // changes hold pointers into the document that an undo would pull from under them
    for( const auto& [client, commit] : m_commits )
    {
        bool open = m_activeClients.count( client ) > 0;
        bool staged = commit.second && !commit.second->Empty();

        if( open || staged )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BUSY );
            e.set_error_message( fmt::format( "cannot {} while client '{}' has an open commit",
                                              aRedo ? "redo" : "undo", client ) );
            return tl::unexpected( e );
        }
    }

    const uint32_t   count = std::max<uint32_t>( 1, aCount );
    UndoRedoResponse response;

    if( API_UNDO_STACK* stack = apiUndoStack() )
    {
        for( uint32_t ii = 0; ii < count; ii++ )
        {
            std::optional<API_UNDO_STACK::RESULT> result = aRedo ? stack->Redo() : stack->Undo();

            if( !result )
                break;

            response.set_applied( response.applied() + 1 );

            wxString message = wxString::Format( aRedo ? _( "Redo %s" ) : _( "Undo %s" ), result->Entry.Description );
            publishDocumentChanged( aClientName, message, result->Created, result->Updated, result->Deleted );
        }

        response.set_undo_count( static_cast<uint32_t>( stack->UndoCount() ) );
        response.set_redo_count( static_cast<uint32_t>( stack->RedoCount() ) );
    }
    else if( m_frame )
    {
        for( uint32_t ii = 0; ii < count; ii++ )
        {
            if( !undoRedoInFrame( aRedo ) )
                break;

            response.set_applied( response.applied() + 1 );

            // The frame's undo does not say which items it touched
            bumpRevision();
        }

        response.set_undo_count( static_cast<uint32_t>( m_frame->GetUndoCommandCount() ) );
        response.set_redo_count( static_cast<uint32_t>( m_frame->GetRedoCommandCount() ) );
    }
    else
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNIMPLEMENTED );
        e.set_error_message( "this document keeps no undo history" );
        return tl::unexpected( e );
    }

    return response;
}


HANDLER_RESULT<UndoRedoResponse> API_HANDLER_EDITOR::handleUndo( const HANDLER_CONTEXT<Undo>& aCtx )
{
    return undoRedo( aCtx.Request.document(), aCtx.ClientName, false, aCtx.Request.count() );
}


HANDLER_RESULT<UndoRedoResponse> API_HANDLER_EDITOR::handleRedo( const HANDLER_CONTEXT<Redo>& aCtx )
{
    return undoRedo( aCtx.Request.document(), aCtx.ClientName, true, aCtx.Request.count() );
}


HANDLER_RESULT<UndoStackResponse> API_HANDLER_EDITOR::handleGetUndoStack( const HANDLER_CONTEXT<GetUndoStack>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    UndoStackResponse response;

    auto packEntry =
            []( UndoStackEntry* aOut, const API_UNDO_STACK::ENTRY& aEntry )
            {
                aOut->set_description( aEntry.Description.ToUTF8() );
                aOut->set_client_name( aEntry.ClientName );
                aOut->set_item_count( static_cast<uint32_t>( aEntry.ItemCount ) );

                if( aEntry.CommitId )
                    aOut->mutable_commit_id()->set_value( aEntry.CommitId->AsStdString() );
            };

    if( API_UNDO_STACK* stack = apiUndoStack() )
    {
        for( const API_UNDO_STACK::ENTRY& entry : stack->UndoEntries() )
            packEntry( response.add_undo(), entry );

        for( const API_UNDO_STACK::ENTRY& entry : stack->RedoEntries() )
            packEntry( response.add_redo(), entry );
    }
    else if( m_frame )
    {
        for( const PICKED_ITEMS_LIST* command : m_frame->GetUndoList().m_CommandsList )
        {
            UndoStackEntry* entry = response.add_undo();
            entry->set_description( command->GetDescription().ToUTF8() );
            entry->set_item_count( static_cast<uint32_t>( command->GetCount() ) );
        }

        for( const PICKED_ITEMS_LIST* command : m_frame->GetRedoList().m_CommandsList )
        {
            UndoStackEntry* entry = response.add_redo();
            entry->set_description( command->GetDescription().ToUTF8() );
            entry->set_item_count( static_cast<uint32_t>( command->GetCount() ) );
        }
    }

    return response;
}


std::optional<API_HANDLER_EDITOR::REVISION_CHANGES> API_HANDLER_EDITOR::changesSince( uint64_t aRevision ) const
{
    REVISION_CHANGES result;
    result.Revision = m_revision;
    result.Complete = true;

    if( aRevision >= m_revision )
        return result;

    // Every step in (aRevision, m_revision] must be in the log
    if( m_revisionChanges.empty() || m_revisionChanges.front().Revision > aRevision + 1 )
        return std::nullopt;

    // Replayed in order so that an item deleted and re-created (a footprint update replaces the
    // object under the same id) ends up changed, and one changed then deleted ends up deleted
    std::set<KIID> changed, deleted;

    for( const REVISION_CHANGES& step : m_revisionChanges )
    {
        if( step.Revision <= aRevision )
            continue;

        if( !step.Complete )
            return std::nullopt;

        for( const KIID& id : step.Deleted )
        {
            changed.erase( id );
            deleted.insert( id );
        }

        for( const KIID& id : step.Changed )
        {
            deleted.erase( id );
            changed.insert( id );
        }
    }

    result.Changed.assign( changed.begin(), changed.end() );
    result.Deleted.assign( deleted.begin(), deleted.end() );

    return result;
}


const std::set<std::string>& API_HANDLER_EDITOR::headlessActions() const
{
    static const std::set<std::string> none;
    return none;
}


bool API_HANDLER_EDITOR::ownsAction( const std::string& aAction ) const
{
    for( const std::string& prefix : actionPrefixes() )
    {
        if( aAction.starts_with( prefix ) )
            return true;
    }

    return false;
}


HANDLER_RESULT<RunActionResponse> API_HANDLER_EDITOR::handleRunAction( const HANDLER_CONTEXT<RunAction>& aCtx )
{
    const std::string& action = aCtx.Request.action();

    // Another editor's action: let its handler answer
    if( !ownsAction( action ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    TOOL_MANAGER* toolMgr = editorToolManager();

    RunActionResponse response;

    if( !toolMgr )
    {
        response.set_status( RunActionStatus::RAS_FRAME_NOT_OPEN );
        return response;
    }

    if( !m_frame )
    {
        // Only actions whose tools work without a window can run in kicad-cli api-server.  Unknown
        // names are still reported through the response, as they are with a frame.
        if( !headlessActions().contains( action ) && toolMgr->GetActionManager()->FindAction( action ) )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_UNIMPLEMENTED );
            e.set_error_message( fmt::format( "action {} is not available in headless mode; see GetActions", action ) );
            return tl::unexpected( e );
        }

        ensureHeadlessTools();
    }

    if( API_UNDO_STACK* stack = apiUndoStack() )
        stack->SetNextAttribution( aCtx.ClientName, std::nullopt );

    if( toolMgr->RunAction( action, true ) )
    {
        response.set_status( RunActionStatus::RAS_OK );

        // A headless tool commits straight to the document; nothing else reports the change
        if( !m_frame )
            bumpRevision();
    }
    else
    {
        response.set_status( RunActionStatus::RAS_INVALID );
    }

    return response;
}


HANDLER_RESULT<GetActionsResponse> API_HANDLER_EDITOR::handleGetActions( const HANDLER_CONTEXT<GetActions>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    TOOL_MANAGER* toolMgr = editorToolManager();

    if( !toolMgr )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "this editor has no tool actions" );
        return tl::unexpected( e );
    }

    const std::set<std::string>& headless = headlessActions();
    GetActionsResponse           response;

    // Every TOOL_ACTION in the process is registered with every ACTION_MANAGER; list the ones
    // that belong to this editor, in name order
    for( const auto& [name, action] : toolMgr->GetActionManager()->GetActions() )
    {
        if( !ownsAction( name ) )
            continue;

        ActionInfo* info = response.add_actions();
        info->set_name( name );
        info->set_label( action->GetFriendlyName().ToUTF8() );
        info->set_description( action->GetDescription().ToUTF8() );
        info->set_headless_capable( headless.contains( name ) );
    }

    return response;
}


HANDLER_RESULT<DocumentRevisionResponse> API_HANDLER_EDITOR::handleGetDocumentRevision(
        const HANDLER_CONTEXT<GetDocumentRevision>& aCtx )
{
    // Another editor's document: let its handler answer
    if( aCtx.Request.document().type() != thisDocumentType() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    DocumentRevisionResponse response;
    response.set_revision( m_revision );
    return response;
}


types::FrameType API_HANDLER_EDITOR::thisFrameType() const
{
    switch( thisDocumentType() )
    {
    case types::DOCTYPE_SCHEMATIC:     return types::FT_SCHEMATIC_EDITOR;
    case types::DOCTYPE_SYMBOL:        return types::FT_SYMBOL_EDITOR;
    case types::DOCTYPE_PCB:           return types::FT_PCB_EDITOR;
    case types::DOCTYPE_FOOTPRINT:     return types::FT_FOOTPRINT_EDITOR;
    case types::DOCTYPE_DRAWING_SHEET: return types::FT_DRAWING_SHEET_EDITOR;
    default:                           return types::FT_UNKNOWN;
    }
}


HANDLER_RESULT<google::protobuf::Empty> API_HANDLER_EDITOR::handleRefreshEditor(
        const HANDLER_CONTEXT<RefreshEditor>& aCtx )
{
    // Let the handler for the requested editor answer; an unspecified frame refreshes this one
    if( aCtx.Request.frame() != types::FT_UNKNOWN && aCtx.Request.frame() != thisFrameType() )
    {
        ApiResponseStatus e;
        // No message needed for AS_UNHANDLED; this is an internal flag for the API server
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    // Headless: nothing to refresh, but the request is still a success
    if( m_frame )
        m_frame->Refresh();

    return google::protobuf::Empty();
}


HANDLER_RESULT<FocusOnItemResponse> API_HANDLER_EDITOR::handleFocusOnItem(
        const HANDLER_CONTEXT<FocusOnItem>& aCtx )
{
    const SelectionSpec& spec = aCtx.Request.focus_item();
    types::DocumentType  docType = thisDocumentType();

    // Footprint and pad specs address a board; sheet paths address a schematic.  Pass anything
    // else on to the other registered handlers.
    bool boardSpec = spec.has_footprint() || spec.has_pad();
    bool boardDoc = docType == types::DOCTYPE_PCB || docType == types::DOCTYPE_FOOTPRINT;

    if( ( boardSpec && !boardDoc ) || ( spec.has_sheet_path() && docType != types::DOCTYPE_SCHEMATIC ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    FocusOnItemResponse response;

    if( !m_frame )
    {
        // Headless: there is no view to focus, but the request is still a success
        response.set_status( CrossProbeStatus::CPS_OK );
        return response;
    }

    focusOnItem( spec, response );
    return response;
}


HANDLER_RESULT<BeginCommitResponse> API_HANDLER_EDITOR::handleBeginCommit(
        const HANDLER_CONTEXT<BeginCommit>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    // Before 11.0, commit requests had no header so we assume they are for the PCB editor
    if( aCtx.Request.has_header() && !validateItemHeaderDocument( aCtx.Request.header() ) )
    {
        ApiResponseStatus e;
        // No message needed for AS_UNHANDLED; this is an internal flag for the API server
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    if( m_commits.count( aCtx.ClientName ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "the client {} already has a commit in progress",
                                          aCtx.ClientName ) );
        return tl::unexpected( e );
    }

    wxASSERT( !m_activeClients.count( aCtx.ClientName ) );

    BeginCommitResponse response;

    KIID id;
    m_commits[aCtx.ClientName] = std::make_pair( id, createCommit() );
    response.mutable_id()->set_value( id.AsStdString() );

    m_activeClients.insert( aCtx.ClientName );

    return response;
}


HANDLER_RESULT<EndCommitResponse> API_HANDLER_EDITOR::handleEndCommit(
        const HANDLER_CONTEXT<EndCommit>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    // Before 11.0, commit requests had no header so we assume they are for the PCB editor
    if( aCtx.Request.has_header() && !validateItemHeaderDocument( aCtx.Request.header() ) )
    {
        ApiResponseStatus e;
        // No message needed for AS_UNHANDLED; this is an internal flag for the API server
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    if( !m_commits.count( aCtx.ClientName ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "the client {} does not has a commit in progress",
                                          aCtx.ClientName ) );
        return tl::unexpected( e );
    }

    wxASSERT( m_activeClients.count( aCtx.ClientName ) );

    const std::pair<KIID, std::unique_ptr<COMMIT>>& pair = m_commits.at( aCtx.ClientName );
    const KIID& id = pair.first;
    const std::unique_ptr<COMMIT>& commit = pair.second;

    EndCommitResponse response;

    // Do not check IDs with drop; it is a safety net in case the id was lost on the client side
    switch( aCtx.Request.action() )
    {
    case kiapi::common::commands::CMA_DROP:
    {
        commit->Revert();
        m_commits.erase( aCtx.ClientName );
        m_activeClients.erase( aCtx.ClientName );
        break;
    }

    case kiapi::common::commands::CMA_COMMIT:
    {
        if( aCtx.Request.id().value().compare( id.AsStdString() ) != 0 )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( fmt::format( "the id {} does not match the commit in progress",
                                              aCtx.Request.id().value() ) );
            return tl::unexpected( e );
        }

        pushCurrentCommit( aCtx.ClientName, wxString( aCtx.Request.message().c_str(), wxConvUTF8 ) );
        break;
    }

    default:
        break;
    }

    return response;
}


COMMIT* API_HANDLER_EDITOR::getCurrentCommit( const std::string& aClientName )
{
    if( !m_commits.count( aClientName ) )
    {
        KIID id;
        m_commits[aClientName] = std::make_pair( id, createCommit() );
    }

    return m_commits.at( aClientName ).second.get();
}


void API_HANDLER_EDITOR::pushCurrentCommit( const std::string& aClientName,
                                            const wxString& aMessage )
{
    auto it = m_commits.find( aClientName );

    if( it == m_commits.end() )
        return;

    // Take ownership so that the entries can still be read after the map entry is gone; Push()
    // clears the staged list, so the event is built first.
    KIID                    id = it->second.first;
    std::unique_ptr<COMMIT> commit = std::move( it->second.second );
    m_commits.erase( it );
    m_activeClients.erase( aClientName );

    wxString message = aMessage.IsEmpty() ? m_defaultCommitMessage : aMessage;

    advanceRevision( true, commit.get() );

    events::Event event;
    fillDocumentChanged( *event.mutable_document_changed(), aClientName, message, &id, commit.get() );

    // The commit hands its undo list to the headless stack while it pushes; name its author
    if( API_UNDO_STACK* stack = apiUndoStack() )
        stack->SetNextAttribution( aClientName, id );

    commit->Push( message );

    publish( event );
}


void API_HANDLER_EDITOR::NotifyDocumentOpened()
{
    if( !Server() )
        return;

    if( std::optional<DocumentSpecifier> doc = Document() )
    {
        events::Event event;
        *event.mutable_document_opened()->mutable_document() = std::move( *doc );
        publish( event );
    }
}


void API_HANDLER_EDITOR::bumpRevision()
{
    publishDocumentChanged( "", wxEmptyString );
}


void API_HANDLER_EDITOR::publishDocumentChanged( const std::string& aClientName, const wxString& aMessage,
                                                 const KIID* aCommitId, const COMMIT* aCommit )
{
    advanceRevision( aCommit != nullptr, aCommit );

    if( !Server() )
        return;

    events::Event event;
    fillDocumentChanged( *event.mutable_document_changed(), aClientName, aMessage, aCommitId, aCommit );
    publish( event );
}


void API_HANDLER_EDITOR::fillDocumentChanged( events::DocumentChanged& aEvent, const std::string& aClientName,
                                              const wxString& aMessage, const KIID* aCommitId,
                                              const COMMIT* aCommit ) const
{
    if( std::optional<DocumentSpecifier> doc = Document() )
        *aEvent.mutable_document() = std::move( *doc );

    aEvent.set_revision( m_revision );
    aEvent.set_message( aMessage.ToUTF8() );
    aEvent.set_client_name( aClientName );

    if( aCommitId )
        aEvent.mutable_commit_id()->set_value( aCommitId->AsStdString() );

    if( aCommit )
    {
        COMMIT_CHANGES changes = classifyCommit( *aCommit );

        for( const KIID& id : changes.Created )
            aEvent.add_created()->set_value( id.AsStdString() );

        for( const KIID& id : changes.Updated )
            aEvent.add_updated()->set_value( id.AsStdString() );

        for( const KIID& id : changes.Deleted )
            aEvent.add_deleted()->set_value( id.AsStdString() );
    }
}


API_HANDLER_EDITOR::COMMIT_CHANGES API_HANDLER_EDITOR::classifyCommit( const COMMIT& aCommit )
{
    std::vector<KIID> added;
    std::vector<KIID> modified;
    std::vector<KIID> removed;

    aCommit.ForEachEntry(
            [&]( EDA_ITEM* aItem, CHANGE_TYPE aType )
            {
                if( !aItem )
                    return;

                switch( aType )
                {
                case CHT_ADD:    added.push_back( aItem->m_Uuid );    break;
                case CHT_MODIFY: modified.push_back( aItem->m_Uuid ); break;
                case CHT_REMOVE: removed.push_back( aItem->m_Uuid );  break;
                default:                                              break;
                }
            } );

    // An item replaced by a new one with the same id (footprints and groups are updated that
    // way) is an update to the client, not a deletion and a creation
    std::set<KIID> addedSet( added.begin(), added.end() );
    std::set<KIID> removedSet( removed.begin(), removed.end() );

    COMMIT_CHANGES changes;
    changes.Updated = modified;

    for( const KIID& id : added )
    {
        if( removedSet.contains( id ) )
            changes.Updated.push_back( id );
        else
            changes.Created.push_back( id );
    }

    for( const KIID& id : removed )
    {
        if( !addedSet.contains( id ) )
            changes.Deleted.push_back( id );
    }

    return changes;
}


void API_HANDLER_EDITOR::publishProjectChanged( events::ProjectChangeKind aKind,
                                                const std::string& aClientName )
{
    if( !Server() )
        return;

    std::optional<DocumentSpecifier> doc = Document();

    if( !doc || !doc->has_project() )
        return;

    events::Event           event;
    events::ProjectChanged& changed = *event.mutable_project_changed();
    *changed.mutable_project() = doc->project();
    changed.set_kind( aKind );
    changed.set_client_name( aClientName );
    publish( event );
}


void API_HANDLER_EDITOR::notifyDocumentSaved( const wxString& aPath )
{
    // Nothing in the document changed
    advanceRevision( true, nullptr );

    if( !Server() )
        return;

    events::Event event;
    events::DocumentSaved& saved = *event.mutable_document_saved();

    if( std::optional<DocumentSpecifier> doc = Document() )
        *saved.mutable_document() = std::move( *doc );

    saved.set_path( aPath.ToUTF8() );
    saved.set_revision( m_revision );
    publish( event );
}


HANDLER_RESULT<bool> API_HANDLER_EDITOR::validateDocument( const DocumentSpecifier& aDocument )
{
    // Another editor's document: answer AS_UNHANDLED so that the API server passes the request
    // on to that editor's handler instead of failing it here.
    if( aDocument.type() != thisDocumentType() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        // No error message, this is a flag that the server should try a different handler
        return tl::unexpected( e );
    }

    if( tl::expected<bool, ApiResponseStatus> result = validateDocumentInternal( aDocument ); !result )
    {
        if( result.error().status() != ApiStatusCode::AS_UNHANDLED && result.error().error_message().empty() )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( fmt::format( "the requested document {} is not open",
                                              aDocument.board_filename() ) );
            return tl::unexpected( e );
        }

        return tl::unexpected( result.error() );
    }

    return true;
}


HANDLER_RESULT<std::optional<KIID>> API_HANDLER_EDITOR::validateItemHeaderDocument(
        const types::ItemHeader& aHeader )
{
    if( !aHeader.has_document() || aHeader.document().type() != thisDocumentType() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        // No error message, this is a flag that the server should try a different handler
        return tl::unexpected( e );
    }

    HANDLER_RESULT<bool> documentValidation = validateDocument( aHeader.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( tl::expected<bool, ApiResponseStatus> result = validateDocumentInternal( aHeader.document() ); !result )
        return tl::unexpected( result.error() );

    if( aHeader.has_container() )
    {
        return KIID( aHeader.container().value() );
    }

    // Valid header, but no container provided
    return std::nullopt;
}


std::optional<ApiResponseStatus> API_HANDLER_EDITOR::checkForBusy()
{
    if( !m_frame )
        return std::nullopt;

    if( !m_frame->CanAcceptApiCommands() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BUSY );
        e.set_error_message( "KiCad is busy and cannot respond to API requests right now" );
        return e;
    }

    return std::nullopt;
}


HANDLER_RESULT<CreateItemsResponse> API_HANDLER_EDITOR::handleCreateItems(
        const HANDLER_CONTEXT<CreateItems>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    CreateItemsResponse response;

    HANDLER_RESULT<ItemRequestStatus> result = handleCreateUpdateItemsInternal( true,
            aCtx.ClientName,
            aCtx.Request.header(), aCtx.Request.items(),
            [&]( const ItemStatus& aStatus, const google::protobuf::Any& aItem )
            {
                ItemCreationResult itemResult;
                itemResult.mutable_status()->CopyFrom( aStatus );
                itemResult.mutable_item()->CopyFrom( aItem );
                response.mutable_created_items()->Add( std::move( itemResult ) );
            } );

    if( !result.has_value() )
        return tl::unexpected( result.error() );

    response.set_status( *result );
    return response;
}


HANDLER_RESULT<UpdateItemsResponse> API_HANDLER_EDITOR::handleUpdateItems(
        const HANDLER_CONTEXT<UpdateItems>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    UpdateItemsResponse response;

    HANDLER_RESULT<ItemRequestStatus> result = handleCreateUpdateItemsInternal( false,
            aCtx.ClientName,
            aCtx.Request.header(), aCtx.Request.items(),
            [&]( const ItemStatus& aStatus, const google::protobuf::Any& aItem )
            {
                ItemUpdateResult itemResult;
                itemResult.mutable_status()->CopyFrom( aStatus );
                itemResult.mutable_item()->CopyFrom( aItem );
                response.mutable_updated_items()->Add( std::move( itemResult ) );
            } );

    if( !result.has_value() )
        return tl::unexpected( result.error() );

    response.set_status( *result );
    return response;
}


HANDLER_RESULT<DeleteItemsResponse> API_HANDLER_EDITOR::handleDeleteItems(
        const HANDLER_CONTEXT<DeleteItems>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( !validateItemHeaderDocument( aCtx.Request.header() ) )
    {
        ApiResponseStatus e;
        // No message needed for AS_UNHANDLED; this is an internal flag for the API server
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    std::map<KIID, ItemDeletionStatus> itemsToDelete;

    for( const kiapi::common::types::KIID& kiidBuf : aCtx.Request.item_ids() )
    {
        if( !kiidBuf.value().empty() )
        {
            KIID kiid( kiidBuf.value() );
            itemsToDelete[kiid] = ItemDeletionStatus::IDS_NONEXISTENT;
        }
    }

    if( itemsToDelete.empty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "no valid items to delete were given" );
        return tl::unexpected( e );
    }

    deleteItemsInternal( itemsToDelete, aCtx.ClientName );

    DeleteItemsResponse response;

    for( const auto& [id, status] : itemsToDelete )
    {
        ItemDeletionResult* result = response.add_deleted_items();
        result->mutable_id()->set_value( id.AsStdString() );
        result->set_status( status );
    }

    response.set_status( kiapi::common::types::ItemRequestStatus::IRS_OK );
    return response;
}


HANDLER_RESULT<HitTestResponse> API_HANDLER_EDITOR::handleHitTest(
        const HANDLER_CONTEXT<HitTest>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( !validateItemHeaderDocument( aCtx.Request.header() ) )
    {
        ApiResponseStatus e;
        // No message needed for AS_UNHANDLED; this is an internal flag for the API server
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    HitTestResponse response;

    std::optional<EDA_ITEM*> item = getItemFromDocument( aCtx.Request.header().document(),
                                                         KIID( aCtx.Request.id().value() ) );

    if( !item )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "the requested item ID is not present in the given document" );
        return tl::unexpected( e );
    }

    const EDA_IU_SCALE& scale = getIuScale();
    VECTOR2I posIu = UnpackVector2( aCtx.Request.position(), scale );
    int toleranceIu = scale.NmToIU( aCtx.Request.tolerance() );

    if( ( *item )->HitTest( posIu, toleranceIu ) )
        response.set_result( HitTestResult::HTR_HIT );
    else
        response.set_result( HitTestResult::HTR_NO_HIT );

    return response;
}


std::vector<KICAD_T> API_HANDLER_EDITOR::parseRequestedItemTypes( const google::protobuf::RepeatedField<int>& aTypes )
{
    std::vector<KICAD_T> types;

    for( int typeRaw : aTypes )
    {
        auto typeMessage = static_cast<types::KiCadObjectType>( typeRaw );

        if( KICAD_T type = FromProtoEnum<KICAD_T>( typeMessage ); type != TYPE_NOT_INIT )
            types.emplace_back( type );
    }

    return types;
}


HANDLER_RESULT<types::TitleBlockInfo>
API_HANDLER_EDITOR::handleGetTitleBlockInfo( const HANDLER_CONTEXT<GetTitleBlockInfo>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    std::optional<TITLE_BLOCK*> optBlock = getTitleBlock( aCtx.Request.document() );

    if( !optBlock )
    {
        ApiResponseStatus e;
        e.set_status( AS_BAD_REQUEST );
        e.set_error_message( "this editor does not support a title block" );
        return tl::unexpected( e );
    }

    TITLE_BLOCK& block = **optBlock;

    types::TitleBlockInfo response;

    response.set_title( block.GetTitle().ToUTF8() );
    response.set_date( block.GetDate().ToUTF8() );
    response.set_revision( block.GetRevision().ToUTF8() );
    response.set_company( block.GetCompany().ToUTF8() );
    response.set_comment1( block.GetComment( 0 ).ToUTF8() );
    response.set_comment2( block.GetComment( 1 ).ToUTF8() );
    response.set_comment3( block.GetComment( 2 ).ToUTF8() );
    response.set_comment4( block.GetComment( 3 ).ToUTF8() );
    response.set_comment5( block.GetComment( 4 ).ToUTF8() );
    response.set_comment6( block.GetComment( 5 ).ToUTF8() );
    response.set_comment7( block.GetComment( 6 ).ToUTF8() );
    response.set_comment8( block.GetComment( 7 ).ToUTF8() );
    response.set_comment9( block.GetComment( 8 ).ToUTF8() );

    return response;
}


HANDLER_RESULT<google::protobuf::Empty>
API_HANDLER_EDITOR::handleSetTitleBlockInfo( const HANDLER_CONTEXT<SetTitleBlockInfo>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( !aCtx.Request.has_title_block() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "SetTitleBlockInfo requires title_block" );
        return tl::unexpected( e );
    }

    std::optional<TITLE_BLOCK*> optBlock = getTitleBlock( aCtx.Request.document() );

    if( !optBlock )
    {
        ApiResponseStatus e;
        e.set_status( AS_BAD_REQUEST );
        e.set_error_message( "this editor does not support a title block" );
        return tl::unexpected( e );
    }

    TITLE_BLOCK& block = **optBlock;

    const types::TitleBlockInfo& request = aCtx.Request.title_block();

    block.SetTitle( wxString::FromUTF8( request.title() ) );
    block.SetDate( wxString::FromUTF8( request.date() ) );
    block.SetRevision( wxString::FromUTF8( request.revision() ) );
    block.SetCompany( wxString::FromUTF8( request.company() ) );
    block.SetComment( 0, wxString::FromUTF8( request.comment1() ) );
    block.SetComment( 1, wxString::FromUTF8( request.comment2() ) );
    block.SetComment( 2, wxString::FromUTF8( request.comment3() ) );
    block.SetComment( 3, wxString::FromUTF8( request.comment4() ) );
    block.SetComment( 4, wxString::FromUTF8( request.comment5() ) );
    block.SetComment( 5, wxString::FromUTF8( request.comment6() ) );
    block.SetComment( 6, wxString::FromUTF8( request.comment7() ) );
    block.SetComment( 7, wxString::FromUTF8( request.comment8() ) );
    block.SetComment( 8, wxString::FromUTF8( request.comment9() ) );

    onModified();

    return google::protobuf::Empty();
}


HANDLER_RESULT<types::PageSettings> API_HANDLER_EDITOR::handleGetPageSettings(
        const HANDLER_CONTEXT<commands::GetPageSettings>& aCtx)
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    std::optional<PAGE_INFO> optPageInfo = getPageSettings( aCtx.Request.document() );

    if( !optPageInfo )
    {
        ApiResponseStatus e;
        e.set_status( AS_BAD_REQUEST );
        e.set_error_message( "this editor does not support page settings" );
        return tl::unexpected( e );
    }

    PAGE_INFO& pageInfo = *optPageInfo;

    types::PageSettings response;
    response.set_page_size( ToProtoEnum<PAGE_SIZE_TYPE, types::PageSize>( pageInfo.GetType() ) );

    if( pageInfo.IsCustom() )
        PackVector2( *response.mutable_user_page_size(), pageInfo.GetSizeIU( pcbIUScale.IU_PER_MILS ) );

    response.set_orientation( pageInfo.IsPortrait() ? types::PageOrientation::PO_PORTRAIT
                                                    : types::PageOrientation::PO_LANDSCAPE );
    response.set_drawing_sheet( getDrawingSheetFileName().ToUTF8() );

    return response;
}


HANDLER_RESULT<types::PageSettings> API_HANDLER_EDITOR::handleSetPageSettings(
        const HANDLER_CONTEXT<commands::SetPageSettings>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( !aCtx.Request.has_page_settings() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "SetPageSettings requires page_settings" );
        return tl::unexpected( e );
    }

    std::optional<PAGE_INFO> optPageInfo = getPageSettings( aCtx.Request.document() );

    if( !optPageInfo )
    {
        ApiResponseStatus e;
        e.set_status( AS_BAD_REQUEST );
        e.set_error_message( "this editor does not support page settings" );
        return tl::unexpected( e );
    }

    const types::PageSettings& request = aCtx.Request.page_settings();

    PAGE_INFO pageInfo = *optPageInfo;
    PAGE_SIZE_TYPE pageSizeType = FromProtoEnum<PAGE_SIZE_TYPE>( request.page_size() );

    if( pageSizeType == PAGE_SIZE_TYPE::User )
    {
        if( !request.has_user_page_size() )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( "custom page size requires user_page_size" );
            return tl::unexpected( e );
        }

        VECTOR2D sizeIu = UnpackVector2( request.user_page_size() );
        PAGE_INFO::SetCustomWidthMils( pcbIUScale.IUToMils( sizeIu.x ) );
        PAGE_INFO::SetCustomHeightMils( pcbIUScale.IUToMils( sizeIu.y ) );

        pageInfo.SetType( PAGE_SIZE_TYPE::User );
    }
    else
    {
        bool portrait = ( request.orientation() == types::PageOrientation::PO_PORTRAIT );
        pageInfo.SetType( pageSizeType, portrait );
    }

    if( !setPageSettings( aCtx.Request.document(), pageInfo ) )
    {
        ApiResponseStatus e;
        e.set_status( AS_BAD_REQUEST );
        e.set_error_message( "this editor does not support page settings" );
        return tl::unexpected( e );
    }

    wxString drawingSheet( wxString::FromUTF8( request.drawing_sheet() ) );
    setDrawingSheetFileName( drawingSheet );

    onModified();

    types::PageSettings response;
    response.set_page_size( ToProtoEnum<PAGE_SIZE_TYPE, types::PageSize>( pageInfo.GetType() ) );

    if( pageInfo.IsCustom() )
        PackVector2( *response.mutable_user_page_size(), pageInfo.GetSizeIU( pcbIUScale.IU_PER_MILS ) );

    response.set_orientation( pageInfo.IsPortrait() ? types::PageOrientation::PO_PORTRAIT
                                                    : types::PageOrientation::PO_LANDSCAPE );
    response.set_drawing_sheet( getDrawingSheetFileName().ToUTF8() );

    return response;
}
