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
 * End-to-end tests for the board operation commands (GetRatsnest, GetUnroutedCount,
 * GetNetLengths, UpdateFootprintsFromLibrary, SetTeardrops / RemoveTeardrops,
 * AutoplaceFootprints, GlobalDeletion).  Since 11.0.
 */

#include <boost/test/unit_test.hpp>
#include <wx/filefn.h>
#include <wx/filename.h>

#include "api_e2e_utils.h"

#include <api/board/board_commands.pb.h>
#include <api/board/board_types.pb.h>
#include <api/common/commands/editor_commands.pb.h>
#include <footprint.h>

using namespace kiapi::common::commands;
using namespace kiapi::board::commands;
using kiapi::common::types::DocumentSpecifier;
using kiapi::common::types::KiCadObjectType;


namespace
{

/// A throw-away project folder holding a copy of the kitchen sink project and board
class TEMP_BOARD_PROJECT
{
public:
    ~TEMP_BOARD_PROJECT()
    {
        if( !m_dir.IsEmpty() && wxFileName::DirExists( m_dir ) )
            wxFileName::Rmdir( m_dir, wxPATH_RMDIR_RECURSIVE );
    }

    bool Create()
    {
        wxString token = wxFileName::CreateTempFileName( wxS( "kicad-api-board-" ) );

        if( token.IsEmpty() )
            return false;

        wxRemoveFile( token );

        if( !wxFileName::Mkdir( token, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) )
            return false;

        m_dir = token;

        wxString testDataDir = wxString::FromUTF8( KI_TEST::GetPcbnewTestDataDir() );

        for( const wxString& ext : { wxS( "kicad_pro" ), wxS( "kicad_pcb" ), wxS( "kicad_dru" ), wxS( "kicad_prl" ) } )
        {
            wxFileName src( testDataDir, wxS( "api_kitchen_sink." ) + ext );
            wxFileName dst( m_dir, src.GetFullName() );

            if( !wxCopyFile( src.GetFullPath(), dst.GetFullPath(), true ) )
                return false;
        }

        m_projectPath = wxFileName( m_dir, wxS( "api_kitchen_sink.kicad_pro" ) ).GetFullPath();
        m_boardPath = wxFileName( m_dir, wxS( "api_kitchen_sink.kicad_pcb" ) ).GetFullPath();
        return true;
    }

    const wxString& ProjectPath() const { return m_projectPath; }
    const wxString& BoardPath() const { return m_boardPath; }

private:
    wxString m_dir;
    wxString m_projectPath;
    wxString m_boardPath;
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


bool OpenKitchenSinkBoard( API_SERVER_E2E_FIXTURE& aFixture, TEMP_BOARD_PROJECT& aProject, DocumentSpecifier* aDoc )
{
    if( !aProject.Create() )
        return false;

    DocumentSpecifier projectDoc;

    if( !aFixture.Client().OpenDocument( aProject.ProjectPath(), kiapi::common::types::DOCTYPE_PROJECT, &projectDoc ) )
        return false;

    return aFixture.Client().OpenDocument( aProject.BoardPath(), kiapi::common::types::DOCTYPE_PCB, aDoc );
}


int CountItems( API_TEST_CLIENT& aClient, const DocumentSpecifier& aDoc, KiCadObjectType aType )
{
    int count = -1;
    aClient.GetItemsCount( aDoc, aType, &count );
    return count;
}

} // namespace


BOOST_FIXTURE_TEST_CASE( BoardOpsRatsnestAndLengths, API_SERVER_E2E_FIXTURE )
{
    BOOST_REQUIRE_MESSAGE( Start(), LastError() );

    TEMP_BOARD_PROJECT project;
    DocumentSpecifier  board;
    BOOST_REQUIRE_MESSAGE( OpenKitchenSinkBoard( *this, project, &board ), "OpenDocument failed: " + Client().LastError() );

    wxString error;

    // The kitchen sink is fully routed
    {
        GetUnroutedCount request;
        *request.mutable_board() = board;

        UnroutedCountResponse response;
        BOOST_REQUIRE_MESSAGE( Send( Client(), request, &response, &error ), error );
        BOOST_CHECK_EQUAL( response.unrouted_count(), 0 );
        BOOST_CHECK_EQUAL( response.unrouted_net_count(), 0 );
    }

    // Net A has two pads joined by a track
    {
        GetNetLengths request;
        *request.mutable_board() = board;
        request.add_nets()->set_name( "A" );

        NetLengthsResponse response;
        BOOST_REQUIRE_MESSAGE( Send( Client(), request, &response, &error ), error );
        BOOST_REQUIRE_EQUAL( response.lengths_size(), 1 );
        BOOST_CHECK_EQUAL( response.lengths( 0 ).net().name(), "A" );
        BOOST_CHECK_EQUAL( response.lengths( 0 ).pad_count(), 2 );
        BOOST_CHECK_GT( response.lengths( 0 ).track_length().value_nm(), 0 );
        BOOST_CHECK_EQUAL( response.lengths( 0 ).total_length().value_nm(),
                           response.lengths( 0 ).track_length().value_nm()
                                   + response.lengths( 0 ).via_length().value_nm()
                                   + response.lengths( 0 ).pad_to_die_length().value_nm() );
        BOOST_CHECK_EQUAL( response.lengths( 0 ).unrouted_length().value_nm(), 0 );
        BOOST_CHECK_GT( response.lengths( 0 ).layer_lengths_size(), 0 );

        request.clear_nets();
        request.add_nets()->set_name( "NO_SUCH_NET" );
        BOOST_CHECK_EQUAL( SendStatus( Client(), request ), kiapi::common::AS_BAD_REQUEST );
    }

    // Deleting the tracks leaves an airline on net A
    {
        GlobalDeletion request;
        *request.mutable_board() = board;
        request.add_types( kiapi::common::types::KOT_PCB_TRACE );
        request.add_types( kiapi::common::types::KOT_PCB_ARC );
        request.add_types( kiapi::common::types::KOT_PCB_VIA );

        GlobalDeletionResponse response;
        BOOST_REQUIRE_MESSAGE( Send( Client(), request, &response, &error ), error );
        BOOST_CHECK_GT( response.deleted_count(), 0 );
        BOOST_CHECK_EQUAL( CountItems( Client(), board, kiapi::common::types::KOT_PCB_TRACE ), 0 );
    }

    {
        GetRatsnest request;
        *request.mutable_board() = board;

        RatsnestResponse response;
        BOOST_REQUIRE_MESSAGE( Send( Client(), request, &response, &error ), error );
        BOOST_REQUIRE_EQUAL( response.edges_size(), 1 );
        BOOST_CHECK_EQUAL( response.unrouted_count(), 1 );
        BOOST_CHECK_EQUAL( response.edges( 0 ).net().name(), "A" );
        BOOST_CHECK( !response.edges( 0 ).source().value().empty() );
        BOOST_CHECK( !response.edges( 0 ).target().value().empty() );
        BOOST_CHECK_GT( response.edges( 0 ).length().value_nm(), 0 );

        request.add_nets()->set_name( "A" );
        BOOST_REQUIRE_MESSAGE( Send( Client(), request, &response, &error ), error );
        BOOST_CHECK_EQUAL( response.edges_size(), 1 );

        GetNetLengths lengths;
        *lengths.mutable_board() = board;
        lengths.add_nets()->set_name( "A" );

        NetLengthsResponse lengthsResponse;
        BOOST_REQUIRE_MESSAGE( Send( Client(), lengths, &lengthsResponse, &error ), error );
        BOOST_REQUIRE_EQUAL( lengthsResponse.lengths_size(), 1 );
        BOOST_CHECK_EQUAL( lengthsResponse.lengths( 0 ).track_length().value_nm(), 0 );
        BOOST_CHECK_EQUAL( lengthsResponse.lengths( 0 ).unrouted_length().value_nm(),
                           response.edges( 0 ).length().value_nm() );
    }
}


BOOST_FIXTURE_TEST_CASE( BoardOpsTeardropsAndFootprints, API_SERVER_E2E_FIXTURE )
{
    BOOST_REQUIRE_MESSAGE( Start(), LastError() );

    TEMP_BOARD_PROJECT project;
    DocumentSpecifier  board;
    BOOST_REQUIRE_MESSAGE( OpenKitchenSinkBoard( *this, project, &board ), "OpenDocument failed: " + Client().LastError() );

    wxString error;

    const int padCount = CountItems( Client(), board, kiapi::common::types::KOT_PCB_PAD );
    const int viaCount = CountItems( Client(), board, kiapi::common::types::KOT_PCB_VIA );
    BOOST_REQUIRE_GT( padCount, 0 );

    // Enabling teardrops touches every copper pad and via; removing them again too
    {
        SetTeardrops request;
        *request.mutable_board() = board;
        request.set_action( TDA_ADD );

        SetTeardropsResponse response;
        BOOST_REQUIRE_MESSAGE( Send( Client(), request, &response, &error ), error );
        BOOST_CHECK_EQUAL( response.item_count(), static_cast<uint32_t>( padCount + viaCount ) );

        request.set_action( TDA_SET );
        BOOST_CHECK_EQUAL( SendStatus( Client(), request ), kiapi::common::AS_BAD_REQUEST );

        request.mutable_settings()->set_mode( kiapi::board::types::PTM_ENABLED );
        request.mutable_settings()->set_curved_edges( true );
        request.set_vias( true );
        BOOST_REQUIRE_MESSAGE( Send( Client(), request, &response, &error ), error );
        BOOST_CHECK_EQUAL( response.item_count(), static_cast<uint32_t>( viaCount ) );

        RemoveTeardrops remove;
        *remove.mutable_board() = board;
        BOOST_REQUIRE_MESSAGE( Send( Client(), remove, &response, &error ), error );
        BOOST_CHECK_EQUAL( response.item_count(), static_cast<uint32_t>( padCount + viaCount ) );
    }

    // Updating from the library accounts for every footprint; the two without a library are missing
    {
        UpdateFootprintsFromLibrary request;
        *request.mutable_board() = board;
        request.set_only_changed( true );

        UpdateFootprintsFromLibraryResponse response;
        BOOST_REQUIRE_MESSAGE( Send( Client(), request, &response, &error ), error );
        BOOST_CHECK_EQUAL( response.updated_count() + response.unchanged_count() + response.missing_size(),
                           static_cast<uint32_t>( CountItems( Client(), board, kiapi::common::types::KOT_PCB_FOOTPRINT ) ) );

        std::set<std::string> missing( response.missing().begin(), response.missing().end() );
        BOOST_CHECK( missing.contains( "D1" ) );
        BOOST_CHECK( missing.contains( "P2" ) );
        BOOST_CHECK_EQUAL( response.messages_size(), 6 );

        request.mutable_new_footprint()->set_library_nickname( "Resistor_SMD" );
        BOOST_CHECK_EQUAL( SendStatus( Client(), request ), kiapi::common::AS_BAD_REQUEST );

        request.add_footprints()->set_value( "00000000-0000-0000-0000-000000000001" );
        request.mutable_new_footprint()->set_entry_name( "R_0805_2012Metric" );
        BOOST_CHECK_EQUAL( SendStatus( Client(), request ), kiapi::common::AS_BAD_REQUEST );
    }
}


BOOST_FIXTURE_TEST_CASE( BoardOpsAutoplaceAndGlobalDeletion, API_SERVER_E2E_FIXTURE )
{
    BOOST_REQUIRE_MESSAGE( Start(), LastError() );

    TEMP_BOARD_PROJECT project;
    DocumentSpecifier  board;
    BOOST_REQUIRE_MESSAGE( OpenKitchenSinkBoard( *this, project, &board ), "OpenDocument failed: " + Client().LastError() );

    wxString error;

    // One footprint, by id
    FOOTPRINT footprint( nullptr );
    BOOST_REQUIRE_MESSAGE( Client().GetFirstFootprint( board, &footprint ), Client().LastError() );

    {
        AutoplaceFootprints request;
        *request.mutable_board() = board;
        request.add_footprints()->set_value( footprint.m_Uuid.AsStdString() );

        AutoplaceFootprintsResponse response;
        BOOST_REQUIRE_MESSAGE( Send( Client(), request, &response, &error ), error );
        BOOST_CHECK_EQUAL( response.result(), APR_COMPLETED );
        BOOST_CHECK_EQUAL( response.placed_count(), 1 );

        request.clear_footprints();
        request.add_footprints()->set_value( "00000000-0000-0000-0000-000000000001" );
        BOOST_CHECK_EQUAL( SendStatus( Client(), request ), kiapi::common::AS_BAD_REQUEST );
    }

    // Texts go, shapes keep the outline unless asked, and the outline last
    {
        const int textCount = CountItems( Client(), board, kiapi::common::types::KOT_PCB_TEXT );
        BOOST_REQUIRE_GT( textCount, 0 );

        GlobalDeletion request;
        *request.mutable_board() = board;
        BOOST_CHECK_EQUAL( SendStatus( Client(), request ), kiapi::common::AS_BAD_REQUEST );

        request.add_types( kiapi::common::types::KOT_PCB_TEXT );

        GlobalDeletionResponse response;
        BOOST_REQUIRE_MESSAGE( Send( Client(), request, &response, &error ), error );
        BOOST_CHECK_EQUAL( response.deleted_count(), static_cast<uint32_t>( textCount ) );
        BOOST_CHECK_EQUAL( CountItems( Client(), board, kiapi::common::types::KOT_PCB_TEXT ), 0 );

        const int shapeCount = CountItems( Client(), board, kiapi::common::types::KOT_PCB_SHAPE );
        BOOST_REQUIRE_GT( shapeCount, 0 );

        request.clear_types();
        request.add_types( kiapi::common::types::KOT_PCB_SHAPE );
        BOOST_REQUIRE_MESSAGE( Send( Client(), request, &response, &error ), error );

        const int outlineCount = CountItems( Client(), board, kiapi::common::types::KOT_PCB_SHAPE );
        BOOST_CHECK_GT( outlineCount, 0 );
        BOOST_CHECK_EQUAL( response.deleted_count(), static_cast<uint32_t>( shapeCount - outlineCount ) );

        request.set_board_edges( true );
        BOOST_REQUIRE_MESSAGE( Send( Client(), request, &response, &error ), error );
        BOOST_CHECK_EQUAL( response.deleted_count(), static_cast<uint32_t>( outlineCount ) );
        BOOST_CHECK_EQUAL( CountItems( Client(), board, kiapi::common::types::KOT_PCB_SHAPE ), 0 );

        // Without an outline the autoplacer has nowhere to place
        AutoplaceFootprints autoplace;
        *autoplace.mutable_board() = board;

        AutoplaceFootprintsResponse autoplaceResponse;
        BOOST_REQUIRE_MESSAGE( Send( Client(), autoplace, &autoplaceResponse, &error ), error );
        BOOST_CHECK_EQUAL( autoplaceResponse.result(), APR_NO_BOARD_OUTLINE );
    }

    // Locked filter and the rest of the footprints
    {
        GlobalDeletion request;
        *request.mutable_board() = board;
        request.add_types( kiapi::common::types::KOT_PCB_FOOTPRINT );
        request.set_locked( LF_LOCKED );

        GlobalDeletionResponse response;
        BOOST_REQUIRE_MESSAGE( Send( Client(), request, &response, &error ), error );

        request.set_locked( LF_ALL );
        BOOST_REQUIRE_MESSAGE( Send( Client(), request, &response, &error ), error );
        BOOST_CHECK_EQUAL( CountItems( Client(), board, kiapi::common::types::KOT_PCB_FOOTPRINT ), 0 );
    }
}
