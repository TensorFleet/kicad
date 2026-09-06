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
 * End-to-end tests for the schematic operation commands (Annotate, ClearAnnotation,
 * SyncSchematicToBoard, Get/SetSchematicSettings, GetSymbolFieldsTable, SetSymbolFields,
 * AssignFootprints, and sheet file creation through CreateItems).  Since 11.0.
 */

#include <boost/test/unit_test.hpp>
#include <google/protobuf/empty.pb.h>
#include <wx/ffile.h>
#include <wx/filefn.h>
#include <wx/filename.h>

#include "api_e2e_utils.h"

#include <api/common/commands/editor_commands.pb.h>
#include <api/common/commands/project_commands.pb.h>
#include <api/schematic/schematic_commands.pb.h>
#include <api/schematic/schematic_types.pb.h>

using namespace kiapi::common::commands;
using namespace kiapi::schematic::commands;
using kiapi::common::types::DocumentSpecifier;


namespace
{

/// A throw-away project folder holding a copy of the kitchen sink project and schematic
class TEMP_SCH_PROJECT
{
public:
    ~TEMP_SCH_PROJECT()
    {
        if( !m_dir.IsEmpty() && wxFileName::DirExists( m_dir ) )
            wxFileName::Rmdir( m_dir, wxPATH_RMDIR_RECURSIVE );
    }

    bool Create()
    {
        wxString token = wxFileName::CreateTempFileName( wxS( "kicad-api-sch-" ) );

        if( token.IsEmpty() )
            return false;

        wxRemoveFile( token );

        if( !wxFileName::Mkdir( token, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) )
            return false;

        m_dir = token;

        wxFileName srcPro( wxString::FromUTF8( KI_TEST::GetPcbnewTestDataDir() ), wxS( "api_kitchen_sink.kicad_pro" ) );
        wxFileName srcSch( wxString::FromUTF8( KI_TEST::GetEeschemaTestDataDir() ), wxS( "api_kitchen_sink.kicad_sch" ) );
        wxFileName dstPro( m_dir, srcPro.GetFullName() );
        wxFileName dstSch( m_dir, srcSch.GetFullName() );

        if( !wxCopyFile( srcPro.GetFullPath(), dstPro.GetFullPath(), true )
            || !wxCopyFile( srcSch.GetFullPath(), dstSch.GetFullPath(), true ) )
        {
            return false;
        }

        m_projectPath = dstPro.GetFullPath();
        m_schematicPath = dstSch.GetFullPath();

        return createFootprintLibraries();
    }

private:
    /**
     * Write the footprint libraries the kitchen sink schematic names, so that a sync to a board
     * can actually place them.  A stub footprint per entry is enough: the sync only has to load
     * it.  Generating them here keeps the fixture beside the test that needs it.
     */
    bool createFootprintLibraries()
    {
        static const std::pair<wxString, wxString> entries[] = {
            { wxS( "Connector_Audio" ), wxS( "Jack_3.5mm_CUI_SJ-3524-SMT_Horizontal" ) },
            { wxS( "Package_SO" ), wxS( "SOIC-8_3.9x4.9mm_P1.27mm" ) }
        };

        wxString table = wxS( "(fp_lib_table\n  (version 7)\n" );

        for( const auto& [nickname, footprint] : entries )
        {
            wxFileName dir( m_dir, wxEmptyString );
            dir.AppendDir( nickname + wxS( ".pretty" ) );

            if( !wxFileName::Mkdir( dir.GetPath(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) )
                return false;

            wxFileName modFile( dir.GetPath(), footprint + wxS( ".kicad_mod" ) );
            wxFFile    mod( modFile.GetFullPath(), wxS( "w" ) );

            if( !mod.IsOpened() )
                return false;

            wxString body;
            body << wxS( "(footprint \"" ) << footprint << wxS( "\"\n" )
                 << wxS( "\t(version 20240108)\n" )
                 << wxS( "\t(generator \"qa\")\n" )
                 << wxS( "\t(generator_version \"9.0\")\n" )
                 << wxS( "\t(layer \"F.Cu\")\n" )
                 << wxS( "\t(attr smd)\n" )
                 << wxS( "\t(pad \"1\" smd rect (at 0 0) (size 1 1) (layers \"F.Cu\" \"F.Paste\" \"F.Mask\"))\n" )
                 << wxS( "\t(pad \"2\" smd rect (at 2 0) (size 1 1) (layers \"F.Cu\" \"F.Paste\" \"F.Mask\"))\n" )
                 << wxS( ")\n" );

            if( !mod.Write( body ) )
                return false;

            mod.Close();

            table << wxS( "  (lib (name \"" ) << nickname << wxS( "\") (type \"KiCad\") (uri \"${KIPRJMOD}/" )
                  << nickname << wxS( ".pretty\") (options \"\") (descr \"\"))\n" );
        }

        table << wxS( ")\n" );

        wxFFile out( wxFileName( m_dir, wxS( "fp-lib-table" ) ).GetFullPath(), wxS( "w" ) );

        return out.IsOpened() && out.Write( table );
    }

public:
    const wxString& Dir() const { return m_dir; }
    const wxString& ProjectPath() const { return m_projectPath; }
    const wxString& SchematicPath() const { return m_schematicPath; }

private:
    wxString m_dir;
    wxString m_projectPath;
    wxString m_schematicPath;
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


bool OpenKitchenSink( API_SERVER_E2E_FIXTURE& aFixture, TEMP_SCH_PROJECT& aProject, DocumentSpecifier* aDoc )
{
    if( !aProject.Create() )
        return false;

    DocumentSpecifier projectDoc;

    if( !aFixture.Client().OpenDocument( aProject.ProjectPath(), kiapi::common::types::DOCTYPE_PROJECT, &projectDoc ) )
        return false;

    return aFixture.Client().OpenDocument( aProject.SchematicPath(), kiapi::common::types::DOCTYPE_SCHEMATIC, aDoc );
}


bool FieldsTable( API_TEST_CLIENT& aClient, const DocumentSpecifier& aDoc, SymbolFieldsTableResponse* aOut,
                  wxString* aError )
{
    GetSymbolFieldsTable request;
    *request.mutable_schematic() = aDoc;
    return Send( aClient, request, aOut, aError );
}

} // namespace


BOOST_FIXTURE_TEST_CASE( SchematicOpsAnnotateAndFields, API_SERVER_E2E_FIXTURE )
{
    BOOST_REQUIRE_MESSAGE( Start(), LastError() );

    TEMP_SCH_PROJECT  project;
    DocumentSpecifier doc;
    BOOST_REQUIRE_MESSAGE( OpenKitchenSink( *this, project, &doc ), "OpenDocument failed: " + Client().LastError() );

    wxString error;

    // The fields table lists every symbol with its mandatory fields
    SymbolFieldsTableResponse table;
    BOOST_REQUIRE_MESSAGE( FieldsTable( Client(), doc, &table, &error ), error );
    BOOST_REQUIRE_EQUAL( table.rows_size(), 5 );

    for( const SymbolFieldsRow& row : table.rows() )
    {
        BOOST_CHECK( row.fields().contains( "Reference" ) );
        BOOST_CHECK( row.fields().contains( "Value" ) );
        BOOST_CHECK( row.fields().contains( "Footprint" ) );
        BOOST_CHECK_EQUAL( row.fields().at( "Reference" ), row.reference() );
    }

    // A field filter restricts the map
    {
        GetSymbolFieldsTable request;
        *request.mutable_schematic() = doc;
        request.add_fields( "Value" );

        SymbolFieldsTableResponse filtered;
        BOOST_REQUIRE_MESSAGE( Send( Client(), request, &filtered, &error ), error );
        BOOST_REQUIRE_EQUAL( filtered.rows_size(), 5 );

        for( const SymbolFieldsRow& row : filtered.rows() )
            BOOST_CHECK_EQUAL( row.fields().size(), 1 );
    }

    // Clear every reference, then annotate from 100 sorted by X
    {
        ClearAnnotation request;
        *request.mutable_schematic() = doc;
        request.set_scope( ANS_ALL );

        AnnotateResponse response;
        BOOST_REQUIRE_MESSAGE( Send( Client(), request, &response, &error ), error );
        BOOST_CHECK_EQUAL( response.annotated_count(), 5 );

        BOOST_REQUIRE_MESSAGE( FieldsTable( Client(), doc, &table, &error ), error );

        for( const SymbolFieldsRow& row : table.rows() )
            BOOST_CHECK( row.reference().ends_with( "?" ) );
    }

    {
        Annotate request;
        *request.mutable_schematic() = doc;
        request.set_scope( ANS_ALL );
        request.mutable_options()->set_sort_order( ASO_X_POSITION );
        request.mutable_options()->set_start_number( 100 );

        AnnotateResponse response;
        BOOST_REQUIRE_MESSAGE( Send( Client(), request, &response, &error ), error );
        BOOST_CHECK_EQUAL( response.annotated_count(), 5 );
        BOOST_CHECK_EQUAL( response.symbol_count(), 5 );
        BOOST_CHECK_EQUAL( response.error_count(), 0 );

        BOOST_REQUIRE_MESSAGE( FieldsTable( Client(), doc, &table, &error ), error );

        bool sawFirst = false;

        for( const SymbolFieldsRow& row : table.rows() )
        {
            BOOST_CHECK( !row.reference().ends_with( "?" ) );
            sawFirst |= row.reference().ends_with( "101" );
        }

        BOOST_CHECK( sawFirst );

        // Annotating again without a reset changes nothing
        request.clear_options();
        BOOST_REQUIRE_MESSAGE( Send( Client(), request, &response, &error ), error );
        BOOST_CHECK_EQUAL( response.annotated_count(), 0 );
    }

    // Selection scope: one symbol
    const SymbolFieldsRow first = table.rows( 0 );

    {
        ClearAnnotation request;
        *request.mutable_schematic() = doc;
        request.set_scope( ANS_SELECTION );
        *request.add_items() = first.id();

        AnnotateResponse response;
        BOOST_REQUIRE_MESSAGE( Send( Client(), request, &response, &error ), error );
        BOOST_CHECK_EQUAL( response.annotated_count(), 1 );

        Annotate annotate;
        *annotate.mutable_schematic() = doc;
        annotate.set_scope( ANS_SELECTION );
        *annotate.add_items() = first.id();
        BOOST_REQUIRE_MESSAGE( Send( Client(), annotate, &response, &error ), error );
        BOOST_CHECK_EQUAL( response.annotated_count(), 1 );
        BOOST_CHECK_EQUAL( response.error_count(), 0 );

        annotate.clear_items();
        BOOST_CHECK_EQUAL( SendStatus( Client(), annotate ), kiapi::common::AS_BAD_REQUEST );
    }

    // Bulk field edits: value, a new user field, a refused mandatory removal, an unknown symbol
    {
        SetSymbolFields request;
        *request.mutable_schematic() = doc;

        SymbolFieldUpdate* value = request.add_updates();
        *value->mutable_id() = first.id();
        value->set_field( "Value" );
        value->set_value( "QA_VALUE" );

        SymbolFieldUpdate* user = request.add_updates();
        *user->mutable_id() = first.id();
        user->set_field( "MPN" );
        user->set_value( "ABC-123" );

        SymbolFieldUpdate* mandatory = request.add_updates();
        *mandatory->mutable_id() = first.id();
        mandatory->set_field( "Reference" );
        mandatory->set_remove( true );

        SymbolFieldUpdate* unknown = request.add_updates();
        unknown->mutable_id()->set_value( "00000000-0000-0000-0000-000000000001" );
        unknown->set_field( "Value" );
        unknown->set_value( "x" );

        SetSymbolFieldsResponse response;
        BOOST_REQUIRE_MESSAGE( Send( Client(), request, &response, &error ), error );
        BOOST_CHECK_EQUAL( response.updated_count(), 2 );
        BOOST_CHECK_EQUAL( response.errors_size(), 2 );

        BOOST_REQUIRE_MESSAGE( FieldsTable( Client(), doc, &table, &error ), error );

        for( const SymbolFieldsRow& row : table.rows() )
        {
            if( row.id().value() != first.id().value() )
                continue;

            BOOST_CHECK_EQUAL( row.fields().at( "Value" ), "QA_VALUE" );
            BOOST_REQUIRE( row.fields().contains( "MPN" ) );
            BOOST_CHECK_EQUAL( row.fields().at( "MPN" ), "ABC-123" );
        }
    }

    // Footprint assignment by reference
    {
        // The selection-scope pass above re-annotated this symbol, so the reference captured
        // with the row is stale; assignment matches on the current one
        std::string firstReference;

        for( const SymbolFieldsRow& row : table.rows() )
        {
            if( row.id().value() == first.id().value() )
                firstReference = row.reference();
        }

        BOOST_REQUIRE( !firstReference.empty() );

        AssignFootprints request;
        *request.mutable_schematic() = doc;

        FootprintAssignment* good = request.add_assignments();
        good->set_reference( firstReference );
        good->mutable_footprint()->set_library_nickname( "Resistor_SMD" );
        good->mutable_footprint()->set_entry_name( "R_0603_1608Metric" );

        FootprintAssignment* bad = request.add_assignments();
        bad->set_reference( "ZZ99" );
        bad->mutable_footprint()->set_library_nickname( "X" );
        bad->mutable_footprint()->set_entry_name( "Y" );

        AssignFootprintsResponse response;
        BOOST_REQUIRE_MESSAGE( Send( Client(), request, &response, &error ), error );
        BOOST_CHECK_EQUAL( response.assigned_count(), 1 );
        BOOST_REQUIRE_EQUAL( response.unmatched_references_size(), 1 );
        BOOST_CHECK_EQUAL( response.unmatched_references( 0 ), "ZZ99" );

        BOOST_REQUIRE_MESSAGE( FieldsTable( Client(), doc, &table, &error ), error );

        for( const SymbolFieldsRow& row : table.rows() )
        {
            if( row.id().value() == first.id().value() )
                BOOST_CHECK_EQUAL( row.fields().at( "Footprint" ), "Resistor_SMD:R_0603_1608Metric" );
        }
    }
}


BOOST_FIXTURE_TEST_CASE( SchematicOpsSettings, API_SERVER_E2E_FIXTURE )
{
    BOOST_REQUIRE_MESSAGE( Start(), LastError() );

    TEMP_SCH_PROJECT  project;
    DocumentSpecifier doc;
    BOOST_REQUIRE_MESSAGE( OpenKitchenSink( *this, project, &doc ), "OpenDocument failed: " + Client().LastError() );

    wxString error;

    GetSchematicSettings get;
    *get.mutable_schematic() = doc;

    SchematicSettings before;
    BOOST_REQUIRE_MESSAGE( Send( Client(), get, &before, &error ), error );
    BOOST_CHECK_GT( before.default_line_width().value_nm(), 0 );
    BOOST_CHECK_GT( before.default_text_size().value_nm(), 0 );
    BOOST_CHECK_EQUAL( before.subpart_first_id(), "A" );

    // Only the sent fields change
    SetSchematicSettings set;
    *set.mutable_schematic() = doc;
    set.mutable_settings()->set_junction_size_choice( 4 );
    set.mutable_settings()->set_intersheet_refs_prefix( "[" );
    set.mutable_settings()->mutable_default_line_width()->set_value_nm( 254000 );

    SchematicSettings after;
    BOOST_REQUIRE_MESSAGE( Send( Client(), set, &after, &error ), error );
    BOOST_CHECK_EQUAL( after.junction_size_choice(), 4 );
    BOOST_CHECK_EQUAL( after.intersheet_refs_prefix(), "[" );
    BOOST_CHECK_EQUAL( after.default_line_width().value_nm(), 254000 );
    BOOST_CHECK_EQUAL( after.default_text_size().value_nm(), before.default_text_size().value_nm() );
    BOOST_CHECK_EQUAL( after.annotate_start_number(), before.annotate_start_number() );

    // Out-of-range values are refused
    set.mutable_settings()->Clear();
    set.mutable_settings()->set_junction_size_choice( 9 );
    BOOST_CHECK_EQUAL( SendStatus( Client(), set ), kiapi::common::AS_BAD_REQUEST );

    set.mutable_settings()->Clear();
    set.mutable_settings()->set_subpart_first_id( "B" );
    BOOST_CHECK_EQUAL( SendStatus( Client(), set ), kiapi::common::AS_BAD_REQUEST );
}


BOOST_FIXTURE_TEST_CASE( SchematicOpsSyncToBoard, API_SERVER_E2E_FIXTURE )
{
    BOOST_REQUIRE_MESSAGE( Start(), LastError() );

    TEMP_SCH_PROJECT  project;
    DocumentSpecifier doc;
    BOOST_REQUIRE_MESSAGE( OpenKitchenSink( *this, project, &doc ), "OpenDocument failed: " + Client().LastError() );

    wxString error;

    // A fresh board in the project
    NewDocument newDocument;
    newDocument.set_type( kiapi::common::types::DOCTYPE_PCB );

    OpenDocumentResponse created;
    BOOST_REQUIRE_MESSAGE( Send( Client(), newDocument, &created, &error ), error );

    const DocumentSpecifier& board = created.document();

    SyncSchematicToBoard sync;
    *sync.mutable_schematic() = doc;
    *sync.mutable_board() = board;

    SyncSchematicToBoardResponse response;
    BOOST_REQUIRE_MESSAGE( Send( Client(), sync, &response, &error ), error );

    // The kitchen sink has two symbols with footprints; the others are reported, not added
    BOOST_CHECK_EQUAL( response.result().new_footprint_count(), 2 );
    BOOST_CHECK( !response.result().report().empty() );
    BOOST_CHECK( !response.netlist_path().empty() );
    BOOST_CHECK( !wxFileExists( wxString::FromUTF8( response.netlist_path() ) ) );

    int footprintCount = 0;
    BOOST_REQUIRE_MESSAGE( Client().GetItemsCount( board, kiapi::common::types::KOT_PCB_FOOTPRINT, &footprintCount ),
                           "GetItems for board failed: " + Client().LastError() );
    BOOST_CHECK_EQUAL( footprintCount, 2 );

    // A dry run reports without changing the board
    sync.set_dry_run( true );
    BOOST_REQUIRE_MESSAGE( Send( Client(), sync, &response, &error ), error );
    BOOST_REQUIRE_MESSAGE( Client().GetItemsCount( board, kiapi::common::types::KOT_PCB_FOOTPRINT, &footprintCount ),
                           "GetItems for board failed: " + Client().LastError() );
    BOOST_CHECK_EQUAL( footprintCount, 2 );

    // A board that is not open is refused
    sync.set_dry_run( false );
    sync.mutable_board()->set_board_filename( "nope.kicad_pcb" );
    BOOST_CHECK_EQUAL( SendStatus( Client(), sync ), kiapi::common::AS_BAD_REQUEST );

    // So is an unannotated schematic
    SymbolFieldsTableResponse table;
    BOOST_REQUIRE_MESSAGE( FieldsTable( Client(), doc, &table, &error ), error );
    BOOST_REQUIRE_GT( table.rows_size(), 0 );

    ClearAnnotation clear;
    *clear.mutable_schematic() = doc;
    clear.set_scope( ANS_SELECTION );
    *clear.add_items() = table.rows( 0 ).id();

    AnnotateResponse cleared;
    BOOST_REQUIRE_MESSAGE( Send( Client(), clear, &cleared, &error ), error );

    *sync.mutable_board() = board;
    BOOST_CHECK_EQUAL( SendStatus( Client(), sync ), kiapi::common::AS_BAD_REQUEST );
}


BOOST_FIXTURE_TEST_CASE( SchematicOpsCreateSheetFile, API_SERVER_E2E_FIXTURE )
{
    BOOST_REQUIRE_MESSAGE( Start(), LastError() );

    TEMP_SCH_PROJECT  project;
    DocumentSpecifier doc;
    BOOST_REQUIRE_MESSAGE( OpenKitchenSink( *this, project, &doc ), "OpenDocument failed: " + Client().LastError() );

    wxString error;

    auto makeSheet =
            []( const std::string& aName, const std::string& aFile, int64_t aX )
            {
                kiapi::schematic::types::SheetSymbol sheet;
                sheet.mutable_position()->set_x_nm( aX );
                sheet.mutable_position()->set_y_nm( 100000000 );
                sheet.mutable_size()->set_x_nm( 30000000 );
                sheet.mutable_size()->set_y_nm( 20000000 );
                sheet.mutable_name_field()->set_name( "Sheetname" );
                sheet.mutable_name_field()->mutable_text()->set_text( aName );
                sheet.mutable_filename_field()->set_name( "Sheetfile" );
                sheet.mutable_filename_field()->mutable_text()->set_text( aFile );
                return sheet;
            };

    // A sheet naming a file that does not exist creates it
    {
        CreateItems request;
        *request.mutable_header()->mutable_document() = doc;
        request.add_items()->PackFrom( makeSheet( "Power", "power.kicad_sch", 100000000 ) );

        CreateItemsResponse response;
        BOOST_REQUIRE_MESSAGE( Send( Client(), request, &response, &error ), error );
        BOOST_REQUIRE_EQUAL( response.created_items_size(), 1 );
        BOOST_CHECK_MESSAGE( response.created_items( 0 ).status().code() == ISC_OK,
                             response.created_items( 0 ).status().error_message() );

        wxFileName created( project.Dir(), wxS( "power.kicad_sch" ) );
        BOOST_CHECK( created.FileExists() );
    }

    // It shows up in the hierarchy
    {
        GetSchematicHierarchy request;
        *request.mutable_document() = doc;

        SchematicHierarchyResponse response;
        BOOST_REQUIRE_MESSAGE( Send( Client(), request, &response, &error ), error );
        BOOST_REQUIRE_EQUAL( response.top_level_sheets_size(), 1 );

        bool found = false;

        for( const kiapi::schematic::types::SheetInstance& child : response.top_level_sheets( 0 ).children() )
            found |= child.name() == "Power" && child.filename() == "power.kicad_sch";

        BOOST_CHECK( found );
    }

    // A second sheet on the same file shares it; a sheet without a file name is refused
    {
        CreateItems request;
        *request.mutable_header()->mutable_document() = doc;
        request.add_items()->PackFrom( makeSheet( "Power2", "power.kicad_sch", 150000000 ) );
        request.add_items()->PackFrom( makeSheet( "NoFile", "", 200000000 ) );

        CreateItemsResponse response;
        BOOST_REQUIRE_MESSAGE( Send( Client(), request, &response, &error ), error );
        BOOST_REQUIRE_EQUAL( response.created_items_size(), 2 );
        BOOST_CHECK_EQUAL( response.created_items( 0 ).status().code(), ISC_OK );
        BOOST_CHECK_EQUAL( response.created_items( 1 ).status().code(), ISC_INVALID_DATA );
    }

    // Saving writes the sheet reference into the root schematic
    {
        SaveDocument request;
        *request.mutable_document() = doc;
        google::protobuf::Empty empty;
        BOOST_REQUIRE_MESSAGE( Send( Client(), request, &empty, &error ), error );

        wxFFile file( project.SchematicPath(), wxS( "rb" ) );
        wxString contents;
        BOOST_REQUIRE( file.IsOpened() && file.ReadAll( &contents ) );
        BOOST_CHECK( contents.Contains( wxS( "\"power.kicad_sch\"" ) ) );
    }
}
