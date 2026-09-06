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
#include <api/board/board_jobs.pb.h>
#include <api/board/board_types.pb.h>
#include <api/common/commands/base_commands.pb.h>
#include <api/common/commands/editor_commands.pb.h>
#include <footprint.h>
#include <wx/file.h>
#include <wx/utils.h>

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

        for( const wxString& ext : { wxS( "kicad_pro" ), wxS( "kicad_pcb" ), wxS( "kicad_dru" ) } )
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
        BOOST_CHECK_GT( response.edges( 0 ).net().code().value(), 0 );
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

        // The count is of what changed, so removing again reports nothing
        BOOST_REQUIRE_MESSAGE( Send( Client(), remove, &response, &error ), error );
        BOOST_CHECK_EQUAL( response.item_count(), 0u );
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


/**
 * DRC must not run while the job registry's worker thread is busy: an asynchronous job shares
 * the document's PROJECT, its footprint library adapter and the KiCad thread pool with the
 * checker, which then deletes and rebuilds the board's markers underneath it.  The web app saw
 * RunBoardJobDrc never answer at all once a session had run its asynchronous exports.
 */
BOOST_FIXTURE_TEST_CASE( BoardOpsDrcAfterAsyncJobs, API_SERVER_E2E_FIXTURE )
{
    BOOST_REQUIRE_MESSAGE( Start(), LastError() );

    TEMP_BOARD_PROJECT project;
    DocumentSpecifier  board;
    BOOST_REQUIRE_MESSAGE( OpenKitchenSinkBoard( *this, project, &board ),
                           "OpenDocument failed: " + Client().LastError() );

    wxString error;

    // Queue several asynchronous exports and do not wait for them
    std::vector<std::string> jobIds;
    std::vector<wxString>    outputs;

    for( int i = 0; i < 4; ++i )
    {
        wxString   tempFile = wxFileName::CreateTempFileName( wxS( "api_drc_async_svg_" ) );
        wxFileName outputPath( tempFile );
        outputPath.SetExt( wxS( "svg" ) );
        outputs.push_back( tempFile );
        outputs.push_back( outputPath.GetFullPath() );

        kiapi::board::jobs::RunBoardJobExportSvg request;
        *request.mutable_job_settings()->mutable_document() = board;
        request.mutable_job_settings()->set_output_path( outputPath.GetFullPath().ToUTF8().data() );
        request.mutable_job_settings()->set_async( true );
        request.mutable_plot_settings()->add_layers( kiapi::board::types::BL_F_Cu );
        request.mutable_plot_settings()->add_layers( kiapi::board::types::BL_B_Cu );
        request.mutable_plot_settings()->add_layers( kiapi::board::types::BL_Edge_Cuts );

        kiapi::common::types::RunJobResponse started;
        BOOST_REQUIRE_MESSAGE( Client().RunJob( request, &started ), "RunJob failed: " + Client().LastError() );
        BOOST_REQUIRE_EQUAL( started.status(), kiapi::common::types::JS_RUNNING );
        BOOST_REQUIRE( !started.job_id().empty() );
        jobIds.push_back( started.job_id() );
    }

    // DRC answers, and only once the queue has drained
    RunBoardJobDrc run;
    *run.mutable_board() = board;

    DrcResultsResponse results;
    BOOST_REQUIRE_MESSAGE( Send( Client(), run, &results, &error ), error );
    BOOST_CHECK_GT( results.markers_size(), 0 );

    for( const std::string& jobId : jobIds )
    {
        GetJobStatus statusRequest;
        statusRequest.set_job_id( jobId );

        GetJobStatusResponse status;
        BOOST_REQUIRE_MESSAGE( Send( Client(), statusRequest, &status, &error ), error );
        BOOST_CHECK_EQUAL( status.state(), JOB_STATE_FINISHED );
    }

    // And the markers survive, so the client can read them back
    GetDrcMarkers get;
    *get.mutable_board() = board;

    DrcResultsResponse after;
    BOOST_REQUIRE_MESSAGE( Send( Client(), get, &after, &error ), error );
    BOOST_CHECK_EQUAL( after.markers_size(), results.markers_size() );

    for( const wxString& path : outputs )
    {
        if( wxFileName::DirExists( path ) )
            wxFileName::Rmdir( path, wxPATH_RMDIR_RECURSIVE );
        else if( wxFileName::FileExists( path ) )
            wxRemoveFile( path );
    }
}


/// RunBoardJobDrc honors RunJobSettings.async: the check is queued on the job worker, the
/// document is closed to other commands while it runs, and GetJobStatus reports the outcome.
BOOST_FIXTURE_TEST_CASE( BoardOpsDrcAsync, API_SERVER_E2E_FIXTURE )
{
    BOOST_REQUIRE_MESSAGE( Start(), LastError() );

    TEMP_BOARD_PROJECT project;
    DocumentSpecifier  board;
    BOOST_REQUIRE_MESSAGE( OpenKitchenSinkBoard( *this, project, &board ),
                           "OpenDocument failed: " + Client().LastError() );

    wxString error;

    // A synchronous run for the expected marker count
    RunBoardJobDrc run;
    *run.mutable_board() = board;

    DrcResultsResponse expected;
    BOOST_REQUIRE_MESSAGE( Send( Client(), run, &expected, &error ), error );
    BOOST_REQUIRE_GT( expected.markers_size(), 0 );
    BOOST_CHECK( expected.job().job_id().empty() );

    // The same check, asynchronously
    run.mutable_job_settings()->set_async( true );

    DrcResultsResponse started;
    BOOST_REQUIRE_MESSAGE( Send( Client(), run, &started, &error ), error );
    BOOST_CHECK_EQUAL( started.markers_size(), 0 );
    BOOST_REQUIRE_EQUAL( started.job().status(), kiapi::common::types::JS_RUNNING );
    BOOST_REQUIRE( !started.job().job_id().empty() );

    // The board is off limits until the checker has rebuilt its markers
    GetDrcMarkers get;
    *get.mutable_board() = board;
    BOOST_CHECK_EQUAL( SendStatus( Client(), get ), kiapi::common::AS_BUSY );

    GetJobStatus statusRequest;
    statusRequest.set_job_id( started.job().job_id() );

    GetJobStatusResponse status;

    for( int attempt = 0; attempt < 1200; ++attempt )
    {
        BOOST_REQUIRE_MESSAGE( Send( Client(), statusRequest, &status, &error ), error );

        if( status.state() == JOB_STATE_FINISHED )
            break;

        wxMilliSleep( 50 );
    }

    BOOST_REQUIRE_EQUAL( status.state(), JOB_STATE_FINISHED );
    BOOST_REQUIRE_MESSAGE( status.result().status() == kiapi::common::types::JS_SUCCESS,
                           "DRC job failed: " + wxString::FromUTF8( status.result().message() ) );
    BOOST_CHECK_EQUAL( status.job_id(), started.job().job_id() );
    BOOST_CHECK_EQUAL( status.percent(), 100 );

    // And the markers it placed are the ones the synchronous run found
    DrcResultsResponse after;
    BOOST_REQUIRE_MESSAGE( Send( Client(), get, &after, &error ), error );
    BOOST_CHECK_EQUAL( after.markers_size(), expected.markers_size() );
    BOOST_CHECK_EQUAL( after.error_count(), expected.error_count() );
    BOOST_CHECK_EQUAL( after.warning_count(), expected.warning_count() );
}

namespace
{

/// A session an autorouter could have produced from the kitchen sink's DSN: one routed net
/// with a via, a wire on a net the board does not have, and a wire on a layer it does not have
const char* KITCHEN_SINK_SESSION =
        "(session api_kitchen_sink\n"
        "  (base_design api_kitchen_sink.dsn)\n"
        "  (routes\n"
        "    (resolution um 10)\n"
        "    (library_out\n"
        "      (padstack \"Via[0-1]_600:300_um\"\n"
        "        (shape (circle F.Cu 6000 0 0))\n"
        "        (shape (circle B.Cu 6000 0 0))\n"
        "        (attach off)\n"
        "      )\n"
        "    )\n"
        "    (network_out\n"
        "      (net A\n"
        "        (wire (path F.Cu 2500 1000000 -600000 1010000 -600000 1010000 -610000) (type route))\n"
        "        (via \"Via[0-1]_600:300_um\" 1010000 -610000)\n"
        "      )\n"
        "      (net NoSuchNet\n"
        "        (wire (path B.Cu 2500 1000000 -700000 1020000 -700000))\n"
        "      )\n"
        "      (net A\n"
        "        (wire (path NoSuch.Cu 2500 1000000 -700000 1020000 -700000))\n"
        "      )\n"
        "    )\n"
        "  )\n"
        ")\n";


int CountTracksAndVias( API_TEST_CLIENT& aClient, const DocumentSpecifier& aDoc )
{
    return CountItems( aClient, aDoc, kiapi::common::types::KOT_PCB_TRACE )
           + CountItems( aClient, aDoc, kiapi::common::types::KOT_PCB_ARC )
           + CountItems( aClient, aDoc, kiapi::common::types::KOT_PCB_VIA );
}


uint64_t DocumentRevision( API_TEST_CLIENT& aClient, const DocumentSpecifier& aDoc )
{
    GetDocumentRevision request;
    *request.mutable_document() = aDoc;

    DocumentRevisionResponse response;
    wxString                 error;

    if( !Send( aClient, request, &response, &error ) )
        return 0;

    return response.revision();
}

} // namespace


/// ImportSpecctraSession merges a router's session into the board as one undoable commit,
/// from a file or from memory, keeping or replacing the tracks that are already there.
BOOST_FIXTURE_TEST_CASE( BoardOpsImportSpecctraSession, API_SERVER_E2E_FIXTURE )
{
    BOOST_REQUIRE_MESSAGE( Start(), LastError() );

    TEMP_BOARD_PROJECT project;
    DocumentSpecifier  board;
    BOOST_REQUIRE_MESSAGE( OpenKitchenSinkBoard( *this, project, &board ),
                           "OpenDocument failed: " + Client().LastError() );

    wxString error;

    const int      tracksBefore = CountItems( Client(), board, kiapi::common::types::KOT_PCB_TRACE );
    const int      viasBefore = CountItems( Client(), board, kiapi::common::types::KOT_PCB_VIA );
    const int      allBefore = CountTracksAndVias( Client(), board );
    const uint64_t revisionBefore = DocumentRevision( Client(), board );
    BOOST_REQUIRE_GT( tracksBefore, 0 );

    // Neither a path nor contents
    {
        ImportSpecctraSession request;
        *request.mutable_board() = board;
        BOOST_CHECK_EQUAL( SendStatus( Client(), request ), kiapi::common::AS_BAD_REQUEST );

        request.set_path( "/nonexistent/routed.ses" );
        BOOST_CHECK_EQUAL( SendStatus( Client(), request ), kiapi::common::AS_BAD_REQUEST );
    }

    // A session without a library_out is rejected and leaves the board alone
    {
        ImportSpecctraSession request;
        *request.mutable_board() = board;
        request.set_contents( "(session x (routes (resolution um 10) (network_out)))" );
        request.set_replace_existing_tracks( true );
        BOOST_CHECK_EQUAL( SendStatus( Client(), request ), kiapi::common::AS_BAD_REQUEST );
        BOOST_CHECK_EQUAL( CountTracksAndVias( Client(), board ), allBefore );
        BOOST_CHECK_EQUAL( DocumentRevision( Client(), board ), revisionBefore );
    }

    // From memory, adding to the existing routing
    {
        ImportSpecctraSession request;
        *request.mutable_board() = board;
        request.set_contents( KITCHEN_SINK_SESSION );

        ImportSpecctraSessionResponse response;
        BOOST_REQUIRE_MESSAGE( Send( Client(), request, &response, &error ), error );

        // The three-point path is two tracks, the unknown net's wire lands on no net
        BOOST_CHECK_EQUAL( response.tracks_added(), 3 );
        BOOST_CHECK_EQUAL( response.vias_added(), 1 );
        BOOST_CHECK_EQUAL( response.tracks_removed(), 0 );
        BOOST_CHECK_EQUAL( response.footprints_moved(), 0 );
        BOOST_REQUIRE_EQUAL( response.warnings_size(), 1 );
        BOOST_CHECK( response.warnings( 0 ).find( "1 session item" ) != std::string::npos );

        BOOST_CHECK_EQUAL( CountItems( Client(), board, kiapi::common::types::KOT_PCB_TRACE ), tracksBefore + 3 );
        BOOST_CHECK_EQUAL( CountItems( Client(), board, kiapi::common::types::KOT_PCB_VIA ), viasBefore + 1 );
        BOOST_CHECK_GT( DocumentRevision( Client(), board ), revisionBefore );
    }

    // The import is one undo step
    {
        Undo undo;
        *undo.mutable_document() = board;

        UndoRedoResponse response;
        BOOST_REQUIRE_MESSAGE( Send( Client(), undo, &response, &error ), error );
        BOOST_CHECK_EQUAL( response.applied(), 1 );
        BOOST_CHECK_EQUAL( CountTracksAndVias( Client(), board ), allBefore );
    }

    // From a file, replacing the unlocked routing
    {
        wxString sessionPath = wxFileName::CreateTempFileName( wxS( "kicad-api-session-" ) );
        {
            wxFile file( sessionPath, wxFile::write );
            BOOST_REQUIRE( file.IsOpened() );
            file.Write( wxString::FromUTF8( KITCHEN_SINK_SESSION ) );
        }

        ImportSpecctraSession request;
        *request.mutable_board() = board;
        request.set_path( sessionPath.ToUTF8().data() );
        request.set_replace_existing_tracks( true );

        ImportSpecctraSessionResponse response;
        BOOST_REQUIRE_MESSAGE( Send( Client(), request, &response, &error ), error );
        wxRemoveFile( sessionPath );

        BOOST_CHECK_EQUAL( response.tracks_added(), 3 );
        BOOST_CHECK_EQUAL( response.vias_added(), 1 );
        BOOST_CHECK_GT( response.tracks_removed(), 0 );
        BOOST_CHECK_EQUAL( CountTracksAndVias( Client(), board ),
                           allBefore - static_cast<int>( response.tracks_removed() ) + 4 );
    }
}
