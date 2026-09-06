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
 * Tests for KICAD_API_SERVER behaviour that does not need a socket: command discovery.
 */

#include <chrono>
#include <map>
#include <memory>
#include <string>

#include <boost/test/unit_test.hpp>

#include <qa_utils/wx_utils/unit_test_utils.h>
#include <pcbnew_utils/board_test_utils.h>

#include <api/api_handler_common.h>
#include <api/api_job_registry.h>
#include <progress_reporter.h>
#include <api/api_handler_pcb.h>
#include <api/api_server.h>
#include <api/headless_pcb_context.h>
#include <api/common/commands/base_commands.pb.h>
#include <api/common/commands/editor_commands.pb.h>
#include <api/common/envelope.pb.h>

#include <board.h>
#include <ki_exception.h>
#include <settings/settings_manager.h>


namespace
{

using kiapi::common::commands::GetSupportedCommandsResponse;
using kiapi::common::commands::SupportedCommand;


std::string typeUrl( const google::protobuf::Message& aMessage )
{
    return "type.googleapis.com/" + std::string( aMessage.GetTypeName() );
}


std::map<std::string, SupportedCommand> byTypeUrl( const GetSupportedCommandsResponse& aResponse )
{
    std::map<std::string, SupportedCommand> map;

    for( const SupportedCommand& cmd : aResponse.commands() )
    {
        // Each command must be reported exactly once even when several handlers serve it
        BOOST_CHECK_MESSAGE( !map.contains( cmd.type_url() ), "duplicate entry for " << cmd.type_url() );
        map[cmd.type_url()] = cmd;
    }

    return map;
}


struct API_SERVER_FIXTURE
{
    SETTINGS_MANAGER                      m_settingsManager;
    std::unique_ptr<BOARD>                m_board;
    std::shared_ptr<HEADLESS_PCB_CONTEXT> m_context;

    // The server is constructed without starting the listener; SupportedCommands() and the
    // GetSupportedCommands handler only need the registered handler set.
    KICAD_API_SERVER   m_server{ false };
    API_HANDLER_COMMON m_commonHandler;

    void loadBoard( const wxString& aRelPath )
    {
        KI_TEST::LoadBoard( m_settingsManager, aRelPath, m_board );

        // LoadBoard does not name the board it read, and the document specifier a client sends
        // is matched against that name
        m_board->SetFileName( wxString::FromUTF8( KI_TEST::GetPcbnewTestDataDir() ) + aRelPath
                              + wxS( ".kicad_pcb" ) );

        m_context = std::make_shared<HEADLESS_PCB_CONTEXT>( std::move( m_board ), &m_settingsManager.Prj(),
                                                            nullptr );
    }
};

} // namespace


BOOST_FIXTURE_TEST_SUITE( ApiServer, API_SERVER_FIXTURE )


BOOST_AUTO_TEST_CASE( SupportedCommandsListsServerAndCommonHandlers )
{
    m_server.RegisterHandler( &m_commonHandler );

    std::map<std::string, SupportedCommand> commands = byTypeUrl( m_server.SupportedCommands() );

    // The discovery command itself is always served by the server
    std::string discovery = typeUrl( kiapi::common::commands::GetSupportedCommands() );
    BOOST_REQUIRE( commands.contains( discovery ) );
    BOOST_CHECK( commands[discovery].headless() );
    BOOST_CHECK_EQUAL( commands[discovery].response_type_url(), typeUrl( GetSupportedCommandsResponse() ) );

    // A plain common command, with its response type
    std::string getVersion = typeUrl( kiapi::common::commands::GetVersion() );
    BOOST_REQUIRE( commands.contains( getVersion ) );
    BOOST_CHECK( commands[getVersion].headless() );
    BOOST_CHECK_EQUAL( commands[getVersion].response_type_url(),
                       typeUrl( kiapi::common::commands::GetVersionResponse() ) );

    BOOST_CHECK( commands.contains( typeUrl( kiapi::common::commands::Ping() ) ) );

    // No board is open, so no board command is listed yet
    BOOST_CHECK( !commands.contains( typeUrl( kiapi::board::commands::RefillZones() ) ) );

    m_server.DeregisterHandler( &m_commonHandler );

    commands = byTypeUrl( m_server.SupportedCommands() );
    BOOST_CHECK( !commands.contains( getVersion ) );
    BOOST_CHECK( commands.contains( discovery ) );
}


BOOST_AUTO_TEST_CASE( SupportedCommandsReportsHeadlessCapability )
{
    loadBoard( wxS( "issue5830" ) );

    API_HANDLER_PCB pcbHandler( m_context );

    m_server.RegisterHandler( &m_commonHandler );
    m_server.RegisterHandler( &pcbHandler );

    std::map<std::string, SupportedCommand> commands = byTypeUrl( m_server.SupportedCommands() );

    // Board commands appear once a board handler is registered
    std::string refill = typeUrl( kiapi::board::commands::RefillZones() );
    BOOST_REQUIRE( commands.contains( refill ) );
    BOOST_CHECK( commands[refill].headless() );
    BOOST_CHECK_EQUAL( commands[refill].response_type_url(), typeUrl( google::protobuf::Empty() ) );

    // RunAction is headless-capable since 11.0: the actions whose tools work without a window
    // run in kicad-cli api-server, and the rest are refused per-action, not per-command
    std::string runAction = typeUrl( kiapi::common::commands::RunAction() );
    BOOST_REQUIRE( commands.contains( runAction ) );
    BOOST_CHECK( commands[runAction].headless() );

    // Commands that need an editor frame are listed but flagged as not available headless
    for( const std::string& guiOnly : { typeUrl( kiapi::common::commands::GetSelection() ),
                                        typeUrl( kiapi::common::commands::SaveSelectionToString() ),
                                        typeUrl( kiapi::board::commands::GetActiveLayer() ),
                                        typeUrl( kiapi::common::commands::RevertDocument() ) } )
    {
        BOOST_REQUIRE_MESSAGE( commands.contains( guiOnly ), guiOnly << " not listed" );
        BOOST_CHECK_MESSAGE( !commands[guiOnly].headless(), guiOnly << " should be GUI-only" );
    }

    // ExpandTextVariables is served by both the common and the board handler; it is listed once
    // and is headless-capable
    std::string expand = typeUrl( kiapi::common::commands::ExpandTextVariables() );
    BOOST_REQUIRE( commands.contains( expand ) );
    BOOST_CHECK( commands[expand].headless() );

    m_server.DeregisterHandler( &pcbHandler );
    m_server.DeregisterHandler( &m_commonHandler );
}


BOOST_AUTO_TEST_CASE( GetSupportedCommandsIsServedThroughHandlers )
{
    // The server's own handler answers the request like any other handler would: this mirrors
    // what a client sees over the socket.
    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    request.mutable_message()->PackFrom( kiapi::common::commands::GetSupportedCommands() );

    m_server.RegisterHandler( &m_commonHandler );

    // API_HANDLER_COMMON does not serve it...
    API_RESULT commonResult = m_commonHandler.Handle( request );
    BOOST_REQUIRE( !commonResult.has_value() );
    BOOST_CHECK_EQUAL( commonResult.error().status(), kiapi::common::ApiStatusCode::AS_UNHANDLED );

    // ...but the server's listing includes it, with the discovery response type
    GetSupportedCommandsResponse response = m_server.SupportedCommands();
    BOOST_CHECK_GT( response.commands_size(), 1 );

    std::map<std::string, SupportedCommand> commands = byTypeUrl( response );
    std::string discovery = typeUrl( kiapi::common::commands::GetSupportedCommands() );
    BOOST_REQUIRE( commands.contains( discovery ) );

    m_server.DeregisterHandler( &m_commonHandler );
}


// Since 11.0: handlers are consulted in registration order, and a handler must decline (with
// AS_UNHANDLED) a document it does not own so that the next handler can answer.
BOOST_AUTO_TEST_CASE( DispatchTriesHandlersInRegistrationOrder )
{
    loadBoard( wxS( "issue5830" ) );

    API_HANDLER_PCB pcbHandler( m_context );

    m_server.RegisterHandler( &m_commonHandler );
    m_server.RegisterHandler( &pcbHandler );

    // A command both editors serve, addressed to a schematic: the board handler must not answer
    // "document is not open" for it.
    kiapi::common::commands::GetTitleBlockInfo command;
    command.mutable_document()->set_type( kiapi::common::types::DOCTYPE_SCHEMATIC );
    command.mutable_document()->mutable_project()->set_name( "issue5830" );

    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    request.mutable_message()->PackFrom( command );

    API_RESULT result = m_server.Dispatch( request );
    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_UNHANDLED );

    // The same command for the open board is answered by the board handler
    command.mutable_document()->set_type( kiapi::common::types::DOCTYPE_PCB );
    command.mutable_document()->set_board_filename( "issue5830.kicad_pcb" );
    request.mutable_message()->PackFrom( command );

    result = m_server.Dispatch( request );
    BOOST_REQUIRE_MESSAGE( result.has_value(), result.error().error_message() );

    // A board that is not open is still a bad request, with a message
    command.mutable_document()->set_board_filename( "other.kicad_pcb" );
    request.mutable_message()->PackFrom( command );

    result = m_server.Dispatch( request );
    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_BAD_REQUEST );
    BOOST_CHECK( !result.error().error_message().empty() );

    m_server.DeregisterHandler( &pcbHandler );
    m_server.DeregisterHandler( &m_commonHandler );
}


// Since 11.0: GetOpenDocuments for an editor that is not running answers an empty list
BOOST_AUTO_TEST_CASE( GetOpenDocumentsAnswersEmptyWithoutEditor )
{
    m_server.RegisterHandler( &m_commonHandler );

    kiapi::common::commands::GetOpenDocuments command;
    command.set_type( kiapi::common::types::DOCTYPE_PCB );

    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    request.mutable_message()->PackFrom( command );

    API_RESULT result = m_server.Dispatch( request );
    BOOST_REQUIRE_MESSAGE( result.has_value(), result.error().error_message() );

    kiapi::common::commands::GetOpenDocumentsResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );
    BOOST_CHECK_EQUAL( response.documents_size(), 0 );

    // The fallback is advertised so that clients can rely on it
    std::map<std::string, SupportedCommand> commands = byTypeUrl( m_server.SupportedCommands() );
    BOOST_CHECK( commands.contains( typeUrl( command ) ) );

    // Once a board is open, its handler answers instead
    loadBoard( wxS( "issue5830" ) );
    API_HANDLER_PCB pcbHandler( m_context );
    m_server.RegisterHandler( &pcbHandler );

    result = m_server.Dispatch( request );
    BOOST_REQUIRE_MESSAGE( result.has_value(), result.error().error_message() );
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );
    BOOST_CHECK_EQUAL( response.documents_size(), 1 );

    m_server.DeregisterHandler( &pcbHandler );
    m_server.DeregisterHandler( &m_commonHandler );
}


namespace
{

/// A handler whose Ping throws, as a job that fails to load a kiface would
class THROWING_HANDLER : public API_HANDLER
{
public:
    THROWING_HANDLER() : API_HANDLER()
    {
        registerHandler<kiapi::common::commands::Ping, google::protobuf::Empty>( &THROWING_HANDLER::handlePing );
    }

private:
    HANDLER_RESULT<google::protobuf::Empty> handlePing( const HANDLER_CONTEXT<kiapi::common::commands::Ping>& )
    {
        THROW_IO_ERROR( wxS( "kiface missing" ) );
    }
};

} // namespace


// Since 11.0: an exception escaping a handler is reported to the client instead of leaving the
// request unanswered (which would block the request/reply socket for every later request)
BOOST_AUTO_TEST_CASE( DispatchReportsHandlerExceptions )
{
    THROWING_HANDLER throwing;
    m_server.RegisterHandler( &throwing );

    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    request.mutable_message()->PackFrom( kiapi::common::commands::Ping() );

    API_RESULT result = m_server.Dispatch( request );
    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_BAD_REQUEST );
    BOOST_CHECK( result.error().error_message().find( "kiface missing" ) != std::string::npos );

    m_server.DeregisterHandler( &throwing );
}


// Since 11.0: the job registry runs synchronous jobs in place and asynchronous ones on its
// worker, and answers GetJobStatus for both
BOOST_AUTO_TEST_CASE( JobRegistryRunsSyncAndAsyncJobs )
{
    API_JOB_REGISTRY& registry = API_JOB_REGISTRY::Instance();

    auto makeExecutor = []( const std::string& aOutput )
    {
        return [aOutput]( PROGRESS_REPORTER& aProgress ) -> kiapi::common::types::RunJobResponse
        {
            aProgress.Report( wxS( "working" ) );
            aProgress.SetCurrentProgress( 0.5 );

            kiapi::common::types::RunJobResponse result;
            result.set_status( kiapi::common::types::JS_SUCCESS );
            result.add_output_path( aOutput );
            return result;
        };
    };

    // Synchronous: the result comes back directly, with a job id that GetJobStatus knows
    kiapi::common::types::RunJobResponse sync = registry.Run( nullptr, makeExecutor( "sync.out" ), false );
    BOOST_CHECK_EQUAL( sync.status(), kiapi::common::types::JS_SUCCESS );
    BOOST_REQUIRE( !sync.job_id().empty() );
    BOOST_REQUIRE_EQUAL( sync.output_path_size(), 1 );

    std::optional<kiapi::common::commands::GetJobStatusResponse> status = registry.Status( sync.job_id() );
    BOOST_REQUIRE( status.has_value() );
    BOOST_CHECK_EQUAL( status->state(), kiapi::common::commands::JOB_STATE_FINISHED );
    BOOST_CHECK_EQUAL( status->percent(), 100 );
    BOOST_CHECK_EQUAL( status->result().output_path( 0 ), "sync.out" );

    // Asynchronous: JS_RUNNING at once, finished after the worker ran it
    kiapi::common::types::RunJobResponse async = registry.Run( nullptr, makeExecutor( "async.out" ), true );
    BOOST_CHECK_EQUAL( async.status(), kiapi::common::types::JS_RUNNING );
    BOOST_REQUIRE( !async.job_id().empty() );
    BOOST_CHECK_NE( async.job_id(), sync.job_id() );

    registry.WaitForIdle();
    BOOST_CHECK( !registry.Busy() );

    status = registry.Status( async.job_id() );
    BOOST_REQUIRE( status.has_value() );
    BOOST_CHECK_EQUAL( status->state(), kiapi::common::commands::JOB_STATE_FINISHED );
    BOOST_CHECK_EQUAL( status->result().status(), kiapi::common::types::JS_SUCCESS );
    BOOST_CHECK_EQUAL( status->result().job_id(), async.job_id() );
    BOOST_CHECK_EQUAL( status->result().output_path( 0 ), "async.out" );

    // An executor that throws is a failed job, not a dead worker
    kiapi::common::types::RunJobResponse throwing = registry.Run(
            nullptr,
            []( PROGRESS_REPORTER& ) -> kiapi::common::types::RunJobResponse
            {
                throw std::runtime_error( "boom" );
            },
            true );

    registry.WaitForIdle();
    status = registry.Status( throwing.job_id() );
    BOOST_REQUIRE( status.has_value() );
    BOOST_CHECK_EQUAL( status->result().status(), kiapi::common::types::JS_ERROR );
    BOOST_CHECK( status->result().message().find( "boom" ) != std::string::npos );

    BOOST_CHECK( !registry.Status( "no-such-job" ).has_value() );
}


// Without a request the wait times out; nothing else can signal it in a socket-less server
BOOST_AUTO_TEST_CASE( WaitForRequestTimesOutWhenIdle )
{
    auto start = std::chrono::steady_clock::now();

    BOOST_CHECK( !m_server.WaitForRequest( std::chrono::milliseconds( 20 ) ) );

    auto elapsed = std::chrono::steady_clock::now() - start;
    BOOST_CHECK( elapsed >= std::chrono::milliseconds( 20 ) );
}


BOOST_AUTO_TEST_SUITE_END()
