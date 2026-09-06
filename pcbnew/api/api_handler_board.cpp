
/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2023 Jon Evans <jon@craftyjon.com>
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

#include <algorithm>
#include <set>
#include <fmt/ranges.h>
#include <magic_enum.hpp>

#include <common.h>
#include <api/api_handler_board.h>
#include <api/api_pcb_utils.h>
#include <api/api_enums.h>
#include <api/api_utils.h>
#include <board_commit.h>
#include <board_design_settings.h>
#include <footprint.h>
#include <kicad_clipboard.h>
#include <pad.h>
#include <pcb_base_edit_frame.h>
#include <pcb_field.h>
#include <pcb_group.h>
#include <pcb_track.h>
#include <pcb_table.h>
#include <pcb_tablecell.h>

#include <layer_ids.h>
#include <project.h>
#include <tool/tool_manager.h>
#include <tools/global_edit_tool.h>
#include <tools/pcb_actions.h>
#include <tools/pcb_selection_tool.h>
#include <tools/zone_filler_tool.h>
#include <widgets/appearance_controls.h>

#include <api/common/types/base_types.pb.h>

using namespace kiapi::common::commands;
using types::CommandStatus;

using types::DocumentType;
using types::ItemRequestStatus;


API_HANDLER_BOARD::API_HANDLER_BOARD( std::shared_ptr<BOARD_CONTEXT> aContext,
                                      EDA_BASE_FRAME* aFrame ) :
        API_HANDLER_EDITOR( aFrame ),
        m_context( std::move( aContext ) )
{
    wxCHECK( m_context, /* void */ );

    registerHandler<GetItemsById, GetItemsResponse>( &API_HANDLER_BOARD::handleGetItemsById );

    registerHandler<GetSelection, SelectionResponse>( &API_HANDLER_BOARD::handleGetSelection, HANDLER_MODE::GUI_ONLY );
    registerHandler<ClearSelection, Empty>( &API_HANDLER_BOARD::handleClearSelection, HANDLER_MODE::GUI_ONLY );
    registerHandler<AddToSelection, SelectionResponse>(
            &API_HANDLER_BOARD::handleAddToSelection, HANDLER_MODE::GUI_ONLY );
    registerHandler<RemoveFromSelection, SelectionResponse>(
            &API_HANDLER_BOARD::handleRemoveFromSelection, HANDLER_MODE::GUI_ONLY );

    registerHandler<GetBoardStackup, BoardStackupResponse>( &API_HANDLER_BOARD::handleGetStackup );
    registerHandler<GetBoardEnabledLayers, BoardEnabledLayersResponse>(
        &API_HANDLER_BOARD::handleGetBoardEnabledLayers );
    registerHandler<GetGraphicsDefaults, GraphicsDefaultsResponse>(
            &API_HANDLER_BOARD::handleGetGraphicsDefaults );
    registerHandler<GetBoundingBox, GetBoundingBoxResponse>( &API_HANDLER_BOARD::handleGetBoundingBox );
    registerHandler<GetPadShapeAsPolygon, PadShapeAsPolygonResponse>(
            &API_HANDLER_BOARD::handleGetPadShapeAsPolygon );
    registerHandler<CheckPadstackPresenceOnLayers, PadstackPresenceResponse>(
            &API_HANDLER_BOARD::handleCheckPadstackPresenceOnLayers );
    registerHandler<ExpandTextVariables, ExpandTextVariablesResponse>(
            &API_HANDLER_BOARD::handleExpandTextVariables );

    registerHandler<InteractiveMoveItems, Empty>(
            &API_HANDLER_BOARD::handleInteractiveMoveItems, HANDLER_MODE::GUI_ONLY );
    registerHandler<FlipItems, FlipItemsResponse>( &API_HANDLER_BOARD::handleFlipItems );

    registerHandler<SaveDocumentToString, SavedDocumentResponse>(
            &API_HANDLER_BOARD::handleSaveDocumentToString );
    registerHandler<SaveSelectionToString, SavedSelectionResponse>(
            &API_HANDLER_BOARD::handleSaveSelectionToString, HANDLER_MODE::GUI_ONLY );
    registerHandler<SaveItemsToString, SavedSelectionResponse>( &API_HANDLER_BOARD::handleSaveItemsToString );
    registerHandler<ParseAndCreateItemsFromString, CreateItemsResponse>(
            &API_HANDLER_BOARD::handleParseAndCreateItemsFromString );
    registerHandler<GetVisibleLayers, BoardLayers>(
            &API_HANDLER_BOARD::handleGetVisibleLayers, HANDLER_MODE::GUI_ONLY );
    registerHandler<SetVisibleLayers, Empty>( &API_HANDLER_BOARD::handleSetVisibleLayers, HANDLER_MODE::GUI_ONLY );
    registerHandler<GetActiveLayer, BoardLayerResponse>(
            &API_HANDLER_BOARD::handleGetActiveLayer, HANDLER_MODE::GUI_ONLY );
    registerHandler<SetActiveLayer, Empty>( &API_HANDLER_BOARD::handleSetActiveLayer, HANDLER_MODE::GUI_ONLY );
}


std::optional<ApiResponseStatus> API_HANDLER_BOARD::checkForHeadless(
        const std::string& aCommandName ) const
{
    if( m_frame )
        return std::nullopt;

    ApiResponseStatus e;
    e.set_status( ApiStatusCode::AS_UNIMPLEMENTED );
    e.set_error_message( fmt::format( "{} is not available in headless mode", aCommandName ) );
    return e;
}


BOARD_ITEM_CONTAINER* API_HANDLER_BOARD::getDefaultContainer()
{
    return board();
}


void API_HANDLER_BOARD::pushCurrentCommit( const std::string& aClientName,
                                            const wxString& aMessage )
{
    API_HANDLER_EDITOR::pushCurrentCommit( aClientName, aMessage );

    if( m_frame )
        m_frame->Refresh();
}


std::unique_ptr<COMMIT> API_HANDLER_BOARD::createCommit()
{
    if( m_frame )
        return std::make_unique<BOARD_COMMIT>( static_cast<EDA_DRAW_FRAME*>( m_frame ) );

    return std::make_unique<BOARD_COMMIT>( toolManager(), true, false );
}


std::optional<BOARD_ITEM*> API_HANDLER_BOARD::getItemById( const KIID& aId ) const
{
    BOARD_ITEM* item = board()->ResolveItem( aId, true );

    if( !item )
        return std::nullopt;

    return item;
}


HANDLER_RESULT<std::unique_ptr<BOARD_ITEM>> API_HANDLER_BOARD::createItemForType( KICAD_T aType,
        BOARD_ITEM_CONTAINER* aContainer )
{
    if( !aContainer )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "Tried to create an item in a null container" );
        return tl::unexpected( e );
    }

    if( aType == PCB_PAD_T && !dynamic_cast<FOOTPRINT*>( aContainer ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "Tried to create a pad in {}, which is not a footprint",
                                          aContainer->GetFriendlyName().ToStdString() ) );
        return tl::unexpected( e );
    }
    else if( aType == PCB_FOOTPRINT_T && !dynamic_cast<BOARD*>( aContainer ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "Tried to create a footprint in {}, which is not a board",
                                          aContainer->GetFriendlyName().ToStdString() ) );
        return tl::unexpected( e );
    }

    std::unique_ptr<BOARD_ITEM> created = CreateItemForType( aType, aContainer );

    if( !created )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "Tried to create an item of type {}, which is unhandled",
                                          magic_enum::enum_name( aType ) ) );
        return tl::unexpected( e );
    }

    return created;
}


void API_HANDLER_BOARD::deleteItemsInternal( std::map<KIID, ItemDeletionStatus>& aItemsToDelete,
                                              const std::string& aClientName )
{
    BOARD* board = this->board();
    std::vector<BOARD_ITEM*> validatedItems;

    for( std::pair<const KIID, ItemDeletionStatus> pair : aItemsToDelete )
    {
        if( BOARD_ITEM* item = board->ResolveItem( pair.first, true ) )
        {
            // A footprint without its mandatory fields is not a state the editor can load or
            // render; the const field accessors return nullptr and callers dereference them
            if( item->Type() == PCB_FIELD_T && static_cast<PCB_FIELD*>( item )->IsMandatory() )
            {
                aItemsToDelete[pair.first] = ItemDeletionStatus::IDS_IMMUTABLE;
                continue;
            }

            validatedItems.push_back( item );
            aItemsToDelete[pair.first] = ItemDeletionStatus::IDS_OK;
        }

        // Note: we don't currently support locking items from API modification, but here is where
        // to add it in the future (and return IDS_IMMUTABLE)
    }

    COMMIT* commit = getCurrentCommit( aClientName );

    for( BOARD_ITEM* item : validatedItems )
        commit->Remove( item );

    if( !m_activeClients.count( aClientName ) )
        pushImplicitCommit( aClientName, _( "Deleted items via API" ) );
}


std::optional<EDA_ITEM*> API_HANDLER_BOARD::getItemFromDocument(
        const DocumentSpecifier& aDocument, const KIID& aId )
{
    if( !validateDocument( aDocument ) )
        return std::nullopt;

    return getItemById( aId );
}


HANDLER_RESULT<ItemRequestStatus> API_HANDLER_BOARD::handleCreateUpdateItemsInternal( bool aCreate,
        const std::string& aClientName,
        const types::ItemHeader &aHeader,
        const google::protobuf::RepeatedPtrField<google::protobuf::Any>& aItems,
        std::function<void( ItemStatus, google::protobuf::Any )> aItemHandler )
{
    ApiResponseStatus e;

    auto containerResult = validateItemHeaderDocument( aHeader );

    if( !containerResult && containerResult.error().status() == ApiStatusCode::AS_UNHANDLED )
    {
        // No message needed for AS_UNHANDLED; this is an internal flag for the API server
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }
    else if( !containerResult )
    {
        e.CopyFrom( containerResult.error() );
        return tl::unexpected( e );
    }

    BOARD* board = this->board();
    BOARD_ITEM_CONTAINER* container = getDefaultContainer();

    if( containerResult->has_value() )
    {
        const KIID& containerId = **containerResult;
        std::optional<BOARD_ITEM*> optItem = getItemById( containerId );

        if( optItem )
        {
            container = dynamic_cast<BOARD_ITEM_CONTAINER*>( *optItem );

            if( !container )
            {
                e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                e.set_error_message( fmt::format(
                        "The requested container {} is not a valid board item container",
                        containerId.AsStdString() ) );
                return tl::unexpected( e );
            }
        }
        else
        {
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( fmt::format(
                    "The requested container {} does not exist in this document",
                    containerId.AsStdString() ) );
            return tl::unexpected( e );
        }
    }

    BOARD_COMMIT* commit = static_cast<BOARD_COMMIT*>( getCurrentCommit( aClientName ) );

    for( const google::protobuf::Any& anyItem : aItems )
    {
        ItemStatus status;
        std::optional<KICAD_T> type = TypeNameFromAny( anyItem );

        if( !type )
        {
            status.set_code( ItemStatusCode::ISC_INVALID_TYPE );
            status.set_error_message( fmt::format( "Could not decode a valid type from {}",
                                                   anyItem.type_url() ) );
            aItemHandler( status, anyItem );
            continue;
        }

        if( type == PCB_DIMENSION_T )
        {
            board::types::Dimension dimension;
            anyItem.UnpackTo( &dimension );

            switch( dimension.dimension_style_case() )
            {
            case board::types::Dimension::kAligned:    type = PCB_DIM_ALIGNED_T;    break;
            case board::types::Dimension::kOrthogonal: type = PCB_DIM_ORTHOGONAL_T; break;
            case board::types::Dimension::kRadial:     type = PCB_DIM_RADIAL_T;     break;
            case board::types::Dimension::kLeader:     type = PCB_DIM_LEADER_T;     break;
            case board::types::Dimension::kCenter:     type = PCB_DIM_CENTER_T;     break;
            case board::types::Dimension::DIMENSION_STYLE_NOT_SET: break;
            }
        }

        HANDLER_RESULT<std::unique_ptr<BOARD_ITEM>> creationResult =
                createItemForType( *type, container );

        if( !creationResult )
        {
            status.set_code( ItemStatusCode::ISC_INVALID_TYPE );
            status.set_error_message( creationResult.error().error_message() );
            aItemHandler( status, anyItem );
            continue;
        }

        std::unique_ptr<BOARD_ITEM> item( std::move( *creationResult ) );

        if( !item->Deserialize( anyItem ) )
        {
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( fmt::format( "could not unpack {} from request",
                                              item->GetClass().ToStdString() ) );
            return tl::unexpected( e );
        }

        if( std::vector<wxString> removed = item->RemoveConflictingCustomProperties(); !removed.empty() )
        {
            auto as_str =
                []( const wxString& aIn )
                {
                    return std::string( aIn.ToUTF8() );
                };

            status.set_code( ItemStatusCode::ISC_INVALID_DATA );
            status.set_error_message( fmt::format(
                    "Invalid custom properties for item {}: property name(s) '{}' already in use",
                    item->m_Uuid.AsStdString(), fmt::join( std::views::transform( removed, as_str ), ", " ) ) );

            aItemHandler( status, anyItem );
            continue;
        }

        std::optional<BOARD_ITEM*> optItem = getItemById( item->m_Uuid );

        if( aCreate && optItem )
        {
            status.set_code( ItemStatusCode::ISC_EXISTING );
            status.set_error_message( fmt::format( "an item with UUID {} already exists",
                                                   item->m_Uuid.AsStdString() ) );
            aItemHandler( status, anyItem );
            continue;
        }
        else if( !aCreate && !optItem )
        {
            status.set_code( ItemStatusCode::ISC_NONEXISTENT );
            status.set_error_message( fmt::format( "an item with UUID {} does not exist",
                                                   item->m_Uuid.AsStdString() ) );
            aItemHandler( status, anyItem );
            continue;
        }

        if( aCreate
            && !item->FitsEnabledLayers( board->GetEnabledLayers(), board->GetCopperLayerCount() ) )
        {
            status.set_code( ItemStatusCode::ISC_INVALID_DATA );
            status.set_error_message(
                "attempted to add item with no overlapping layers with the board" );
            aItemHandler( status, anyItem );
            continue;
        }

        status.set_code( ItemStatusCode::ISC_OK );
        google::protobuf::Any newItem;

        if( aCreate )
        {
            if( item->Type() == PCB_TABLECELL_T )
            {
                PCB_TABLE* table = dynamic_cast<PCB_TABLE*>( container );

                if( !table )
                {
                    status.set_code( ItemStatusCode::ISC_INVALID_DATA );
                    status.set_error_message( "a table cell must target a table container" );
                    aItemHandler( status, anyItem );
                    continue;
                }

                PCB_TABLECELL* cell = static_cast<PCB_TABLECELL*>( item.release() );
                commit->Modify( table );
                table->AddCell( cell );
                cell->Serialize( newItem );
            }
            else
            {
                if( item->Type() == PCB_FOOTPRINT_T || item->Type() == PCB_TABLE_T )
                {
                    // Ensure children have unique identifiers; in case the API client created
                    // this new item by cloning an existing one and only changing the parent UUID.
                    item->RunOnChildren(
                            []( BOARD_ITEM* aChild )
                            {
                                aChild->ResetUuid();
                            },
                            RECURSE );
                }

                item->Serialize( newItem );
                commit->Add( item.release() );
            }
        }
        else
        {
            BOARD_ITEM* boardItem = *optItem;

            // Footprints can't be modified by CopyFrom at the moment because the commit system
            // doesn't currently know what to do with a footprint that has had its children
            // replaced with other children; which results in things like the view not having its
            // cached geometry for footprint children updated when you move a footprint around.
            // And also, groups are special because they can contain any item type, so we
            // can't use CopyFrom on them either.
            if( boardItem->Type() == PCB_FOOTPRINT_T  || boardItem->Type() == PCB_GROUP_T )
            {
                // Save group membership before removal, since Remove() severs the relationship
                PCB_GROUP* parentGroup = dynamic_cast<PCB_GROUP*>( boardItem->GetParentGroup() );

                commit->Remove( boardItem );
                item->Serialize( newItem );

                BOARD_ITEM* newBoardItem = item.release();
                commit->Add( newBoardItem );

                // Restore group membership for the newly added item
                if( parentGroup )
                    parentGroup->AddItem( newBoardItem );
            }
            else
            {
                commit->Modify( boardItem );
                boardItem->CopyFrom( item.get() );
                boardItem->Serialize( newItem );
            }
        }

        aItemHandler( status, newItem );
    }

    if( !m_activeClients.count( aClientName ) )
    {
        pushImplicitCommit( aClientName, aCreate ? _( "Created items via API" )
                                                : _( "Modified items via API" ) );
    }


    return ItemRequestStatus::IRS_OK;
}


std::vector<KICAD_T> API_HANDLER_BOARD::parseRequestedItemTypes(
        const google::protobuf::RepeatedField<int>& aTypes )
{
    std::vector<KICAD_T> types;

    for( int typeRaw : aTypes )
    {
        auto typeMessage = static_cast<common::types::KiCadObjectType>( typeRaw );
        KICAD_T type = FromProtoEnum<KICAD_T>( typeMessage );

        if( type != TYPE_NOT_INIT )
            types.emplace_back( type );
    }

    return types;
}


const std::set<std::string>& API_HANDLER_BOARD::headlessActions() const
{
    // Actions whose tools have a path that neither opens a dialog nor needs a canvas; see
    // ensureHeadlessTools for the tools behind them
    static const std::set<std::string> actions = {
        PCB_ACTIONS::zoneFillAll.GetName(),
        PCB_ACTIONS::zoneUnfillAll.GetName(),
        PCB_ACTIONS::cleanupTracksAndVias.GetName(),
        PCB_ACTIONS::cleanupGraphics.GetName(),
    };

    return actions;
}


void API_HANDLER_BOARD::ensureHeadlessTools()
{
    if( m_frame )
        return;

    TOOL_MANAGER* mgr = toolManager();
    bool          added = false;

    if( !mgr->FindTool( ZONE_FILLER_TOOL_NAME ) )
    {
        mgr->RegisterTool( new ZONE_FILLER_TOOL );
        added = true;
    }

    if( !mgr->FindTool( "pcbnew.GlobalEdit" ) )
    {
        mgr->RegisterTool( new GLOBAL_EDIT_TOOL );
        added = true;
    }

    // Tools only get their event transitions from a reset; RefillZones may have registered the
    // zone filler without one (it calls the tool directly), so reset whenever the set grew.
    // InitTools is not usable here: PCB_TOOL_BASE::Init builds the context menu on the frame.
    if( added || !m_headlessToolsInitialized )
    {
        mgr->ResetTools( TOOL_BASE::RUN );
        m_headlessToolsInitialized = true;
    }
}


HANDLER_RESULT<GetItemsResponse> API_HANDLER_BOARD::handleGetItemsById(
        const HANDLER_CONTEXT<GetItemsById>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    // validateItemHeaderDocument already answers AS_UNHANDLED for another editor's
    // document type; anything else it reports is a real error for this client
    if( HANDLER_RESULT<std::optional<KIID>> header = validateItemHeaderDocument( aCtx.Request.header() );
        !header )
    {
        return tl::unexpected( header.error() );
    }

    GetItemsResponse response;

    std::vector<BOARD_ITEM*> items;

    for( const kiapi::common::types::KIID& id : aCtx.Request.items() )
    {
        if( std::optional<BOARD_ITEM*> item = getItemById( KIID( id.value() ) ) )
            items.emplace_back( *item );
    }

    if( items.empty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "none of the requested IDs were found or valid" );
        return tl::unexpected( e );
    }

    for( const BOARD_ITEM* item : items )
    {
        google::protobuf::Any itemBuf;
        item->Serialize( itemBuf );
        response.mutable_items()->Add( std::move( itemBuf ) );
    }

    response.set_status( ItemRequestStatus::IRS_OK );
    return response;
}


HANDLER_RESULT<SelectionResponse> API_HANDLER_BOARD::handleGetSelection(
            const HANDLER_CONTEXT<GetSelection>& aCtx )
{
    if( std::optional<ApiResponseStatus> headless = checkForHeadless( "GetSelection" ) )
        return tl::unexpected( *headless );

    // validateItemHeaderDocument already answers AS_UNHANDLED for another editor's
    // document type; anything else it reports is a real error for this client
    if( HANDLER_RESULT<std::optional<KIID>> header = validateItemHeaderDocument( aCtx.Request.header() );
        !header )
    {
        return tl::unexpected( header.error() );
    }

    std::set<KICAD_T> filter;

    for( KICAD_T type : parseRequestedItemTypes( aCtx.Request.types() ) )
        filter.insert( type );

    TOOL_MANAGER* mgr = toolManager();
    PCB_SELECTION_TOOL* selectionTool = mgr->GetTool<PCB_SELECTION_TOOL>();

    SelectionResponse response;

    for( EDA_ITEM* item : selectionTool->GetSelection() )
    {
        if( filter.empty() || filter.contains( item->Type() ) )
            item->Serialize( *response.add_items() );
    }

    return response;
}


HANDLER_RESULT<Empty> API_HANDLER_BOARD::handleClearSelection(
        const HANDLER_CONTEXT<ClearSelection>& aCtx )
{
    if( std::optional<ApiResponseStatus> headless = checkForHeadless( "ClearSelection" ) )
        return tl::unexpected( *headless );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    // validateItemHeaderDocument already answers AS_UNHANDLED for another editor's
    // document type; anything else it reports is a real error for this client
    if( HANDLER_RESULT<std::optional<KIID>> header = validateItemHeaderDocument( aCtx.Request.header() );
        !header )
    {
        return tl::unexpected( header.error() );
    }

    TOOL_MANAGER* mgr = toolManager();
    mgr->RunAction( ACTIONS::selectionClear );
    m_frame->Refresh();

    return Empty();
}


HANDLER_RESULT<SelectionResponse> API_HANDLER_BOARD::handleAddToSelection(
        const HANDLER_CONTEXT<AddToSelection>& aCtx )
{
    if( std::optional<ApiResponseStatus> headless = checkForHeadless( "AddToSelection" ) )
        return tl::unexpected( *headless );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    // validateItemHeaderDocument already answers AS_UNHANDLED for another editor's
    // document type; anything else it reports is a real error for this client
    if( HANDLER_RESULT<std::optional<KIID>> header = validateItemHeaderDocument( aCtx.Request.header() );
        !header )
    {
        return tl::unexpected( header.error() );
    }

    TOOL_MANAGER* mgr = toolManager();
    PCB_SELECTION_TOOL* selectionTool = mgr->GetTool<PCB_SELECTION_TOOL>();

    std::vector<EDA_ITEM*> toAdd;

    for( const types::KIID& id : aCtx.Request.items() )
    {
        if( std::optional<BOARD_ITEM*> item = getItemById( KIID( id.value() ) ) )
            toAdd.emplace_back( *item );
    }

    selectionTool->AddItemsToSel( &toAdd );
    m_frame->Refresh();

    SelectionResponse response;

    for( EDA_ITEM* item : selectionTool->GetSelection() )
        item->Serialize( *response.add_items() );

    return response;
}


HANDLER_RESULT<SelectionResponse> API_HANDLER_BOARD::handleRemoveFromSelection(
        const HANDLER_CONTEXT<RemoveFromSelection>& aCtx )
{
    if( std::optional<ApiResponseStatus> headless = checkForHeadless( "RemoveFromSelection" ) )
        return tl::unexpected( *headless );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    // validateItemHeaderDocument already answers AS_UNHANDLED for another editor's
    // document type; anything else it reports is a real error for this client
    if( HANDLER_RESULT<std::optional<KIID>> header = validateItemHeaderDocument( aCtx.Request.header() );
        !header )
    {
        return tl::unexpected( header.error() );
    }

    TOOL_MANAGER* mgr = toolManager();
    PCB_SELECTION_TOOL* selectionTool = mgr->GetTool<PCB_SELECTION_TOOL>();

    std::vector<EDA_ITEM*> toRemove;

    for( const types::KIID& id : aCtx.Request.items() )
    {
        if( std::optional<BOARD_ITEM*> item = getItemById( KIID( id.value() ) ) )
            toRemove.emplace_back( *item );
    }

    selectionTool->RemoveItemsFromSel( &toRemove );
    m_frame->Refresh();

    SelectionResponse response;

    for( EDA_ITEM* item : selectionTool->GetSelection() )
        item->Serialize( *response.add_items() );

    return response;
}


HANDLER_RESULT<BoardStackupResponse> API_HANDLER_BOARD::handleGetStackup(
        const HANDLER_CONTEXT<GetBoardStackup>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    BoardStackupResponse response;

    board::PackBoardStackup( *board(), *response.mutable_stackup() );

    return response;
}


HANDLER_RESULT<BoardEnabledLayersResponse> API_HANDLER_BOARD::handleGetBoardEnabledLayers(
        const HANDLER_CONTEXT<GetBoardEnabledLayers>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    BoardEnabledLayersResponse response;

    BOARD* board = this->board();
    int copperLayerCount = board->GetCopperLayerCount();

    response.set_copper_layer_count( copperLayerCount );

    LSET enabled = board->GetEnabledLayers();

    // The Rescue layer is an internal detail and should be hidden from the API
    enabled.reset( Rescue );

    // Just in case this is out of sync; the API should always return the expected copper layers
    enabled |= LSET::AllCuMask( copperLayerCount );

    board::PackLayerSet( *response.mutable_layers(), enabled );

    return response;
}


HANDLER_RESULT<GraphicsDefaultsResponse> API_HANDLER_BOARD::handleGetGraphicsDefaults(
        const HANDLER_CONTEXT<GetGraphicsDefaults>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    const BOARD_DESIGN_SETTINGS& bds = board()->GetDesignSettings();
    GraphicsDefaultsResponse response;

    // TODO: This should change to be an enum class
    constexpr std::array<kiapi::board::BoardLayerClass, LAYER_CLASS_COUNT> classOrder = {
        kiapi::board::BLC_SILKSCREEN,
        kiapi::board::BLC_COPPER,
        kiapi::board::BLC_EDGES,
        kiapi::board::BLC_COURTYARD,
        kiapi::board::BLC_FABRICATION,
        kiapi::board::BLC_OTHER
    };

    for( int i = 0; i < LAYER_CLASS_COUNT; ++i )
    {
        kiapi::board::BoardLayerGraphicsDefaults* l = response.mutable_defaults()->add_layers();

        l->set_layer( classOrder[i] );
        l->mutable_line_thickness()->set_value_nm( bds.m_LineThickness[i] );

        kiapi::common::types::TextAttributes* text = l->mutable_text();
        text->mutable_size()->set_x_nm( bds.m_TextSize[i].x );
        text->mutable_size()->set_y_nm( bds.m_TextSize[i].y );
        text->mutable_stroke_width()->set_value_nm( bds.m_TextThickness[i] );
        text->set_italic( bds.m_TextItalic[i] );
        text->set_keep_upright( bds.m_TextUpright[i] );
    }

    return response;
}


HANDLER_RESULT<GetBoundingBoxResponse> API_HANDLER_BOARD::handleGetBoundingBox(
        const HANDLER_CONTEXT<GetBoundingBox>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    // validateItemHeaderDocument already answers AS_UNHANDLED for another editor's
    // document type; anything else it reports is a real error for this client
    if( HANDLER_RESULT<std::optional<KIID>> header = validateItemHeaderDocument( aCtx.Request.header() );
        !header )
    {
        return tl::unexpected( header.error() );
    }

    GetBoundingBoxResponse response;
    bool includeText = aCtx.Request.mode() == BoundingBoxMode::BBM_ITEM_AND_CHILD_TEXT;

    for( const types::KIID& idMsg : aCtx.Request.items() )
    {
        KIID id( idMsg.value() );
        std::optional<BOARD_ITEM*> optItem = getItemById( id );

        if( !optItem )
            continue;

        BOARD_ITEM* item = *optItem;
        BOX2I bbox;

        if( item->Type() == PCB_FOOTPRINT_T )
            bbox = static_cast<FOOTPRINT*>( item )->GetBoundingBox( includeText );
        else
            bbox = item->GetBoundingBox();

        response.add_items()->set_value( idMsg.value() );
        PackBox2( *response.add_boxes(), bbox );
    }

    return response;
}


HANDLER_RESULT<PadShapeAsPolygonResponse> API_HANDLER_BOARD::handleGetPadShapeAsPolygon(
        const HANDLER_CONTEXT<GetPadShapeAsPolygon>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );
        !documentValidation )
    {
        return tl::unexpected( documentValidation.error() );
    }

    PadShapeAsPolygonResponse response;
    PCB_LAYER_ID layer = FromProtoEnum<PCB_LAYER_ID, board::types::BoardLayer>( aCtx.Request.layer() );

    for( const types::KIID& padRequest : aCtx.Request.pads() )
    {
        KIID id( padRequest.value() );
        std::optional<BOARD_ITEM*> optPad = getItemById( id );

        if( !optPad || ( *optPad )->Type() != PCB_PAD_T )
            continue;

        response.add_pads()->set_value( padRequest.value() );

        PAD* pad = static_cast<PAD*>( *optPad );
        SHAPE_POLY_SET poly;
        pad->TransformShapeToPolygon( poly, pad->Padstack().EffectiveLayerFor( layer ), 0,
                                      pad->GetMaxError(), ERROR_INSIDE );

        types::PolygonWithHoles* polyMsg = response.mutable_polygons()->Add();
        PackPolyLine( *polyMsg->mutable_outline(), poly.COutline( 0 ) );
    }

    return response;
}


HANDLER_RESULT<PadstackPresenceResponse> API_HANDLER_BOARD::handleCheckPadstackPresenceOnLayers(
        const HANDLER_CONTEXT<CheckPadstackPresenceOnLayers>& aCtx )
{
    using board::types::BoardLayer;

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );
        !documentValidation )
    {
        return tl::unexpected( documentValidation.error() );
    }

    PadstackPresenceResponse response;

    LSET layers;

    for( const int layer : aCtx.Request.layers() )
        layers.set( FromProtoEnum<PCB_LAYER_ID, BoardLayer>( static_cast<BoardLayer>( layer ) ) );

    for( const types::KIID& padRequest : aCtx.Request.items() )
    {
        KIID id( padRequest.value() );
        std::optional<BOARD_ITEM*> optItem = getItemById( id );

        if( !optItem )
            continue;

        switch( ( *optItem )->Type() )
        {
        case PCB_PAD_T:
        {
            PAD* pad = static_cast<PAD*>( *optItem );

            for( PCB_LAYER_ID layer : layers )
            {
                PadstackPresenceEntry* entry = response.add_entries();
                entry->mutable_item()->set_value( pad->m_Uuid.AsStdString() );
                entry->set_layer( ToProtoEnum<PCB_LAYER_ID, BoardLayer>( layer ) );
                entry->set_presence( pad->FlashLayer( layer ) ? PSP_PRESENT : PSP_NOT_PRESENT );
            }

            break;
        }

        case PCB_VIA_T:
        {
            PCB_VIA* via = static_cast<PCB_VIA*>( *optItem );

            for( PCB_LAYER_ID layer : layers )
            {
                PadstackPresenceEntry* entry = response.add_entries();
                entry->mutable_item()->set_value( via->m_Uuid.AsStdString() );
                entry->set_layer( ToProtoEnum<PCB_LAYER_ID, BoardLayer>( layer ) );
                entry->set_presence( via->FlashLayer( layer ) ? PSP_PRESENT : PSP_NOT_PRESENT );
            }

            break;
        }

        default:
            break;
        }
    }

    return response;
}


HANDLER_RESULT<ExpandTextVariablesResponse> API_HANDLER_BOARD::handleExpandTextVariables(
    const HANDLER_CONTEXT<ExpandTextVariables>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    ExpandTextVariablesResponse reply;
    BOARD* board = this->board();

    std::function<bool( wxString* )> textResolver =
            [&]( wxString* token ) -> bool
            {
                // Handles m_board->GetTitleBlock() *and* m_board->GetProject()
                return board->ResolveTextVar( token, 0 );
            };

    for( const std::string& textMsg : aCtx.Request.text() )
    {
        wxString text = ExpandTextVars( wxString::FromUTF8( textMsg ), &textResolver );

        if( aCtx.Request.expand_env_vars() )
            text = ExpandEnvVarSubstitutions( text, board->GetProject() );

        reply.add_text( text.ToUTF8() );
    }

    return reply;
}


HANDLER_RESULT<Empty> API_HANDLER_BOARD::handleInteractiveMoveItems(
        const HANDLER_CONTEXT<InteractiveMoveItems>& aCtx )
{
    if( std::optional<ApiResponseStatus> headless = checkForHeadless( "InteractiveMoveItems" ) )
        return tl::unexpected( *headless );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    TOOL_MANAGER* mgr = toolManager();
    std::vector<EDA_ITEM*> toSelect;

    for( const kiapi::common::types::KIID& id : aCtx.Request.items() )
    {
        if( std::optional<BOARD_ITEM*> item = getItemById( KIID( id.value() ) ) )
            toSelect.emplace_back( static_cast<EDA_ITEM*>( *item ) );
    }

    if( toSelect.empty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "None of the given items exist on the board",
                                          aCtx.Request.board().board_filename() ) );
        return tl::unexpected( e );
    }

    PCB_SELECTION_TOOL* selectionTool = mgr->GetTool<PCB_SELECTION_TOOL>();
    selectionTool->GetSelection().SetReferencePoint( toSelect[0]->GetPosition() );

    mgr->RunAction( ACTIONS::selectionClear );
    mgr->RunAction<EDA_ITEMS*>( ACTIONS::selectItems, &toSelect );

    COMMIT* commit = getCurrentCommit( aCtx.ClientName );
    mgr->PostAPIAction( PCB_ACTIONS::move, commit );

    return Empty();
}


HANDLER_RESULT<FlipItemsResponse> API_HANDLER_BOARD::handleFlipItems(
        const HANDLER_CONTEXT<FlipItems>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    auto containerResult = validateItemHeaderDocument( aCtx.Request.header() );

    if( !containerResult && containerResult.error().status() == ApiStatusCode::AS_UNHANDLED )
    {
        // No message needed for AS_UNHANDLED; this is an internal flag for the API server
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }
    else if( !containerResult )
    {
        return tl::unexpected( containerResult.error() );
    }

    FLIP_DIRECTION flipDirection = FromProtoEnum<FLIP_DIRECTION, BoardFlipDirection>(
            aCtx.Request.direction() );

    FlipItemsResponse response;
    response.mutable_header()->CopyFrom( aCtx.Request.header() );

    BOARD_COMMIT* commit = static_cast<BOARD_COMMIT*>( getCurrentCommit( aCtx.ClientName ) );

    bool anyModified = false;

    for( const types::KIID& id : aCtx.Request.items() )
    {
        ItemFlipResult* result = response.add_flipped_items();

        std::optional<BOARD_ITEM*> optItem = getItemById( KIID( id.value() ) );

        if( !optItem )
        {
            result->mutable_status()->set_code( ItemStatusCode::ISC_NONEXISTENT );
            result->mutable_status()->set_error_message(
                    fmt::format( "an item with UUID {} does not exist", id.value() ) );
            continue;
        }

        BOARD_ITEM* boardItem = *optItem;

        static const std::set<KICAD_T> flippableTypes = {
            PCB_FOOTPRINT_T,
            PCB_PAD_T,
            PCB_SHAPE_T,
            PCB_REFERENCE_IMAGE_T,
            PCB_FIELD_T,
            PCB_GENERATOR_T,
            PCB_TEXT_T,
            PCB_TEXTBOX_T,
            PCB_TABLE_T,
            PCB_TRACE_T,
            PCB_VIA_T,
            PCB_ARC_T,
            PCB_ZONE_T,
            PCB_GROUP_T,
            PCB_BARCODE_T,
            PCB_GRIDITEM_T,
            PCB_MARKER_T,
            PCB_POINT_T,
            PCB_TARGET_T,
            PCB_DIM_ALIGNED_T,
            PCB_DIM_LEADER_T,
            PCB_DIM_CENTER_T,
            PCB_DIM_RADIAL_T,
            PCB_DIM_ORTHOGONAL_T,
        };

        KICAD_T itemType = boardItem->Type();

        if( !flippableTypes.contains( itemType ) )
        {
            result->mutable_status()->set_code( ItemStatusCode::ISC_INVALID_TYPE );
            result->mutable_status()->set_error_message(
                    fmt::format( "items of type {} cannot be flipped",
                                 magic_enum::enum_name( itemType ) ) );
            continue;
        }

        commit->Modify( boardItem, nullptr, RECURSE_MODE::RECURSE );
        boardItem->Flip( boardItem->GetPosition(), flipDirection );
        boardItem->Normalize();

        // Maybe this should be in FOOTPRINT::Normalize?
        if( boardItem->Type() == PCB_FOOTPRINT_T )
            static_cast<FOOTPRINT*>( boardItem )->InvalidateComponentClassCache();

        anyModified = true;

        google::protobuf::Any itemBuf;
        boardItem->Serialize( itemBuf );
        *result->mutable_item() = std::move( itemBuf );

        result->mutable_status()->set_code( ItemStatusCode::ISC_OK );
    }

    response.set_status( ItemRequestStatus::IRS_OK );

    if( anyModified && !m_activeClients.count( aCtx.ClientName ) )
        pushCurrentCommit( aCtx.ClientName, _( "Flipped items via API" ) );

    return response;
}


HANDLER_RESULT<SavedDocumentResponse> API_HANDLER_BOARD::handleSaveDocumentToString(
        const HANDLER_CONTEXT<SaveDocumentToString>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    SavedDocumentResponse response;
    response.mutable_document()->CopyFrom( aCtx.Request.document() );

    CLIPBOARD_IO io;
    io.SetWriter(
        [&]( const wxString& aData )
        {
            response.set_contents( aData.ToUTF8() );
        } );

    io.SaveBoard( wxEmptyString, *board(), nullptr );

    return response;
}


HANDLER_RESULT<SavedSelectionResponse> API_HANDLER_BOARD::handleSaveSelectionToString(
        const HANDLER_CONTEXT<SaveSelectionToString>& aCtx )
{
    if( std::optional<ApiResponseStatus> headless = checkForHeadless( "SaveSelectionToString" ) )
        return tl::unexpected( *headless );

    SavedSelectionResponse response;

    TOOL_MANAGER* mgr = toolManager();
    PCB_SELECTION_TOOL* selectionTool = mgr->GetTool<PCB_SELECTION_TOOL>();
    PCB_SELECTION& selection = selectionTool->GetSelection();

    CLIPBOARD_IO io;
    io.SetWriter(
        [&]( const wxString& aData )
        {
            response.set_contents( aData.ToUTF8() );
        } );

    io.SetBoard( board() );
    io.SaveSelection( selection, false );

    return response;
}


HANDLER_RESULT<SavedSelectionResponse> API_HANDLER_BOARD::handleSaveItemsToString(
        const HANDLER_CONTEXT<SaveItemsToString>& aCtx )
{
    HANDLER_RESULT<std::optional<KIID>> containerResult = validateItemHeaderDocument( aCtx.Request.header() );

    if( !containerResult )
        return tl::unexpected( containerResult.error() );

    SavedSelectionResponse response;
    PCB_SELECTION          selection;

    for( const types::KIID& id : aCtx.Request.items() )
    {
        std::optional<BOARD_ITEM*> item = getItemById( KIID( id.value() ) );

        if( !item )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( fmt::format( "item {} does not exist in the document", id.value() ) );
            return tl::unexpected( e );
        }

        selection.Add( *item );
        response.add_ids()->set_value( id.value() );
    }

    CLIPBOARD_IO io;
    io.SetWriter(
        [&]( const wxString& aData )
        {
            response.set_contents( aData.ToUTF8() );
        } );

    io.SetBoard( board() );
    io.SaveSelection( selection, thisDocumentType() == types::DOCTYPE_FOOTPRINT );

    return response;
}


void API_HANDLER_BOARD::focusOnItem( const SelectionSpec& aSpec, FocusOnItemResponse& aResponse )
{
    BOARD_ITEM* target = nullptr;
    wxString    reference;

    if( aSpec.has_footprint() )
        reference = wxString::FromUTF8( aSpec.footprint().reference() );
    else if( aSpec.has_pad() )
        reference = wxString::FromUTF8( aSpec.pad().reference() );

    if( FOOTPRINT* footprint = board()->FindFootprintByReference( reference ) )
    {
        if( aSpec.has_pad() )
            target = footprint->FindPadByNumber( wxString::FromUTF8( aSpec.pad().number() ) );
        else
            target = footprint;
    }

    if( !target )
    {
        aResponse.set_status( CrossProbeStatus::CPS_NOT_FOUND );
        aResponse.set_message( "no item matches the given selection spec" );
        return;
    }

    static_cast<PCB_BASE_FRAME*>( m_frame )->FocusOnItem( target );
    aResponse.set_status( CrossProbeStatus::CPS_OK );
}


HANDLER_RESULT<CreateItemsResponse> API_HANDLER_BOARD::handleParseAndCreateItemsFromString(
        const HANDLER_CONTEXT<ParseAndCreateItemsFromString>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( aCtx.Request.contents().empty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "ParseAndCreateItemsFromString requires contents" );
        return tl::unexpected( e );
    }

    // The same parser the Paste action uses: a kicad_pcb container (as written by
    // SaveSelectionToString / SaveItemsToString / SaveDocumentToString) or a single footprint
    wxString contents = wxString::FromUTF8( aCtx.Request.contents() );

    auto parse = [&]( const wxString& aText ) -> std::unique_ptr<BOARD_ITEM>
    {
        CLIPBOARD_IO io;
        io.SetBoard( board() );
        io.SetReader(
                [&]()
                {
                    return aText;
                } );

        return std::unique_ptr<BOARD_ITEM>( io.Parse() );
    };

    std::unique_ptr<BOARD_ITEM> parsed = parse( contents );

    // A container written by hand may omit the version header the parser insists on
    if( !parsed && contents.Trim( false ).StartsWith( wxS( "(kicad_pcb" ) ) && !contents.Contains( wxS( "(version" ) ) )
    {
        wxString withHeader = contents;
        withHeader.Replace( wxS( "(kicad_pcb" ),
                            wxString::Format( wxS( "(kicad_pcb (version %d) (generator \"kicad_api\")" ),
                                              SEXPR_BOARD_FILE_VERSION ),
                            false );
        parsed = parse( withHeader );
    }

    if( !parsed )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "contents could not be parsed as a board or footprint" );
        return tl::unexpected( e );
    }

    std::vector<BOARD_ITEM*> items;

    if( parsed->Type() == PCB_T )
    {
        BOARD* clipBoard = static_cast<BOARD*>( parsed.get() );

        // Groups reference their members by id, so members must be created first
        for( BOARD_ITEM* item : clipBoard->GetItemSet() )
        {
            if( item->Type() != PCB_MARKER_T && item->Type() != PCB_GROUP_T )
                items.push_back( item );
        }

        for( PCB_GROUP* group : clipBoard->Groups() )
            items.push_back( group );
    }
    else
    {
        items.push_back( parsed.get() );
    }

    // Pasting a copy of items that are already on the board must not collide with them; like the
    // Paste action, everything gets new ids in that case (member references follow the pointers,
    // so groups still serialize their members' new ids)
    bool conflict = std::ranges::any_of( items,
                                         [&]( BOARD_ITEM* aItem )
                                         {
                                             return getItemById( aItem->m_Uuid ).has_value();
                                         } );

    if( conflict )
    {
        for( BOARD_ITEM* item : items )
        {
            item->ResetUuidDirect();
            item->RunOnChildren(
                    []( BOARD_ITEM* aChild )
                    {
                        aChild->ResetUuidDirect();
                    },
                    RECURSE_MODE::RECURSE );
        }
    }

    google::protobuf::RepeatedPtrField<google::protobuf::Any> protoItems;

    for( BOARD_ITEM* item : items )
        item->Serialize( *protoItems.Add() );

    // The parsed objects have done their job; the items are created from the messages so that
    // they go through the same validation and commit handling as CreateItems
    parsed.reset();

    types::ItemHeader header;
    *header.mutable_document() = aCtx.Request.document();

    CreateItemsResponse response;

    HANDLER_RESULT<ItemRequestStatus> result = handleCreateUpdateItemsInternal(
            true, aCtx.ClientName, header, protoItems,
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


HANDLER_RESULT<BoardLayers> API_HANDLER_BOARD::handleGetVisibleLayers(
        const HANDLER_CONTEXT<GetVisibleLayers>& aCtx )
{
    if( std::optional<ApiResponseStatus> headless = checkForHeadless( "GetVisibleLayers" ) )
        return tl::unexpected( *headless );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    BoardLayers response;

    for( PCB_LAYER_ID layer : board()->GetVisibleLayers() )
        response.add_layers( ToProtoEnum<PCB_LAYER_ID, board::types::BoardLayer>( layer ) );

    return response;
}


HANDLER_RESULT<Empty> API_HANDLER_BOARD::handleSetVisibleLayers(
        const HANDLER_CONTEXT<SetVisibleLayers>& aCtx )
{
    if( std::optional<ApiResponseStatus> headless = checkForHeadless( "SetVisibleLayers" ) )
        return tl::unexpected( *headless );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    LSET visible;
    LSET enabled = board()->GetEnabledLayers();

    for( int layerIdx : aCtx.Request.layers() )
    {
        PCB_LAYER_ID layer =
                FromProtoEnum<PCB_LAYER_ID>( static_cast<board::types::BoardLayer>( layerIdx ) );

        if( enabled.Contains( layer ) )
            visible.set( layer );
    }

    board()->SetVisibleLayers( visible );

    PCB_BASE_EDIT_FRAME* editFrame = static_cast<PCB_BASE_EDIT_FRAME*>( m_frame );
    editFrame->GetAppearancePanel()->OnBoardChanged();
    editFrame->GetCanvas()->SyncLayersVisibility( board() );
    editFrame->Refresh();
    return Empty();
}


HANDLER_RESULT<BoardLayerResponse> API_HANDLER_BOARD::handleGetActiveLayer(
        const HANDLER_CONTEXT<GetActiveLayer>& aCtx )
{
    if( std::optional<ApiResponseStatus> headless = checkForHeadless( "GetActiveLayer" ) )
        return tl::unexpected( *headless );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    PCB_BASE_EDIT_FRAME* editFrame = static_cast<PCB_BASE_EDIT_FRAME*>( m_frame );

    BoardLayerResponse response;
    response.set_layer(
            ToProtoEnum<PCB_LAYER_ID, board::types::BoardLayer>( editFrame->GetActiveLayer() ) );

    return response;
}


HANDLER_RESULT<Empty> API_HANDLER_BOARD::handleSetActiveLayer(
        const HANDLER_CONTEXT<SetActiveLayer>& aCtx )
{
    if( std::optional<ApiResponseStatus> headless = checkForHeadless( "SetActiveLayer" ) )
        return tl::unexpected( *headless );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    PCB_LAYER_ID layer = FromProtoEnum<PCB_LAYER_ID>( aCtx.Request.layer() );

    if( !board()->GetEnabledLayers().Contains( layer ) )
    {
        ApiResponseStatus err;
        err.set_status( ApiStatusCode::AS_BAD_REQUEST );
        err.set_error_message( fmt::format( "Layer {} is not a valid layer for the given board",
                                            magic_enum::enum_name( layer ) ) );
        return tl::unexpected( err );
    }

    PCB_BASE_EDIT_FRAME* editFrame = static_cast<PCB_BASE_EDIT_FRAME*>( m_frame );
    editFrame->SetActiveLayer( layer );
    return Empty();
}
