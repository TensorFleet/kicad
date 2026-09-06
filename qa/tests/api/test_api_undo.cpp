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
 * End-to-end tests for Undo, Redo and GetUndoStack on headless documents.  Since 11.0.
 */

#include <boost/test/unit_test.hpp>
#include <google/protobuf/empty.pb.h>
#include <wx/filefn.h>
#include <wx/filename.h>

#include "api_e2e_utils.h"

#include <api/board/board_commands.pb.h>
#include <api/board/board_types.pb.h>
#include <api/common/commands/editor_commands.pb.h>
#include <api/schematic/schematic_types.pb.h>
#include <footprint.h>

using namespace kiapi::common::commands;
using kiapi::common::types::DocumentSpecifier;


namespace
{

/// A throw-away project folder holding a copy of the kitchen sink project, board and schematic
class TEMP_UNDO_PROJECT
{
public:
    ~TEMP_UNDO_PROJECT()
    {
        if( !m_dir.IsEmpty() && wxFileName::DirExists( m_dir ) )
            wxFileName::Rmdir( m_dir, wxPATH_RMDIR_RECURSIVE );
    }

    bool Create()
    {
        wxString token = wxFileName::CreateTempFileName( wxS( "kicad-api-undo-" ) );

        if( token.IsEmpty() )
            return false;

        wxRemoveFile( token );

        if( !wxFileName::Mkdir( token, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) )
            return false;

        m_dir = token;

        wxString pcbData = wxString::FromUTF8( KI_TEST::GetPcbnewTestDataDir() );
        wxString schData = wxString::FromUTF8( KI_TEST::GetEeschemaTestDataDir() );

        for( const wxString& ext : { wxS( "kicad_pro" ), wxS( "kicad_pcb" ), wxS( "kicad_dru" ) } )
        {
            wxFileName src( pcbData, wxS( "api_kitchen_sink." ) + ext );

            if( !wxCopyFile( src.GetFullPath(), wxFileName( m_dir, src.GetFullName() ).GetFullPath(), true ) )
                return false;
        }

        wxFileName sch( schData, wxS( "api_kitchen_sink.kicad_sch" ) );

        if( !wxCopyFile( sch.GetFullPath(), wxFileName( m_dir, sch.GetFullName() ).GetFullPath(), true ) )
            return false;

        m_projectPath = wxFileName( m_dir, wxS( "api_kitchen_sink.kicad_pro" ) ).GetFullPath();
        m_boardPath = wxFileName( m_dir, wxS( "api_kitchen_sink.kicad_pcb" ) ).GetFullPath();
        m_schematicPath = wxFileName( m_dir, wxS( "api_kitchen_sink.kicad_sch" ) ).GetFullPath();
        return true;
    }

    const wxString& ProjectPath() const { return m_projectPath; }
    const wxString& BoardPath() const { return m_boardPath; }
    const wxString& SchematicPath() const { return m_schematicPath; }

private:
    wxString m_dir;
    wxString m_projectPath;
    wxString m_boardPath;
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


bool GetUndoStackOf( API_TEST_CLIENT& aClient, const DocumentSpecifier& aDoc, UndoStackResponse* aOut,
                     wxString* aError )
{
    GetUndoStack request;
    *request.mutable_document() = aDoc;
    return Send( aClient, request, aOut, aError );
}

} // namespace


BOOST_FIXTURE_TEST_CASE( UndoRedoBoard, API_SERVER_E2E_FIXTURE )
{
    BOOST_REQUIRE_MESSAGE( Start(), LastError() );

    TEMP_UNDO_PROJECT project;
    BOOST_REQUIRE( project.Create() );

    DocumentSpecifier projectDoc;
    DocumentSpecifier board;
    BOOST_REQUIRE_MESSAGE( Client().OpenDocument( project.ProjectPath(), kiapi::common::types::DOCTYPE_PROJECT,
                                                  &projectDoc ),
                           "OpenDocument failed: " + Client().LastError() );
    BOOST_REQUIRE_MESSAGE( Client().OpenDocument( project.BoardPath(), kiapi::common::types::DOCTYPE_PCB, &board ),
                           "OpenDocument failed: " + Client().LastError() );

    wxString error;

    // A freshly opened board has no history
    UndoStackResponse stack;
    BOOST_REQUIRE_MESSAGE( GetUndoStackOf( Client(), board, &stack, &error ), error );
    BOOST_CHECK_EQUAL( stack.undo_size(), 0 );
    BOOST_CHECK_EQUAL( stack.redo_size(), 0 );

    // Move the drill origin
    kiapi::board::commands::GetBoardOrigin getOrigin;
    *getOrigin.mutable_board() = board;
    getOrigin.set_type( kiapi::board::commands::BOT_DRILL );

    kiapi::common::types::Vector2 originBefore;
    BOOST_REQUIRE_MESSAGE( Send( Client(), getOrigin, &originBefore, &error ), error );

    kiapi::board::commands::SetBoardOrigin setOrigin;
    *setOrigin.mutable_board() = board;
    setOrigin.set_type( kiapi::board::commands::BOT_DRILL );
    setOrigin.mutable_origin()->set_x_nm( originBefore.x_nm() + 12000000 );
    setOrigin.mutable_origin()->set_y_nm( originBefore.y_nm() + 34000000 );

    google::protobuf::Empty empty;
    BOOST_REQUIRE_MESSAGE( Send( Client(), setOrigin, &empty, &error ), error );

    // Move a footprint through a commit
    FOOTPRINT footprint( nullptr );
    BOOST_REQUIRE_MESSAGE( Client().GetFirstFootprint( board, &footprint ), Client().LastError() );

    const VECTOR2I positionBefore = footprint.GetPosition();

    kiapi::board::types::FootprintInstance instance;
    {
        google::protobuf::Any any;
        footprint.Serialize( any );
        BOOST_REQUIRE( any.UnpackTo( &instance ) );
    }

    instance.mutable_position()->set_x_nm( positionBefore.x + 5000000 );
    instance.mutable_position()->set_y_nm( positionBefore.y + 7000000 );

    BeginCommitResponse begun;
    BOOST_REQUIRE_MESSAGE( Send( Client(), BeginCommit(), &begun, &error ), error );

    // An open commit refuses undo even before anything is staged in it
    Undo emptyCommitUndo;
    *emptyCommitUndo.mutable_document() = board;
    BOOST_CHECK_EQUAL( SendStatus( Client(), emptyCommitUndo ), kiapi::common::AS_BUSY );

    UpdateItems update;
    *update.mutable_header()->mutable_document() = board;
    update.add_items()->PackFrom( instance );

    UpdateItemsResponse updated;
    BOOST_REQUIRE_MESSAGE( Send( Client(), update, &updated, &error ), error );
    BOOST_REQUIRE_EQUAL( updated.updated_items_size(), 1 );
    BOOST_CHECK_EQUAL( updated.updated_items( 0 ).status().code(), ISC_OK );

    // Not while the commit is open
    Undo undo;
    *undo.mutable_document() = board;
    BOOST_CHECK_EQUAL( SendStatus( Client(), undo ), kiapi::common::AS_BUSY );

    EndCommit end;
    *end.mutable_id() = begun.id();
    end.set_action( CMA_COMMIT );
    end.set_message( "Move footprint" );

    EndCommitResponse ended;
    BOOST_REQUIRE_MESSAGE( Send( Client(), end, &ended, &error ), error );

    BOOST_REQUIRE_MESSAGE( GetUndoStackOf( Client(), board, &stack, &error ), error );
    BOOST_REQUIRE_EQUAL( stack.undo_size(), 2 );
    BOOST_CHECK_EQUAL( stack.undo( 1 ).description(), "Move footprint" );
    BOOST_CHECK_EQUAL( stack.undo( 1 ).commit_id().value(), begun.id().value() );
    BOOST_CHECK( !stack.undo( 1 ).client_name().empty() );
    BOOST_CHECK_GE( stack.undo( 1 ).item_count(), 1 );

    BOOST_REQUIRE_MESSAGE( Client().GetFirstFootprint( board, &footprint ), Client().LastError() );
    BOOST_CHECK( footprint.GetPosition() != positionBefore );

    // Undo both
    undo.set_count( 2 );

    UndoRedoResponse response;
    BOOST_REQUIRE_MESSAGE( Send( Client(), undo, &response, &error ), error );
    BOOST_CHECK_EQUAL( response.applied(), 2 );
    BOOST_CHECK_EQUAL( response.undo_count(), 0 );
    BOOST_CHECK_EQUAL( response.redo_count(), 2 );

    BOOST_REQUIRE_MESSAGE( Client().GetFirstFootprint( board, &footprint ), Client().LastError() );
    BOOST_CHECK( footprint.GetPosition() == positionBefore );

    kiapi::common::types::Vector2 originAfter;
    BOOST_REQUIRE_MESSAGE( Send( Client(), getOrigin, &originAfter, &error ), error );
    BOOST_CHECK_EQUAL( originAfter.x_nm(), originBefore.x_nm() );
    BOOST_CHECK_EQUAL( originAfter.y_nm(), originBefore.y_nm() );

    // Redo brings both back; asking for more than there is applies what there is
    Redo redo;
    *redo.mutable_document() = board;
    redo.set_count( 5 );
    BOOST_REQUIRE_MESSAGE( Send( Client(), redo, &response, &error ), error );
    BOOST_CHECK_EQUAL( response.applied(), 2 );
    BOOST_CHECK_EQUAL( response.redo_count(), 0 );

    BOOST_REQUIRE_MESSAGE( Client().GetFirstFootprint( board, &footprint ), Client().LastError() );
    BOOST_CHECK( footprint.GetPosition() == VECTOR2I( positionBefore.x + 5000000, positionBefore.y + 7000000 ) );

    BOOST_REQUIRE_MESSAGE( Send( Client(), getOrigin, &originAfter, &error ), error );
    BOOST_CHECK_EQUAL( originAfter.x_nm(), originBefore.x_nm() + 12000000 );

    // A deleted footprint comes back with its id
    int footprintCount = 0;
    BOOST_REQUIRE( Client().GetItemsCount( board, kiapi::common::types::KOT_PCB_FOOTPRINT, &footprintCount ) );

    DeleteItems del;
    *del.mutable_header()->mutable_document() = board;
    del.add_item_ids()->set_value( footprint.m_Uuid.AsStdString() );

    DeleteItemsResponse deleted;
    BOOST_REQUIRE_MESSAGE( Send( Client(), del, &deleted, &error ), error );

    int afterDelete = 0;
    BOOST_REQUIRE( Client().GetItemsCount( board, kiapi::common::types::KOT_PCB_FOOTPRINT, &afterDelete ) );
    BOOST_CHECK_EQUAL( afterDelete, footprintCount - 1 );

    undo.set_count( 1 );
    BOOST_REQUIRE_MESSAGE( Send( Client(), undo, &response, &error ), error );
    BOOST_CHECK_EQUAL( response.applied(), 1 );

    int afterUndo = 0;
    BOOST_REQUIRE( Client().GetItemsCount( board, kiapi::common::types::KOT_PCB_FOOTPRINT, &afterUndo ) );
    BOOST_CHECK_EQUAL( afterUndo, footprintCount );

    GetItemsById byId;
    *byId.mutable_header()->mutable_document() = board;
    byId.add_items()->set_value( footprint.m_Uuid.AsStdString() );

    GetItemsResponse found;
    BOOST_REQUIRE_MESSAGE( Send( Client(), byId, &found, &error ), error );
    BOOST_CHECK_EQUAL( found.items_size(), 1 );
}


BOOST_FIXTURE_TEST_CASE( UndoRedoSchematic, API_SERVER_E2E_FIXTURE )
{
    BOOST_REQUIRE_MESSAGE( Start(), LastError() );

    TEMP_UNDO_PROJECT project;
    BOOST_REQUIRE( project.Create() );

    DocumentSpecifier projectDoc;
    DocumentSpecifier schematic;
    BOOST_REQUIRE_MESSAGE( Client().OpenDocument( project.ProjectPath(), kiapi::common::types::DOCTYPE_PROJECT,
                                                  &projectDoc ),
                           "OpenDocument failed: " + Client().LastError() );
    BOOST_REQUIRE_MESSAGE( Client().OpenDocument( project.SchematicPath(), kiapi::common::types::DOCTYPE_SCHEMATIC,
                                                  &schematic ),
                           "OpenDocument failed: " + Client().LastError() );

    wxString error;

    auto firstSymbol =
            [&]( kiapi::schematic::types::SchematicSymbolInstance* aOut ) -> bool
            {
                GetItems request;
                *request.mutable_header()->mutable_document() = schematic;
                request.add_types( kiapi::common::types::KOT_SCH_SYMBOL );

                GetItemsResponse response;

                if( !Send( Client(), request, &response, &error ) || response.items_size() == 0 )
                    return false;

                return response.items( 0 ).UnpackTo( aOut );
            };

    kiapi::schematic::types::SchematicSymbolInstance symbol;
    BOOST_REQUIRE_MESSAGE( firstSymbol( &symbol ), error );

    const std::string id = symbol.id().value();
    const int64_t     xBefore = symbol.position().x_nm();

    symbol.mutable_position()->set_x_nm( xBefore + 2540000 );

    UpdateItems update;
    *update.mutable_header()->mutable_document() = schematic;
    update.add_items()->PackFrom( symbol );

    UpdateItemsResponse updated;
    BOOST_REQUIRE_MESSAGE( Send( Client(), update, &updated, &error ), error );
    BOOST_REQUIRE_EQUAL( updated.updated_items_size(), 1 );
    BOOST_CHECK_EQUAL( updated.updated_items( 0 ).status().code(), ISC_OK );

    UndoStackResponse stack;
    BOOST_REQUIRE_MESSAGE( GetUndoStackOf( Client(), schematic, &stack, &error ), error );
    BOOST_REQUIRE_EQUAL( stack.undo_size(), 1 );
    BOOST_CHECK( !stack.undo( 0 ).client_name().empty() );

    Undo undo;
    *undo.mutable_document() = schematic;

    UndoRedoResponse response;
    BOOST_REQUIRE_MESSAGE( Send( Client(), undo, &response, &error ), error );
    BOOST_CHECK_EQUAL( response.applied(), 1 );

    BOOST_REQUIRE_MESSAGE( firstSymbol( &symbol ), error );
    BOOST_CHECK_EQUAL( symbol.position().x_nm(), xBefore );

    // Delete and bring back
    int countBefore = 0;
    BOOST_REQUIRE( Client().GetItemsCount( schematic, kiapi::common::types::KOT_SCH_SYMBOL, &countBefore ) );

    DeleteItems del;
    *del.mutable_header()->mutable_document() = schematic;
    del.add_item_ids()->set_value( id );

    DeleteItemsResponse deleted;
    BOOST_REQUIRE_MESSAGE( Send( Client(), del, &deleted, &error ), error );

    int afterDelete = 0;
    BOOST_REQUIRE( Client().GetItemsCount( schematic, kiapi::common::types::KOT_SCH_SYMBOL, &afterDelete ) );
    BOOST_CHECK_EQUAL( afterDelete, countBefore - 1 );

    BOOST_REQUIRE_MESSAGE( Send( Client(), undo, &response, &error ), error );
    BOOST_CHECK_EQUAL( response.applied(), 1 );

    int afterUndo = 0;
    BOOST_REQUIRE( Client().GetItemsCount( schematic, kiapi::common::types::KOT_SCH_SYMBOL, &afterUndo ) );
    BOOST_CHECK_EQUAL( afterUndo, countBefore );

    // Nothing left to undo
    undo.set_count( 3 );
    BOOST_REQUIRE_MESSAGE( Send( Client(), undo, &response, &error ), error );
    BOOST_CHECK_EQUAL( response.applied(), 0 );
    BOOST_CHECK_EQUAL( response.undo_count(), 0 );
    BOOST_CHECK_EQUAL( response.redo_count(), 1 );
}
