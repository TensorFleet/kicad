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

#include <api/api_handler_symbol_library.h>

#include <api/api_sch_utils.h>
#include <api/api_utils.h>
#include <ki_exception.h>
#include <lib_symbol.h>
#include <libraries/library_manager.h>
#include <libraries/symbol_library_adapter.h>
#include <project_sch.h>
#include <wildcards_and_files_ext.h>

using namespace kiapi::common::commands;
using kiapi::common::ApiStatusCode;


API_HANDLER_SYMBOL_LIBRARY::API_HANDLER_SYMBOL_LIBRARY( PROJECT* aProject ) :
        API_HANDLER_LIBRARY( LIBRARY_TABLE_TYPE::SYMBOL, PROJECT_SCH::SymbolLibAdapter( aProject ), aProject )
{
}


SYMBOL_LIBRARY_ADAPTER* API_HANDLER_SYMBOL_LIBRARY::adapter() const
{
    return static_cast<SYMBOL_LIBRARY_ADAPTER*>( m_adapter );
}


wxString API_HANDLER_SYMBOL_LIBRARY::defaultLibraryExtension() const
{
    return FILEEXT::KiCadSymbolLibFileExtension;
}


std::optional<ApiResponseStatus> API_HANDLER_SYMBOL_LIBRARY::ensureLibraryLoaded( const wxString& aNickname )
{
    if( aNickname.IsEmpty() )
        return badRequest( "a library nickname is required" );

    // The tables know every library; the adapter's cache only knows the loaded ones
    std::optional<LIBRARY_TABLE_ROW*> row = adapter()->GetRow( aNickname );

    if( !row || ( *row )->Disabled() )
        return badRequest( fmt::format( "no enabled symbol library named '{}'", aNickname.ToUTF8().data() ) );

    std::optional<LIB_STATUS> status = adapter()->LoadLibraryEntry( aNickname );

    if( !status || status->load_status != LOAD_STATUS::LOADED )
    {
        wxString error = status && status->error ? status->error->message : wxString( wxS( "library failed to load" ) );
        return badRequest( fmt::format( "symbol library '{}': {}", aNickname.ToUTF8().data(),
                                        error.ToUTF8().data() ) );
    }

    return std::nullopt;
}


HANDLER_RESULT<ListLibraryEntriesResponse> API_HANDLER_SYMBOL_LIBRARY::handleListLibraryEntries(
        const HANDLER_CONTEXT<ListLibraryEntries>& aCtx )
{
    if( std::optional<ApiResponseStatus> e = checkType( aCtx.Request.type() ) )
        return tl::unexpected( *e );

    wxString nickname = wxString::FromUTF8( aCtx.Request.nickname() );

    if( std::optional<ApiResponseStatus> e = ensureLibraryLoaded( nickname ) )
        return tl::unexpected( *e );

    wxString filter = wxString::FromUTF8( aCtx.Request.filter() ).Lower();

    auto matches =
            [&]( const wxString& aText )
            {
                return filter.IsEmpty() || aText.Lower().Contains( filter );
            };

    ListLibraryEntriesResponse response;

    for( LIB_SYMBOL* symbol : adapter()->GetSymbols( nickname ) )
    {
        wxString name = symbol->GetName();

        if( !matches( name ) && !matches( symbol->GetDescription() ) && !matches( symbol->GetKeyWords() ) )
            continue;

        LibraryEntry* entry = response.add_entries();
        entry->mutable_id()->set_library_nickname( nickname.ToUTF8() );
        entry->mutable_id()->set_entry_name( name.ToUTF8() );
        entry->set_name( name.ToUTF8() );
        entry->set_description( symbol->GetDescription().ToUTF8() );
        entry->set_keywords( symbol->GetKeyWords().ToUTF8() );

        SymbolEntryInfo* info = entry->mutable_symbol();
        info->set_unit_count( symbol->GetUnitCount() );
        info->set_is_power( symbol->IsPower() );
        info->set_footprint( symbol->GetFootprintField().GetText().ToUTF8() );

        for( const wxString& fpFilter : symbol->GetFPFilters() )
            info->add_footprint_filters( fpFilter.ToUTF8() );

        if( std::shared_ptr<LIB_SYMBOL> parent = symbol->GetParent().lock() )
            info->set_parent_name( parent->GetName().ToUTF8() );
    }

    return response;
}


HANDLER_RESULT<GetLibraryItemResponse> API_HANDLER_SYMBOL_LIBRARY::handleGetLibraryItem(
        const HANDLER_CONTEXT<GetLibraryItem>& aCtx )
{
    if( std::optional<ApiResponseStatus> e = checkType( aCtx.Request.type() ) )
        return tl::unexpected( *e );

    LIB_ID id = kiapi::common::UnpackLibId( aCtx.Request.id() );

    if( std::optional<ApiResponseStatus> e = ensureLibraryLoaded( id.GetUniStringLibNickname() ) )
        return tl::unexpected( *e );

    LIB_SYMBOL* symbol = nullptr;

    try
    {
        symbol = adapter()->LoadSymbol( id );
    }
    catch( const IO_ERROR& ioe )
    {
        return tl::unexpected( badRequest( fmt::format( "could not load '{}': {}", id.Format().c_str(),
                                                        ioe.What().ToUTF8().data() ) ) );
    }

    if( !symbol )
        return tl::unexpected( badRequest( fmt::format( "symbol '{}' not found", id.Format().c_str() ) ) );

    // Derived symbols are served flattened, as the schematic editor places them
    std::unique_ptr<LIB_SYMBOL> flattened = symbol->Flatten();

    GetLibraryItemResponse response;
    kiapi::common::PackLibId( response.mutable_id(), id );
    PackLibSymbol( response.mutable_item()->mutable_symbol(), flattened.get() );
    return response;
}


HANDLER_RESULT<SaveLibraryItemResponse> API_HANDLER_SYMBOL_LIBRARY::handleSaveLibraryItem(
        const HANDLER_CONTEXT<SaveLibraryItem>& aCtx )
{
    if( std::optional<ApiResponseStatus> e = checkType( aCtx.Request.type() ) )
        return tl::unexpected( *e );

    if( !aCtx.Request.item().has_symbol() )
        return tl::unexpected( badRequest( "SaveLibraryItem for LT_SYMBOL requires item.symbol" ) );

    wxString nickname = wxString::FromUTF8( aCtx.Request.id().library_nickname() );

    if( std::optional<ApiResponseStatus> e = ensureLibraryLoaded( nickname ) )
        return tl::unexpected( *e );

    if( !adapter()->IsSymbolLibWritable( nickname ) )
        return tl::unexpected( badRequest( fmt::format( "symbol library '{}' is read-only",
                                                        nickname.ToUTF8().data() ) ) );

    std::unique_ptr<LIB_SYMBOL> symbol = UnpackLibSymbol( aCtx.Request.item().symbol() );

    if( !symbol )
        return tl::unexpected( badRequest( "could not unpack the symbol" ) );

    wxString name = wxString::FromUTF8( aCtx.Request.id().entry_name() );

    if( name.IsEmpty() )
        name = symbol->GetName();

    if( name.IsEmpty() )
        return tl::unexpected( badRequest( "the symbol has no name; set id.entry_name" ) );

    LIB_ID id( nickname, name );
    symbol->SetName( name );
    symbol->SetLibId( id );

    if( !aCtx.Request.overwrite() && adapter()->LoadSymbol( id ) )
        return tl::unexpected( badRequest( fmt::format( "'{}' already exists and overwrite is not set", id.Format().c_str() ) ) );

    // The plugin's cache takes ownership of the symbol it is given (see SCH_IO_LIB_CACHE::AddSymbol),
    // so it gets its own copy, as the symbol editor does
    try
    {
        if( adapter()->SaveSymbol( nickname, new LIB_SYMBOL( *symbol ), true ) != SYMBOL_LIBRARY_ADAPTER::SAVE_OK )
            return tl::unexpected( badRequest( fmt::format( "'{}' was not saved", id.Format().c_str() ) ) );
    }
    catch( const IO_ERROR& ioe )
    {
        return tl::unexpected( badRequest( fmt::format( "could not save '{}': {}", id.Format().c_str(),
                                                        ioe.What().ToUTF8().data() ) ) );
    }

    SaveLibraryItemResponse response;
    kiapi::common::PackLibId( response.mutable_id(), id );
    return response;
}


HANDLER_RESULT<google::protobuf::Empty> API_HANDLER_SYMBOL_LIBRARY::handleDeleteLibraryItem(
        const HANDLER_CONTEXT<DeleteLibraryItem>& aCtx )
{
    if( std::optional<ApiResponseStatus> e = checkType( aCtx.Request.type() ) )
        return tl::unexpected( *e );

    LIB_ID id = kiapi::common::UnpackLibId( aCtx.Request.id() );
    wxString nickname = id.GetUniStringLibNickname();

    if( std::optional<ApiResponseStatus> e = ensureLibraryLoaded( nickname ) )
        return tl::unexpected( *e );

    if( !adapter()->IsSymbolLibWritable( nickname ) )
        return tl::unexpected( badRequest( fmt::format( "symbol library '{}' is read-only",
                                                        nickname.ToUTF8().data() ) ) );

    if( !adapter()->LoadSymbol( id ) )
        return tl::unexpected( badRequest( fmt::format( "symbol '{}' not found", id.Format().c_str() ) ) );

    try
    {
        adapter()->DeleteSymbol( nickname, id.GetUniStringLibItemName() );
    }
    catch( const IO_ERROR& ioe )
    {
        return tl::unexpected( badRequest( fmt::format( "could not delete '{}': {}", id.Format().c_str(),
                                                        ioe.What().ToUTF8().data() ) ) );
    }

    return google::protobuf::Empty();
}
