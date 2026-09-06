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

/**
 * End-to-end tests for the library commands (library_commands.proto): tables, entries, and
 * reading/writing footprints and symbols through kicad-cli api-server.  Since 11.0.
 */

#include <boost/test/unit_test.hpp>
#include <wx/ffile.h>
#include <wx/filefn.h>
#include <wx/filename.h>

#include "api_e2e_utils.h"

#include <api/api_handler_footprint_library.h>
#include <api/common/commands/library_commands.pb.h>
#include <footprint.h>
#include <pad.h>

using namespace kiapi::common::commands;
using kiapi::common::types::LibraryIdentifier;


namespace
{

/// A throw-away project folder holding a copy of the kitchen sink project file
class TEMP_PROJECT
{
public:
    ~TEMP_PROJECT()
    {
        if( !m_dir.IsEmpty() && wxFileName::DirExists( m_dir ) )
            wxFileName::Rmdir( m_dir, wxPATH_RMDIR_RECURSIVE );
    }

    bool Create()
    {
        wxString token = wxFileName::CreateTempFileName( wxS( "kicad-api-lib-" ) );

        if( token.IsEmpty() )
            return false;

        wxRemoveFile( token );

        if( !wxFileName::Mkdir( token, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) )
            return false;

        m_dir = token;

        wxString   testDataDir = wxString::FromUTF8( KI_TEST::GetPcbnewTestDataDir() );
        wxFileName srcPro( testDataDir, wxS( "api_kitchen_sink.kicad_pro" ) );
        wxFileName dstPro( m_dir, srcPro.GetFullName() );

        if( !wxCopyFile( srcPro.GetFullPath(), dstPro.GetFullPath(), true ) )
            return false;

        m_projectPath = dstPro.GetFullPath();
        return true;
    }

    const wxString& Dir() const { return m_dir; }
    const wxString& ProjectPath() const { return m_projectPath; }

private:
    wxString m_dir;
    wxString m_projectPath;
};


template <typename REQUEST, typename RESPONSE>
bool Send( API_TEST_CLIENT& aClient, const REQUEST& aRequest, RESPONSE* aOut, wxString* aError )
{
    kiapi::common::ApiResponse response;

    if( !aClient.SendCommand( aRequest, &response ) )
    {
        *aError = aClient.LastError();
        return false;
    }

    if( response.status().status() != kiapi::common::AS_OK )
    {
        *aError = response.status().error_message();
        return false;
    }

    if( aOut && !response.message().UnpackTo( aOut ) )
    {
        *aError = wxS( "Failed to unpack response" );
        return false;
    }

    return true;
}


template <typename REQUEST>
kiapi::common::ApiStatusCode SendStatus( API_TEST_CLIENT& aClient, const REQUEST& aRequest )
{
    kiapi::common::ApiResponse response;

    if( !aClient.SendCommand( aRequest, &response ) )
        return kiapi::common::AS_UNKNOWN;

    return response.status().status();
}

} // namespace


BOOST_AUTO_TEST_SUITE( ApiLibraries )


BOOST_FIXTURE_TEST_CASE( LibraryCommandsRequireProject, API_SERVER_E2E_FIXTURE )
{
    BOOST_REQUIRE_MESSAGE( Start(), LastError() );
    BOOST_REQUIRE( Client().CloseAllDocuments() );

    GetLibraryTables request;
    request.set_type( LT_FOOTPRINT );

    // No project open: no handler serves the library commands
    BOOST_CHECK_EQUAL( SendStatus( Client(), request ), kiapi::common::AS_UNHANDLED );
}


BOOST_FIXTURE_TEST_CASE( FootprintLibraryRoundTrip, API_SERVER_E2E_FIXTURE )
{
    BOOST_REQUIRE_MESSAGE( Start(), LastError() );

    TEMP_PROJECT project;
    BOOST_REQUIRE( project.Create() );

    kiapi::common::types::DocumentSpecifier document;
    BOOST_REQUIRE_MESSAGE( Client().OpenDocument( project.ProjectPath(), kiapi::common::types::DOCTYPE_PROJECT,
                                                  &document ),
                           "OpenDocument failed: " + Client().LastError() );

    wxString error;

    // The table listing works with just the project open; the project table starts empty
    {
        GetLibraryTables request;
        request.set_type( LT_FOOTPRINT );
        request.set_scope( LTS_PROJECT );

        GetLibraryTablesResponse response;
        BOOST_REQUIRE_MESSAGE( Send( Client(), request, &response, &error ), error );
        BOOST_CHECK_EQUAL( response.rows_size(), 0 );
    }

    // Create a project-scoped library
    {
        CreateLibrary request;
        request.set_type( LT_FOOTPRINT );
        request.set_nickname( "qa_fp" );
        request.set_scope( LTS_PROJECT );
        request.set_description( "QA footprints" );

        LibraryTableRow row;
        BOOST_REQUIRE_MESSAGE( Send( Client(), request, &row, &error ), error );
        BOOST_CHECK_EQUAL( row.nickname(), "qa_fp" );
        BOOST_CHECK_EQUAL( row.scope(), LTS_PROJECT );
        BOOST_CHECK_EQUAL( row.type(), "KiCad" );
        BOOST_CHECK( row.uri().find( "${KIPRJMOD}" ) != std::string::npos );
        BOOST_CHECK( wxFileName::DirExists( wxString::FromUTF8( row.resolved_uri() ) ) );

        // A second library with the same nickname is refused
        BOOST_CHECK_EQUAL( SendStatus( Client(), request ), kiapi::common::AS_BAD_REQUEST );
    }

    // Save a footprint built here into it
    {
        FOOTPRINT footprint( nullptr );
        footprint.SetFPID( LIB_ID( wxS( "qa_fp" ), wxS( "QA_PAD" ) ) );
        footprint.SetLibDescription( wxS( "one pad" ) );
        footprint.SetKeywords( wxS( "qa test" ) );
        footprint.SetAttributes( FP_SMD );

        PAD* pad = new PAD( &footprint );
        pad->SetNumber( wxS( "1" ) );
        pad->SetAttribute( PAD_ATTRIB::SMD );
        pad->SetShape( PADSTACK::ALL_LAYERS, PAD_SHAPE::RECTANGLE );
        pad->SetSize( PADSTACK::ALL_LAYERS, VECTOR2I( 1000000, 500000 ) );
        pad->SetLayerSet( PAD::SMDMask() );
        footprint.Add( pad );

        SaveLibraryItem request;
        request.set_type( LT_FOOTPRINT );
        request.mutable_id()->set_library_nickname( "qa_fp" );
        request.mutable_id()->set_entry_name( "QA_PAD" );
        API_HANDLER_FOOTPRINT_LIBRARY::PackLibraryFootprint( *request.mutable_item()->mutable_footprint(),
                                                             footprint );

        SaveLibraryItemResponse response;
        BOOST_REQUIRE_MESSAGE( Send( Client(), request, &response, &error ), error );
        BOOST_CHECK_EQUAL( response.id().entry_name(), "QA_PAD" );

        // Saving again without overwrite is refused
        BOOST_CHECK_EQUAL( SendStatus( Client(), request ), kiapi::common::AS_BAD_REQUEST );
        request.set_overwrite( true );
        BOOST_REQUIRE_MESSAGE( Send( Client(), request, &response, &error ), error );

        wxFileName file( project.Dir() + wxS( "/qa_fp.pretty" ), wxS( "QA_PAD.kicad_mod" ) );
        BOOST_CHECK( file.FileExists() );
    }

    // List it back, with and without a filter
    {
        ListLibraryEntries request;
        request.set_type( LT_FOOTPRINT );
        request.set_nickname( "qa_fp" );

        ListLibraryEntriesResponse response;
        BOOST_REQUIRE_MESSAGE( Send( Client(), request, &response, &error ), error );
        BOOST_REQUIRE_EQUAL( response.entries_size(), 1 );

        const LibraryEntry& entry = response.entries( 0 );
        BOOST_CHECK_EQUAL( entry.name(), "QA_PAD" );
        BOOST_CHECK_EQUAL( entry.description(), "one pad" );
        BOOST_CHECK_EQUAL( entry.keywords(), "qa test" );
        BOOST_CHECK_EQUAL( entry.footprint().pad_count(), 1 );
        BOOST_CHECK_EQUAL( entry.footprint().mounting_style(), kiapi::board::types::FMS_SMD );

        request.set_filter( "nothing-matches" );
        BOOST_REQUIRE_MESSAGE( Send( Client(), request, &response, &error ), error );
        BOOST_CHECK_EQUAL( response.entries_size(), 0 );

        request.set_filter( "ONE PAD" );
        BOOST_REQUIRE_MESSAGE( Send( Client(), request, &response, &error ), error );
        BOOST_CHECK_EQUAL( response.entries_size(), 1 );
    }

    // Load it
    {
        GetLibraryItem request;
        request.set_type( LT_FOOTPRINT );
        request.mutable_id()->set_library_nickname( "qa_fp" );
        request.mutable_id()->set_entry_name( "QA_PAD" );

        GetLibraryItemResponse response;
        BOOST_REQUIRE_MESSAGE( Send( Client(), request, &response, &error ), error );
        BOOST_REQUIRE( response.item().has_footprint() );
        BOOST_CHECK_EQUAL( response.item().footprint().id().entry_name(), "QA_PAD" );
        BOOST_CHECK_EQUAL( response.item().footprint().attributes().mounting_style(),
                           kiapi::board::types::FMS_SMD );

        int pads = 0;

        for( const google::protobuf::Any& item : response.item().footprint().items() )
        {
            if( item.Is<kiapi::board::types::Pad>() )
                ++pads;
        }

        BOOST_CHECK_EQUAL( pads, 1 );

        request.mutable_id()->set_entry_name( "MISSING" );
        BOOST_CHECK_EQUAL( SendStatus( Client(), request ), kiapi::common::AS_BAD_REQUEST );
    }

    // Delete it
    {
        DeleteLibraryItem request;
        request.set_type( LT_FOOTPRINT );
        request.mutable_id()->set_library_nickname( "qa_fp" );
        request.mutable_id()->set_entry_name( "QA_PAD" );

        google::protobuf::Empty response;
        BOOST_REQUIRE_MESSAGE( Send( Client(), request, &response, &error ), error );
        BOOST_CHECK_EQUAL( SendStatus( Client(), request ), kiapi::common::AS_BAD_REQUEST );
    }

    // Table rows: add an alias, list, remove
    {
        AddLibraryTableRow add;
        add.set_type( LT_FOOTPRINT );
        add.set_scope( LTS_PROJECT );
        add.mutable_row()->set_nickname( "qa_alias" );
        add.mutable_row()->set_uri( "${KIPRJMOD}/qa_fp.pretty" );
        add.mutable_row()->set_enabled( true );

        LibraryTableRow row;
        BOOST_REQUIRE_MESSAGE( Send( Client(), add, &row, &error ), error );
        BOOST_CHECK_EQUAL( row.type(), "KiCad" );

        GetLibraryTables list;
        list.set_type( LT_FOOTPRINT );
        list.set_scope( LTS_PROJECT );

        GetLibraryTablesResponse tables;
        BOOST_REQUIRE_MESSAGE( Send( Client(), list, &tables, &error ), error );
        BOOST_CHECK_EQUAL( tables.rows_size(), 2 );

        RemoveLibraryTableRow remove;
        remove.set_type( LT_FOOTPRINT );
        remove.set_scope( LTS_PROJECT );
        remove.set_nickname( "qa_alias" );

        google::protobuf::Empty empty;
        BOOST_REQUIRE_MESSAGE( Send( Client(), remove, &empty, &error ), error );
        BOOST_CHECK_EQUAL( SendStatus( Client(), remove ), kiapi::common::AS_BAD_REQUEST );

        BOOST_REQUIRE_MESSAGE( Send( Client(), list, &tables, &error ), error );
        BOOST_REQUIRE_EQUAL( tables.rows_size(), 1 );
        BOOST_CHECK_EQUAL( tables.rows( 0 ).nickname(), "qa_fp" );

        // The table was written to disk
        wxFileName tableFile( project.Dir(), wxS( "fp-lib-table" ) );
        BOOST_CHECK( tableFile.FileExists() );
    }

    BOOST_REQUIRE( Client().CloseAllDocuments() );
}


BOOST_FIXTURE_TEST_CASE( SymbolLibraryRoundTrip, API_SERVER_E2E_FIXTURE )
{
    BOOST_REQUIRE_MESSAGE( Start(), LastError() );

    TEMP_PROJECT project;
    BOOST_REQUIRE( project.Create() );

    kiapi::common::types::DocumentSpecifier document;
    BOOST_REQUIRE_MESSAGE( Client().OpenDocument( project.ProjectPath(), kiapi::common::types::DOCTYPE_PROJECT,
                                                  &document ),
                           "OpenDocument failed: " + Client().LastError() );

    wxString error;

    {
        CreateLibrary request;
        request.set_type( LT_SYMBOL );
        request.set_nickname( "qa_sym" );
        request.set_scope( LTS_PROJECT );

        LibraryTableRow row;
        BOOST_REQUIRE_MESSAGE( Send( Client(), request, &row, &error ), error );
        BOOST_CHECK( wxFileName::FileExists( wxString::FromUTF8( row.resolved_uri() ) ) );
    }

    // A minimal symbol: mandatory fields and one pin
    {
        SaveLibraryItem request;
        request.set_type( LT_SYMBOL );
        request.mutable_id()->set_library_nickname( "qa_sym" );
        request.mutable_id()->set_entry_name( "QA_R" );

        kiapi::schematic::types::SchematicSymbol* symbol = request.mutable_item()->mutable_symbol();
        symbol->mutable_reference_field()->mutable_text()->set_text( "R" );
        symbol->mutable_reference_field()->set_name( "Reference" );
        symbol->mutable_value_field()->mutable_text()->set_text( "QA_R" );
        symbol->mutable_value_field()->set_name( "Value" );
        symbol->mutable_description_field()->mutable_text()->set_text( "QA resistor" );
        symbol->mutable_description_field()->set_name( "Description" );
        symbol->set_unit_count( 1 );
        symbol->set_keywords( "qa" );
        symbol->add_footprint_filters( "R_*" );

        kiapi::schematic::types::SchematicPin pin;
        pin.set_number( "1" );
        pin.set_name( "A" );
        pin.mutable_length()->set_value_nm( 2540000 );

        kiapi::schematic::types::SchematicSymbolChild* child = symbol->add_items();
        child->mutable_unit()->set_unit( 1 );
        child->mutable_body_style()->set_style( 1 );
        child->mutable_item()->PackFrom( pin );

        SaveLibraryItemResponse response;
        BOOST_REQUIRE_MESSAGE( Send( Client(), request, &response, &error ), error );
        BOOST_CHECK_EQUAL( response.id().entry_name(), "QA_R" );
    }

    {
        ListLibraryEntries request;
        request.set_type( LT_SYMBOL );
        request.set_nickname( "qa_sym" );

        ListLibraryEntriesResponse response;
        BOOST_REQUIRE_MESSAGE( Send( Client(), request, &response, &error ), error );
        BOOST_REQUIRE_EQUAL( response.entries_size(), 1 );
        BOOST_CHECK_EQUAL( response.entries( 0 ).name(), "QA_R" );
        BOOST_CHECK_EQUAL( response.entries( 0 ).description(), "QA resistor" );
        BOOST_CHECK_EQUAL( response.entries( 0 ).symbol().unit_count(), 1 );
        BOOST_REQUIRE_EQUAL( response.entries( 0 ).symbol().footprint_filters_size(), 1 );
        BOOST_CHECK_EQUAL( response.entries( 0 ).symbol().footprint_filters( 0 ), "R_*" );
    }

    {
        GetLibraryItem request;
        request.set_type( LT_SYMBOL );
        request.mutable_id()->set_library_nickname( "qa_sym" );
        request.mutable_id()->set_entry_name( "QA_R" );

        GetLibraryItemResponse response;
        BOOST_REQUIRE_MESSAGE( Send( Client(), request, &response, &error ), error );
        BOOST_REQUIRE( response.item().has_symbol() );

        int pins = 0;

        for( const kiapi::schematic::types::SchematicSymbolChild& child : response.item().symbol().items() )
        {
            if( child.item().Is<kiapi::schematic::types::SchematicPin>() )
                ++pins;
        }

        BOOST_CHECK_EQUAL( pins, 1 );
        BOOST_CHECK_EQUAL( response.item().symbol().keywords(), "qa" );
    }

    {
        DeleteLibraryItem request;
        request.set_type( LT_SYMBOL );
        request.mutable_id()->set_library_nickname( "qa_sym" );
        request.mutable_id()->set_entry_name( "QA_R" );

        google::protobuf::Empty response;
        BOOST_REQUIRE_MESSAGE( Send( Client(), request, &response, &error ), error );

        ListLibraryEntries list;
        list.set_type( LT_SYMBOL );
        list.set_nickname( "qa_sym" );

        ListLibraryEntriesResponse entries;
        BOOST_REQUIRE_MESSAGE( Send( Client(), list, &entries, &error ), error );
        BOOST_CHECK_EQUAL( entries.entries_size(), 0 );
    }

    BOOST_REQUIRE( Client().CloseAllDocuments() );
}


BOOST_AUTO_TEST_SUITE_END()
