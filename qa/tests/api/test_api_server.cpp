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
#include <api/api_handler_pcb.h>
#include <api/api_server.h>
#include <api/headless_pcb_context.h>
#include <api/common/commands/base_commands.pb.h>
#include <api/common/envelope.pb.h>

#include <board.h>
#include <settings/settings_manager.h>


namespace
{

using kiapi::common::commands::GetSupportedCommandsResponse;
using kiapi::common::commands::SupportedCommand;


std::string typeUrl( const google::protobuf::Message& aMessage )
{
    return "type.googleapis.com/" + aMessage.GetTypeName();
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

    // Commands that need an editor frame are listed but flagged as not available headless
    for( const std::string& guiOnly : { typeUrl( kiapi::common::commands::RunAction() ),
                                        typeUrl( kiapi::common::commands::GetSelection() ),
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


// Without a request the wait times out; nothing else can signal it in a socket-less server
BOOST_AUTO_TEST_CASE( WaitForRequestTimesOutWhenIdle )
{
    auto start = std::chrono::steady_clock::now();

    BOOST_CHECK( !m_server.WaitForRequest( std::chrono::milliseconds( 20 ) ) );

    auto elapsed = std::chrono::steady_clock::now() - start;
    BOOST_CHECK( elapsed >= std::chrono::milliseconds( 20 ) );
}


BOOST_AUTO_TEST_SUITE_END()
