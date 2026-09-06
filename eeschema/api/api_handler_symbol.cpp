/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
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

#include <api/api_handler_symbol.h>

#include <api/api_enums.h>
#include <api/api_sch_utils.h>
#include <api/api_utils.h>
#include <lib_id.h>
#include <lib_symbol.h>
#include <project.h>
#include <sch_field.h>
#include <sch_item.h>
#include <sch_commit.h>
#include <sch_pin.h>
#include <undo_redo_container.h>
#include <wx/log.h>

using namespace kiapi::common::commands;
using kiapi::common::types::DocumentType;
using kiapi::common::types::ItemRequestStatus;
using google::protobuf::Empty;


namespace
{

/**
 * A commit against a LIB_SYMBOL that has no editor frame: additions and removals are applied on
 * Push(), and Revert() puts the symbol back the way it was.  Push hands the undo list to the
 * handler's undo stack, as the editor commits hand theirs to the frame.
 */
class SYMBOL_API_COMMIT : public COMMIT
{
public:
    SYMBOL_API_COMMIT( LIB_SYMBOL* aSymbol, UNDO_REDO_SINK* aSink ) :
            m_symbol( aSymbol ),
            m_sink( aSink )
    {
    }

    ~SYMBOL_API_COMMIT() override
    {
        // A commit that is neither pushed nor reverted leaves the symbol untouched
        if( !Empty() )
            Revert();
    }

    void Push( const wxString& aMessage = wxT( "A commit" ), int aFlags = 0 ) override
    {
        // The undo list is built the way the editor commits build theirs: added items as
        // NEWITEM, removed items as DELETED (the list keeps the image, since the symbol frees
        // the original), and modified items as CHANGED with the image as link
        PICKED_ITEMS_LIST undoList;
        undoList.SetDescription( aMessage );

        const bool keepHistory = m_sink && !( aFlags & SKIP_UNDO );

        for( COMMIT_LINE& ent : m_entries )
        {
            SCH_ITEM* item = static_cast<SCH_ITEM*>( ent.m_item );
            SCH_ITEM* copy = static_cast<SCH_ITEM*>( ent.m_copy );

            switch( ent.m_type & CHT_TYPE )
            {
            case CHT_ADD:
                if( !( ent.m_type & CHT_DONE ) )
                    m_symbol->AddDrawItem( item );

                if( keepHistory )
                    undoList.PushItem( ITEM_PICKER( nullptr, item, UNDO_REDO::NEWITEM ) );

                break;

            case CHT_REMOVE:
                if( !( ent.m_type & CHT_DONE ) )
                    m_symbol->RemoveDrawItem( item ); // frees the item

                if( keepHistory && copy )
                {
                    copy->SetFlags( UR_TRANSIENT );
                    undoList.PushItem( ITEM_PICKER( nullptr, copy, UNDO_REDO::DELETED ) );
                }
                else
                {
                    delete copy;
                }

                break;

            case CHT_MODIFY:
                if( keepHistory && copy )
                {
                    ITEM_PICKER picker( nullptr, item, UNDO_REDO::CHANGED );
                    picker.SetLink( copy );
                    undoList.PushItem( picker );
                }
                else
                {
                    delete copy;
                }

                break;

            default:
                break;
            }
        }

        clear();

        if( undoList.GetCount() > 0 )
            m_sink->SaveCopyInUndoList( undoList, false );
    }

    void Revert() override
    {
        for( auto it = m_entries.rbegin(); it != m_entries.rend(); ++it )
        {
            COMMIT_LINE& ent = *it;
            SCH_ITEM*    item = static_cast<SCH_ITEM*>( ent.m_item );
            SCH_ITEM*    copy = static_cast<SCH_ITEM*>( ent.m_copy );

            switch( ent.m_type & CHT_TYPE )
            {
            case CHT_ADD:
                if( ent.m_type & CHT_DONE )
                    m_symbol->RemoveDrawItem( item );
                else
                    delete item;

                break;

            case CHT_REMOVE:
                if( ent.m_type & CHT_DONE )
                    m_symbol->AddDrawItem( copy ); // the original is gone; the image takes its place
                else
                    delete copy;

                break;

            case CHT_MODIFY:
                if( copy )
                    item->SwapItemData( copy );

                delete copy;
                break;

            default:
                break;
            }
        }

        clear();
    }

protected:
    EDA_ITEM* undoLevelItem( EDA_ITEM* aItem ) const override { return aItem; }

    EDA_ITEM* makeImage( EDA_ITEM* aItem ) const override { return aItem->Clone(); }

private:
    LIB_SYMBOL*     m_symbol;
    UNDO_REDO_SINK* m_sink;
};


/// The child item types a symbol document exposes
const std::set<KICAD_T> s_symbolChildTypes = { SCH_PIN_T, SCH_SHAPE_T, SCH_TEXT_T, SCH_TEXTBOX_T, SCH_FIELD_T };

} // namespace


API_HANDLER_SYMBOL::API_HANDLER_SYMBOL( std::shared_ptr<SYMBOL_CONTEXT> aContext ) :
        API_HANDLER_EDITOR( nullptr ),
        m_context( std::move( aContext ) )
{
    registerHandler<GetOpenDocuments, GetOpenDocumentsResponse>( &API_HANDLER_SYMBOL::handleGetOpenDocuments );
    registerHandler<SaveDocument, Empty>( &API_HANDLER_SYMBOL::handleSaveDocument );
    registerHandler<SaveCopyOfDocument, Empty>( &API_HANDLER_SYMBOL::handleSaveCopyOfDocument );
    registerHandler<GetItems, GetItemsResponse>( &API_HANDLER_SYMBOL::handleGetItems );
    registerHandler<GetItemsById, GetItemsResponse>( &API_HANDLER_SYMBOL::handleGetItemsById );

    m_undoStack = std::make_unique<API_UNDO_STACK>(
            [this]( PICKED_ITEMS_LIST& aList )
            {
                restoreUndoList( aList );
            },
            []( PICKED_ITEMS_LIST& aList )
            {
                aList.ClearListAndDeleteItems(
                        []( EDA_ITEM* aItem )
                        {
                            delete aItem;
                        } );
            } );
}


void API_HANDLER_SYMBOL::restoreUndoList( PICKED_ITEMS_LIST& aList )
{
    LIB_SYMBOL* sym = symbol();

    // Reverse order, so that an item changed and then deleted by one command comes back right
    for( int ii = (int) aList.GetCount() - 1; ii >= 0; ii-- )
    {
        ITEM_PICKER& wrapper = aList.GetItemWrapper( ii );
        EDA_ITEM*    picked = wrapper.GetItem();

        if( !picked )
            continue;

        switch( wrapper.GetStatus() )
        {
        case UNDO_REDO::CHANGED:
        {
            // Pointers go stale when an item is re-created by an undo; resolve by id
            SCH_ITEM* live = picked->m_Uuid == sym->m_Uuid ? sym : findItem( picked->m_Uuid );
            SCH_ITEM* image = static_cast<SCH_ITEM*>( wrapper.GetLink() );

            if( !live || !image )
                break;

            live->SwapItemData( image );
            wrapper.SetItem( live );
            break;
        }

        case UNDO_REDO::NEWITEM:
        {
            SCH_ITEM* live = findItem( picked->m_Uuid );

            if( !live )
                break;

            // The symbol frees what it removes, so the list keeps a copy for the redo
            SCH_ITEM* kept = static_cast<SCH_ITEM*>( live->Clone() );
            kept->SetFlags( UR_TRANSIENT );
            sym->RemoveDrawItem( live );
            wrapper.SetItem( kept );
            wrapper.SetStatus( UNDO_REDO::DELETED );
            break;
        }

        case UNDO_REDO::DELETED:
        {
            SCH_ITEM* item = static_cast<SCH_ITEM*>( picked );
            item->ClearFlags( UR_TRANSIENT );
            sym->AddDrawItem( item );
            wrapper.SetStatus( UNDO_REDO::NEWITEM );
            break;
        }

        default:
            break;
        }
    }
}


std::optional<DocumentSpecifier> API_HANDLER_SYMBOL::Document() const
{
    DocumentSpecifier doc;
    LIB_ID            id = m_context->GetLoadedLibId();

    doc.set_type( DocumentType::DOCTYPE_SYMBOL );
    doc.mutable_lib_id()->set_library_nickname( id.GetUniStringLibNickname() );
    doc.mutable_lib_id()->set_entry_name( id.GetUniStringLibItemName() );

    if( !m_context->Prj().IsNullProject() )
        PackProject( *doc.mutable_project(), m_context->Prj() );

    return doc;
}


std::unique_ptr<COMMIT> API_HANDLER_SYMBOL::createCommit()
{
    return std::make_unique<SYMBOL_API_COMMIT>( symbol(), m_undoStack.get() );
}


tl::expected<bool, ApiResponseStatus> API_HANDLER_SYMBOL::validateDocumentInternal(
        const DocumentSpecifier& aDocument ) const
{
    if( aDocument.type() != DocumentType::DOCTYPE_SYMBOL )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "the requested document is not a symbol" );
        return tl::unexpected( e );
    }

    LIB_ID      loaded = m_context->GetLoadedLibId();
    std::string actualLib = loaded.GetUniStringLibNickname().ToStdString();
    std::string actualName = loaded.GetUniStringLibItemName().ToStdString();

    if( aDocument.lib_id().library_nickname() != actualLib )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "the requested library is {} but the actual library is {}",
                                          aDocument.lib_id().library_nickname(), actualLib ) );
        return tl::unexpected( e );
    }

    if( aDocument.lib_id().entry_name() != actualName )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "the requested symbol name is {} but the actual name is {}",
                                          aDocument.lib_id().entry_name(), actualName ) );
        return tl::unexpected( e );
    }

    return true;
}


SCH_ITEM* API_HANDLER_SYMBOL::findItem( const KIID& aId ) const
{
    for( SCH_ITEM& item : symbol()->GetDrawItems() )
    {
        if( item.m_Uuid == aId )
            return &item;
    }

    return nullptr;
}


std::optional<EDA_ITEM*> API_HANDLER_SYMBOL::getItemFromDocument( const DocumentSpecifier& aDocument,
                                                                  const KIID& aId )
{
    if( !validateDocumentInternal( aDocument ) )
        return std::nullopt;

    if( SCH_ITEM* item = findItem( aId ) )
        return item;

    return std::nullopt;
}


HANDLER_RESULT<GetOpenDocumentsResponse> API_HANDLER_SYMBOL::handleGetOpenDocuments(
        const HANDLER_CONTEXT<GetOpenDocuments>& aCtx )
{
    if( aCtx.Request.type() != DocumentType::DOCTYPE_SYMBOL )
    {
        ApiResponseStatus e;
        // No message needed for AS_UNHANDLED; this is an internal flag for the API server
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    GetOpenDocumentsResponse response;
    response.mutable_documents()->Add( *Document() );
    return response;
}


HANDLER_RESULT<Empty> API_HANDLER_SYMBOL::handleSaveDocument( const HANDLER_CONTEXT<SaveDocument>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( !m_context->SaveSymbol() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "failed to save symbol" );
        return tl::unexpected( e );
    }

    notifyDocumentSaved( m_context->GetLoadedLibId().GetUniStringLibId() );
    return Empty();
}


HANDLER_RESULT<Empty> API_HANDLER_SYMBOL::handleSaveCopyOfDocument(
        const HANDLER_CONTEXT<SaveCopyOfDocument>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    wxString pathStr = wxString::FromUTF8( aCtx.Request.path() );

    if( pathStr.IsEmpty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "path must contain the new symbol name, "
                             "optionally prefixed with a library nickname (e.g. \"lib:name\")" );
        return tl::unexpected( e );
    }

    // path can be "NewName" (same library) or "LibNick:NewName" (different library)
    LIB_ID target;
    target.Parse( pathStr );

    wxString libraryName = target.GetUniStringLibNickname();

    if( libraryName.IsEmpty() )
        libraryName = m_context->GetLoadedLibId().GetUniStringLibNickname();

    wxString newName = target.GetUniStringLibItemName();
    bool     overwrite = aCtx.Request.has_options() && aCtx.Request.options().overwrite();
    wxString error;

    if( !m_context->SaveSymbolCopy( libraryName, newName, overwrite, &error ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "failed to save symbol copy '{}' to library '{}': {}",
                                          newName.ToStdString(), libraryName.ToStdString(),
                                          error.ToStdString() ) );
        return tl::unexpected( e );
    }

    return Empty();
}


HANDLER_RESULT<GetItemsResponse> API_HANDLER_SYMBOL::handleGetItems( const HANDLER_CONTEXT<GetItems>& aCtx )
{
    if( !validateItemHeaderDocument( aCtx.Request.header() ) )
    {
        ApiResponseStatus e;
        // No message needed for AS_UNHANDLED; this is an internal flag for the API server
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    GetItemsResponse response;
    std::set<KICAD_T> typesRequested;
    bool              wantSymbol = false;

    for( int typeRaw : aCtx.Request.types() )
    {
        auto    typeMessage = static_cast<types::KiCadObjectType>( typeRaw );
        KICAD_T type = FromProtoEnum<KICAD_T>( typeMessage );

        if( type == LIB_SYMBOL_T )
            wantSymbol = true;
        else if( s_symbolChildTypes.contains( type ) )
            typesRequested.insert( type );
    }

    if( !wantSymbol && typesRequested.empty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "none of the requested types are valid for a symbol document" );
        return tl::unexpected( e );
    }

    std::vector<const EDA_ITEM*> items;

    if( wantSymbol )
        items.push_back( symbol() );

    for( const SCH_ITEM& item : symbol()->GetDrawItems() )
    {
        if( typesRequested.contains( item.Type() ) )
            items.push_back( &item );
    }

    windowItems( aCtx.Request, items, response,
                 []( const EDA_ITEM* aItem )
                 {
                     return aItem;
                 } );

    for( const EDA_ITEM* item : items )
    {
        if( item == symbol() )
        {
            kiapi::schematic::types::SchematicSymbol packed;
            PackLibSymbol( &packed, symbol() );
            response.add_items()->PackFrom( packed );
        }
        else
        {
            google::protobuf::Any itemBuf;
            item->Serialize( itemBuf );
            response.mutable_items()->Add( std::move( itemBuf ) );
        }
    }

    response.set_status( ItemRequestStatus::IRS_OK );
    return response;
}


std::map<KICAD_T, uint32_t> API_HANDLER_SYMBOL::countItems( const DocumentSpecifier& aDocument )
{
    std::map<KICAD_T, uint32_t> counts;
    counts[LIB_SYMBOL_T] = 1;

    for( const SCH_ITEM& item : symbol()->GetDrawItems() )
    {
        if( s_symbolChildTypes.contains( item.Type() ) )
            ++counts[item.Type()];
    }

    return counts;
}


HANDLER_RESULT<GetItemsResponse> API_HANDLER_SYMBOL::handleGetItemsById( const HANDLER_CONTEXT<GetItemsById>& aCtx )
{
    if( !validateItemHeaderDocument( aCtx.Request.header() ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    GetItemsResponse response;

    for( const types::KIID& id : aCtx.Request.items() )
    {
        if( SCH_ITEM* item = findItem( KIID( id.value() ) ) )
        {
            google::protobuf::Any itemBuf;
            item->Serialize( itemBuf );
            response.mutable_items()->Add( std::move( itemBuf ) );
        }
    }

    response.set_status( ItemRequestStatus::IRS_OK );
    return response;
}


HANDLER_RESULT<ItemRequestStatus> API_HANDLER_SYMBOL::handleCreateUpdateItemsInternal( bool aCreate,
        const std::string& aClientName,
        const types::ItemHeader& aHeader,
        const google::protobuf::RepeatedPtrField<google::protobuf::Any>& aItems,
        std::function<void( ItemStatus, google::protobuf::Any )> aItemHandler )
{
    ApiResponseStatus e;

    auto containerResult = validateItemHeaderDocument( aHeader );

    if( !containerResult && containerResult.error().status() == ApiStatusCode::AS_UNHANDLED )
    {
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }
    else if( !containerResult )
    {
        e.CopyFrom( containerResult.error() );
        return tl::unexpected( e );
    }

    COMMIT* commit = getCurrentCommit( aClientName );

    for( const google::protobuf::Any& anyItem : aItems )
    {
        ItemStatus             status;
        std::optional<KICAD_T> type = TypeNameFromAny( anyItem );

        if( !type )
        {
            status.set_code( ItemStatusCode::ISC_INVALID_TYPE );
            status.set_error_message( fmt::format( "Could not decode a valid type from {}", anyItem.type_url() ) );
            aItemHandler( status, anyItem );
            continue;
        }

        if( !s_symbolChildTypes.contains( *type ) )
        {
            status.set_code( ItemStatusCode::ISC_INVALID_TYPE );
            status.set_error_message( fmt::format( "{} is not a valid child of a symbol", anyItem.type_url() ) );
            aItemHandler( status, anyItem );
            continue;
        }

        std::unique_ptr<EDA_ITEM> item = CreateItemForType( *type, symbol() );

        if( !item )
        {
            status.set_code( ItemStatusCode::ISC_INVALID_TYPE );
            status.set_error_message( fmt::format( "could not create an item of type {}", anyItem.type_url() ) );
            aItemHandler( status, anyItem );
            continue;
        }

        if( !item->Deserialize( anyItem ) )
        {
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( fmt::format( "could not unpack {} from request", item->GetClass().ToStdString() ) );
            return tl::unexpected( e );
        }

        SCH_ITEM* existingItem = findItem( item->m_Uuid );

        if( aCreate && existingItem )
        {
            status.set_code( ItemStatusCode::ISC_EXISTING );
            status.set_error_message( fmt::format( "an item with UUID {} already exists",
                                                   item->m_Uuid.AsStdString() ) );
            aItemHandler( status, anyItem );
            continue;
        }
        else if( !aCreate && !existingItem )
        {
            status.set_code( ItemStatusCode::ISC_NONEXISTENT );
            status.set_error_message( fmt::format( "an item with UUID {} does not exist",
                                                   item->m_Uuid.AsStdString() ) );
            aItemHandler( status, anyItem );
            continue;
        }

        status.set_code( ItemStatusCode::ISC_OK );
        google::protobuf::Any newItem;

        if( aCreate )
        {
            SCH_ITEM* createdItem = static_cast<SCH_ITEM*>( item.release() );
            createdItem->SetParent( symbol() );
            createdItem->SetLayer( LAYER_DEVICE );
            commit->Add( createdItem );
            createdItem->Serialize( newItem );
        }
        else
        {
            commit->Modify( existingItem );
            existingItem->SwapItemData( static_cast<SCH_ITEM*>( item.get() ) );
            existingItem->Serialize( newItem );
        }

        aItemHandler( status, newItem );
    }

    if( !m_activeClients.contains( aClientName ) )
    {
        pushCurrentCommit( aClientName, aCreate ? _( "Created items via API" )
                                                : _( "Modified items via API" ) );
    }

    return ItemRequestStatus::IRS_OK;
}


void API_HANDLER_SYMBOL::deleteItemsInternal( std::map<KIID, ItemDeletionStatus>& aItemsToDelete,
                                              const std::string& aClientName )
{
    COMMIT* commit = getCurrentCommit( aClientName );

    for( auto& [id, status] : aItemsToDelete )
    {
        SCH_ITEM* item = findItem( id );

        if( !item )
            continue;

        if( item->Type() == SCH_FIELD_T && static_cast<SCH_FIELD*>( item )->IsMandatory() )
        {
            status = ItemDeletionStatus::IDS_IMMUTABLE;
            continue;
        }

        commit->Remove( item );
        status = ItemDeletionStatus::IDS_OK;
    }

    if( !m_activeClients.contains( aClientName ) )
        pushCurrentCommit( aClientName, _( "Deleted items via API" ) );
}
