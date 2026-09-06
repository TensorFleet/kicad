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

#include <api/api_handler_library.h>

#include <wx/filename.h>

#include <api/api_server.h>
#include <env_paths.h>
#include <libraries/library_manager.h>
#include <paths.h>
#include <pgm_base.h>
#include <project.h>

using namespace kiapi::common::commands;
using kiapi::common::ApiStatusCode;


API_HANDLER_LIBRARY::API_HANDLER_LIBRARY( LIBRARY_TABLE_TYPE aType, LIBRARY_MANAGER_ADAPTER* aAdapter,
                                          PROJECT* aProject ) :
        API_HANDLER(),
        m_type( aType ),
        m_adapter( aAdapter ),
        m_project( aProject )
{
    registerHandler<GetLibraryTables, GetLibraryTablesResponse>( &API_HANDLER_LIBRARY::handleGetLibraryTables );
    registerHandler<CreateLibrary, LibraryTableRow>( &API_HANDLER_LIBRARY::handleCreateLibrary );
    registerHandler<AddLibraryTableRow, LibraryTableRow>( &API_HANDLER_LIBRARY::handleAddLibraryTableRow );
    registerHandler<RemoveLibraryTableRow, google::protobuf::Empty>(
            &API_HANDLER_LIBRARY::handleRemoveLibraryTableRow );
    registerHandler<ListLibraryEntries, ListLibraryEntriesResponse>(
            &API_HANDLER_LIBRARY::handleListLibraryEntries );
    registerHandler<GetLibraryItem, GetLibraryItemResponse>( &API_HANDLER_LIBRARY::handleGetLibraryItem );
    registerHandler<SaveLibraryItem, SaveLibraryItemResponse>( &API_HANDLER_LIBRARY::handleSaveLibraryItem );
    registerHandler<DeleteLibraryItem, google::protobuf::Empty>( &API_HANDLER_LIBRARY::handleDeleteLibraryItem );
}


LibraryType API_HANDLER_LIBRARY::ToProtoType( LIBRARY_TABLE_TYPE aType )
{
    switch( aType )
    {
    case LIBRARY_TABLE_TYPE::SYMBOL:       return LT_SYMBOL;
    case LIBRARY_TABLE_TYPE::FOOTPRINT:    return LT_FOOTPRINT;
    case LIBRARY_TABLE_TYPE::DESIGN_BLOCK: return LT_DESIGN_BLOCK;
    default:                               return LT_UNKNOWN;
    }
}


LIBRARY_TABLE_TYPE API_HANDLER_LIBRARY::FromProtoType( LibraryType aType )
{
    switch( aType )
    {
    case LT_SYMBOL:       return LIBRARY_TABLE_TYPE::SYMBOL;
    case LT_FOOTPRINT:    return LIBRARY_TABLE_TYPE::FOOTPRINT;
    case LT_DESIGN_BLOCK: return LIBRARY_TABLE_TYPE::DESIGN_BLOCK;
    default:              return LIBRARY_TABLE_TYPE::UNINITIALIZED;
    }
}


ApiResponseStatus API_HANDLER_LIBRARY::badRequest( const std::string& aMessage )
{
    ApiResponseStatus e;
    e.set_status( ApiStatusCode::AS_BAD_REQUEST );
    e.set_error_message( aMessage );
    return e;
}


ApiResponseStatus API_HANDLER_LIBRARY::unimplemented( const std::string& aMessage )
{
    ApiResponseStatus e;
    e.set_status( ApiStatusCode::AS_UNIMPLEMENTED );
    e.set_error_message( aMessage );
    return e;
}


std::optional<ApiResponseStatus> API_HANDLER_LIBRARY::checkType( LibraryType aType ) const
{
    if( FromProtoType( aType ) == m_type )
        return std::nullopt;

    // Another type's handler may serve it; no message, this is a flag for the server
    ApiResponseStatus e;
    e.set_status( ApiStatusCode::AS_UNHANDLED );
    return e;
}


void API_HANDLER_LIBRARY::PackRow( LibraryTableRow& aOut, const LIBRARY_TABLE_ROW& aRow ) const
{
    aOut.set_nickname( aRow.Nickname().ToUTF8() );
    aOut.set_uri( aRow.URI().ToUTF8() );
    aOut.set_type( aRow.Type().ToUTF8() );
    aOut.set_options( aRow.Options().ToUTF8() );
    aOut.set_description( aRow.Description().ToUTF8() );
    aOut.set_scope( aRow.Scope() == LIBRARY_TABLE_SCOPE::GLOBAL ? LTS_GLOBAL : LTS_PROJECT );
    aOut.set_enabled( !aRow.Disabled() );
    aOut.set_hidden( aRow.Hidden() );
    aOut.set_ok( aRow.IsOk() );
    aOut.set_error( aRow.ErrorDescription().ToUTF8() );
    aOut.set_resolved_uri( LIBRARY_MANAGER::GetFullURI( &aRow, true ).ToUTF8() );
}


const LIBRARY_TABLE_ROW* API_HANDLER_LIBRARY::findRow( const wxString& aNickname, LIBRARY_TABLE_SCOPE aScope ) const
{
    // Rows that do not validate yet (a library file about to be created) are wanted too
    for( const LIBRARY_TABLE_ROW* row : Pgm().GetLibraryManager().Rows( m_type, aScope, true ) )
    {
        if( row->Nickname() == aNickname )
            return row;
    }

    return nullptr;
}


HANDLER_RESULT<LIBRARY_TABLE*> API_HANDLER_LIBRARY::table( LibraryTableScope aScope ) const
{
    if( aScope != LTS_GLOBAL && aScope != LTS_PROJECT )
        return tl::unexpected( badRequest( "scope must be LTS_GLOBAL or LTS_PROJECT" ) );

    LIBRARY_TABLE_SCOPE scope = aScope == LTS_GLOBAL ? LIBRARY_TABLE_SCOPE::GLOBAL : LIBRARY_TABLE_SCOPE::PROJECT;

    std::optional<LIBRARY_TABLE*> table = Pgm().GetLibraryManager().Table( m_type, scope );

    if( !table || !*table )
        return tl::unexpected( badRequest( "the requested library table is not available" ) );

    return *table;
}


std::optional<ApiResponseStatus> API_HANDLER_LIBRARY::saveTable( LIBRARY_TABLE* aTable,
                                                                 const std::string& aClientName )
{
    std::optional<ApiResponseStatus> error;

    aTable->Save().map_error(
            [&]( const LIBRARY_ERROR& aError )
            {
                error = badRequest( fmt::format( "could not save library table: {}",
                                                 aError.message.ToUTF8().data() ) );
            } );

    if( error )
        return error;

    Pgm().GetLibraryManager().ReloadTables( aTable->Scope(), { m_type } );
    notifyTablesChanged( aClientName );
    return std::nullopt;
}


void API_HANDLER_LIBRARY::notifyTablesChanged( const std::string& aClientName )
{
    if( !Server() || !m_project )
        return;

    kiapi::common::events::Event event;
    kiapi::common::events::ProjectChanged& changed = *event.mutable_project_changed();
    changed.mutable_project()->set_name( m_project->GetProjectName().ToUTF8() );
    changed.mutable_project()->set_path( m_project->GetProjectPath().ToUTF8() );
    changed.set_kind( kiapi::common::events::PCK_LIBRARY_TABLES );
    changed.set_client_name( aClientName );
    publish( event );
}


HANDLER_RESULT<GetLibraryTablesResponse> API_HANDLER_LIBRARY::handleGetLibraryTables(
        const HANDLER_CONTEXT<GetLibraryTables>& aCtx )
{
    if( std::optional<ApiResponseStatus> e = checkType( aCtx.Request.type() ) )
        return tl::unexpected( *e );

    LIBRARY_TABLE_SCOPE scope = LIBRARY_TABLE_SCOPE::BOTH;

    if( aCtx.Request.scope() == LTS_GLOBAL )
        scope = LIBRARY_TABLE_SCOPE::GLOBAL;
    else if( aCtx.Request.scope() == LTS_PROJECT )
        scope = LIBRARY_TABLE_SCOPE::PROJECT;

    GetLibraryTablesResponse response;

    for( LIBRARY_TABLE_ROW* row : Pgm().GetLibraryManager().Rows( m_type, scope, true ) )
        PackRow( *response.add_rows(), *row );

    return response;
}


HANDLER_RESULT<LibraryTableRow> API_HANDLER_LIBRARY::handleCreateLibrary(
        const HANDLER_CONTEXT<CreateLibrary>& aCtx )
{
    if( std::optional<ApiResponseStatus> e = checkType( aCtx.Request.type() ) )
        return tl::unexpected( *e );

    if( !m_adapter )
        return tl::unexpected( unimplemented( "libraries of this type cannot be created through the API" ) );

    wxString nickname = wxString::FromUTF8( aCtx.Request.nickname() );

    if( nickname.IsEmpty() )
        return tl::unexpected( badRequest( "CreateLibrary requires a nickname" ) );

    HANDLER_RESULT<LIBRARY_TABLE*> table = this->table( aCtx.Request.scope() );

    if( !table )
        return tl::unexpected( table.error() );

    if( ( *table )->HasRow( nickname ) )
        return tl::unexpected( badRequest( fmt::format( "a library named '{}' already exists in this table",
                                                        aCtx.Request.nickname() ) ) );

    if( ( *table )->IsReadOnly() )
        return tl::unexpected( badRequest( "the library table is read-only" ) );

    wxString uri = wxString::FromUTF8( aCtx.Request.uri() );

    if( uri.IsEmpty() )
    {
        wxFileName fn;

        if( aCtx.Request.scope() == LTS_PROJECT )
            fn.AssignDir( m_project ? m_project->GetProjectPath() : wxString() );
        else if( m_type == LIBRARY_TABLE_TYPE::SYMBOL )
            fn.AssignDir( PATHS::GetDefaultUserSymbolsPath() );
        else if( m_type == LIBRARY_TABLE_TYPE::FOOTPRINT )
            fn.AssignDir( PATHS::GetDefaultUserFootprintsPath() );
        else
            fn.AssignDir( PATHS::GetDefaultUserDesignBlocksPath() );

        fn.SetName( nickname );
        fn.SetExt( defaultLibraryExtension() );

        uri = NormalizePath( fn, &Pgm().GetLocalEnvVariables(), m_project );
    }

    wxString resolved = LIBRARY_MANAGER::ExpandURI( uri, *m_project );

    if( wxFileName::Exists( resolved ) )
        return tl::unexpected( badRequest( fmt::format( "'{}' already exists on disk", resolved.ToUTF8().data() ) ) );

    LIBRARY_TABLE_ROW& row = ( *table )->InsertRow();
    row.SetNickname( nickname );
    row.SetURI( uri );
    row.SetType( defaultPluginType() );
    row.SetDescription( wxString::FromUTF8( aCtx.Request.description() ) );

    // saveTable reloads the tables, which replaces the table object
    LIBRARY_TABLE_SCOPE scope = ( *table )->Scope();

    if( std::optional<ApiResponseStatus> e = saveTable( *table, aCtx.ClientName ) )
        return tl::unexpected( *e );

    if( !m_adapter->CreateLibrary( nickname ) )
    {
        return tl::unexpected( badRequest( fmt::format( "the table row was added but the library at '{}' could "
                                                        "not be created", resolved.ToUTF8().data() ) ) );
    }

    LibraryTableRow response;

    if( const LIBRARY_TABLE_ROW* saved = findRow( nickname, scope ) )
        PackRow( response, *saved );

    return response;
}


HANDLER_RESULT<LibraryTableRow> API_HANDLER_LIBRARY::handleAddLibraryTableRow(
        const HANDLER_CONTEXT<AddLibraryTableRow>& aCtx )
{
    if( std::optional<ApiResponseStatus> e = checkType( aCtx.Request.type() ) )
        return tl::unexpected( *e );

    const LibraryTableRow& in = aCtx.Request.row();
    wxString               nickname = wxString::FromUTF8( in.nickname() );

    if( nickname.IsEmpty() )
        return tl::unexpected( badRequest( "AddLibraryTableRow requires a nickname" ) );

    if( in.uri().empty() )
        return tl::unexpected( badRequest( "AddLibraryTableRow requires a uri" ) );

    HANDLER_RESULT<LIBRARY_TABLE*> table = this->table( aCtx.Request.scope() );

    if( !table )
        return tl::unexpected( table.error() );

    if( ( *table )->IsReadOnly() )
        return tl::unexpected( badRequest( "the library table is read-only" ) );

    LIBRARY_TABLE_ROW* row = nullptr;

    if( std::optional<LIBRARY_TABLE_ROW*> existing = ( *table )->Row( nickname ) )
    {
        if( !aCtx.Request.replace() )
        {
            return tl::unexpected( badRequest( fmt::format( "a library named '{}' already exists in this table",
                                                            in.nickname() ) ) );
        }

        row = *existing;
    }
    else
    {
        row = &( *table )->InsertRow();
    }

    row->SetNickname( nickname );
    row->SetURI( wxString::FromUTF8( in.uri() ) );
    row->SetType( in.type().empty() ? defaultPluginType() : wxString::FromUTF8( in.type() ) );
    row->SetOptions( wxString::FromUTF8( in.options() ) );
    row->SetDescription( wxString::FromUTF8( in.description() ) );
    row->SetDisabled( !in.enabled() );
    row->SetHidden( in.hidden() );

    // saveTable reloads the tables, which replaces the table object
    LIBRARY_TABLE_SCOPE scope = ( *table )->Scope();

    if( std::optional<ApiResponseStatus> e = saveTable( *table, aCtx.ClientName ) )
        return tl::unexpected( *e );

    LibraryTableRow response;

    if( const LIBRARY_TABLE_ROW* saved = findRow( nickname, scope ) )
        PackRow( response, *saved );

    return response;
}


HANDLER_RESULT<google::protobuf::Empty> API_HANDLER_LIBRARY::handleRemoveLibraryTableRow(
        const HANDLER_CONTEXT<RemoveLibraryTableRow>& aCtx )
{
    if( std::optional<ApiResponseStatus> e = checkType( aCtx.Request.type() ) )
        return tl::unexpected( *e );

    wxString nickname = wxString::FromUTF8( aCtx.Request.nickname() );

    HANDLER_RESULT<LIBRARY_TABLE*> table = this->table( aCtx.Request.scope() );

    if( !table )
        return tl::unexpected( table.error() );

    if( ( *table )->IsReadOnly() )
        return tl::unexpected( badRequest( "the library table is read-only" ) );

    std::deque<LIBRARY_TABLE_ROW>& rows = ( *table )->Rows();

    auto it = std::find_if( rows.begin(), rows.end(),
                            [&]( const LIBRARY_TABLE_ROW& aRow )
                            {
                                return aRow.Nickname() == nickname;
                            } );

    if( it == rows.end() )
    {
        return tl::unexpected( badRequest( fmt::format( "no library named '{}' in this table",
                                                        aCtx.Request.nickname() ) ) );
    }

    rows.erase( it );

    if( std::optional<ApiResponseStatus> e = saveTable( *table, aCtx.ClientName ) )
        return tl::unexpected( *e );

    return google::protobuf::Empty();
}


HANDLER_RESULT<ListLibraryEntriesResponse> API_HANDLER_LIBRARY::handleListLibraryEntries(
        const HANDLER_CONTEXT<ListLibraryEntries>& aCtx )
{
    if( std::optional<ApiResponseStatus> e = checkType( aCtx.Request.type() ) )
        return tl::unexpected( *e );

    return tl::unexpected( unimplemented( "listing entries is not implemented for this library type" ) );
}


HANDLER_RESULT<GetLibraryItemResponse> API_HANDLER_LIBRARY::handleGetLibraryItem(
        const HANDLER_CONTEXT<GetLibraryItem>& aCtx )
{
    if( std::optional<ApiResponseStatus> e = checkType( aCtx.Request.type() ) )
        return tl::unexpected( *e );

    return tl::unexpected( unimplemented( "loading items is not implemented for this library type" ) );
}


HANDLER_RESULT<SaveLibraryItemResponse> API_HANDLER_LIBRARY::handleSaveLibraryItem(
        const HANDLER_CONTEXT<SaveLibraryItem>& aCtx )
{
    if( std::optional<ApiResponseStatus> e = checkType( aCtx.Request.type() ) )
        return tl::unexpected( *e );

    return tl::unexpected( unimplemented( "saving items is not implemented for this library type" ) );
}


HANDLER_RESULT<google::protobuf::Empty> API_HANDLER_LIBRARY::handleDeleteLibraryItem(
        const HANDLER_CONTEXT<DeleteLibraryItem>& aCtx )
{
    if( std::optional<ApiResponseStatus> e = checkType( aCtx.Request.type() ) )
        return tl::unexpected( *e );

    return tl::unexpected( unimplemented( "deleting items is not implemented for this library type" ) );
}
