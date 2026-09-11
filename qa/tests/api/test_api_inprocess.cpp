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
 * Tests for the in-process transport seam on KICAD_API_SERVER: DispatchBytes() answers a request
 * without a socket, and StartInProcess() routes published events to a sink instead of a
 * pub/sub socket.  This is the path a WASM (or any embedded) host uses.
 */

#include <string>
#include <vector>

#include <boost/test/unit_test.hpp>

#include <qa_utils/wx_utils/unit_test_utils.h>

#include <api/api_handler_common.h>
#include <api/api_server.h>
#include <api/common/commands/base_commands.pb.h>
#include <api/common/envelope.pb.h>
#include <api/common/events.pb.h>

#include <google/protobuf/empty.pb.h>


namespace
{

using kiapi::common::ApiRequest;
using kiapi::common::ApiResponse;
using kiapi::common::ApiStatusCode;


struct API_INPROCESS_FIXTURE
{
    // Constructed without starting a listener; the in-process path never opens a socket.
    KICAD_API_SERVER   m_server{ false };
    API_HANDLER_COMMON m_commonHandler;

    std::vector<std::string> m_events;

    API_INPROCESS_FIXTURE()
    {
        m_server.RegisterHandler( &m_commonHandler );
    }

    ~API_INPROCESS_FIXTURE()
    {
        // The sink captures this fixture, so the server has to let go of it before m_events dies
        m_server.Stop();
        m_server.DeregisterHandler( &m_commonHandler );
    }

    void startInProcess()
    {
        m_server.StartInProcess( [this]( const std::string& aEvent )
                                 {
                                     m_events.push_back( aEvent );
                                 } );
    }

    /// An ApiRequest carrying aMessage, without a token (which the server accepts)
    static std::string request( const google::protobuf::Message& aMessage )
    {
        ApiRequest req;
        req.mutable_header()->set_client_name( "qa_api/inprocess" );
        req.mutable_message()->PackFrom( aMessage );
        return req.SerializeAsString();
    }

    static ApiResponse parse( const std::string& aBytes )
    {
        ApiResponse response;
        BOOST_REQUIRE( response.ParseFromString( aBytes ) );
        return response;
    }
};

} // namespace


BOOST_FIXTURE_TEST_SUITE( ApiInProcess, API_INPROCESS_FIXTURE )


BOOST_AUTO_TEST_CASE( DispatchBytesRefusesBeforeReady )
{
    startInProcess();

    // SetReadyToReply has not been called: the ready-flag guard answers instead of the handlers
    ApiResponse response = parse( m_server.DispatchBytes( request( kiapi::common::commands::Ping() ) ) );

    BOOST_CHECK_EQUAL( response.status().status(), ApiStatusCode::AS_NOT_READY );
}


BOOST_AUTO_TEST_CASE( DispatchBytesAnswersPingWithToken )
{
    startInProcess();
    m_server.SetReadyToReply( true );

    ApiResponse response = parse( m_server.DispatchBytes( request( kiapi::common::commands::Ping() ) ) );

    BOOST_CHECK_EQUAL( response.status().status(), ApiStatusCode::AS_OK );
    BOOST_CHECK_EQUAL( response.header().kicad_token(), m_server.Token() );
}


BOOST_AUTO_TEST_CASE( DispatchBytesReportsUnhandledMessages )
{
    startInProcess();
    m_server.SetReadyToReply( true );

    // No handler serves google.protobuf.Empty as a command
    ApiResponse response = parse( m_server.DispatchBytes( request( google::protobuf::Empty() ) ) );

    BOOST_CHECK_EQUAL( response.status().status(), ApiStatusCode::AS_UNHANDLED );
    BOOST_CHECK_EQUAL( response.header().kicad_token(), m_server.Token() );
    BOOST_CHECK( response.status().error_message().find( "google.protobuf.Empty" ) != std::string::npos );
}


BOOST_AUTO_TEST_CASE( DispatchBytesRejectsGarbageAndTokenMismatch )
{
    startInProcess();
    m_server.SetReadyToReply( true );

    ApiResponse bad = parse( m_server.DispatchBytes( "definitely not a protobuf \xff\xff\xff" ) );
    BOOST_CHECK_EQUAL( bad.status().status(), ApiStatusCode::AS_BAD_REQUEST );

    ApiRequest req;
    req.mutable_header()->set_kicad_token( "not-this-instance" );
    req.mutable_message()->PackFrom( kiapi::common::commands::Ping() );

    ApiResponse mismatch = parse( m_server.DispatchBytes( req.SerializeAsString() ) );
    BOOST_CHECK_EQUAL( mismatch.status().status(), ApiStatusCode::AS_TOKEN_MISMATCH );
    BOOST_CHECK_EQUAL( mismatch.header().kicad_token(), m_server.Token() );
}


BOOST_AUTO_TEST_CASE( PublishReachesTheSinkInSequence )
{
    startInProcess();

    BOOST_CHECK( m_server.Running() );

    kiapi::common::events::Event first;
    first.mutable_server_shutdown();
    BOOST_CHECK( m_server.Publish( first ) );

    kiapi::common::events::Event second;
    second.mutable_document_opened()->mutable_document()->set_type(
            kiapi::common::types::DOCTYPE_PROJECT );
    BOOST_CHECK( m_server.Publish( second ) );

    BOOST_REQUIRE_EQUAL( m_events.size(), 2 );

    kiapi::common::events::Event decoded;
    BOOST_REQUIRE( decoded.ParseFromString( m_events[0] ) );
    BOOST_CHECK_EQUAL( decoded.sequence(), 1 );
    BOOST_CHECK( decoded.has_server_shutdown() );

    BOOST_REQUIRE( decoded.ParseFromString( m_events[1] ) );
    BOOST_CHECK_EQUAL( decoded.sequence(), 2 );
    BOOST_CHECK( decoded.has_document_opened() );

    BOOST_CHECK_EQUAL( m_server.PublishedEventCount(), 2 );
}


BOOST_AUTO_TEST_CASE( ServerInfoReportsTheInProcessUrls )
{
    m_server.StartInProcess( [this]( const std::string& aEvent )
                             {
                                 m_events.push_back( aEvent );
                             },
                             "inproc://qa-api", "inproc://qa-api-events" );

    BOOST_CHECK_EQUAL( m_server.SocketPath(), "inproc://qa-api" );
    BOOST_CHECK_EQUAL( m_server.EventsSocketPath(), "inproc://qa-api-events" );

    kiapi::common::commands::GetServerInfoResponse info = m_server.ServerInfo();
    BOOST_CHECK_EQUAL( info.socket_url(), "inproc://qa-api" );
    BOOST_CHECK_EQUAL( info.events_socket_url(), "inproc://qa-api-events" );
    BOOST_CHECK_EQUAL( info.kicad_token(), m_server.Token() );

    // Stopping publishes a shutdown event through the sink and closes the server
    m_server.Stop();

    BOOST_CHECK( !m_server.Running() );
    BOOST_REQUIRE_EQUAL( m_events.size(), 1 );

    kiapi::common::events::Event decoded;
    BOOST_REQUIRE( decoded.ParseFromString( m_events[0] ) );
    BOOST_CHECK( decoded.has_server_shutdown() );

    BOOST_CHECK_EQUAL( m_server.SocketPath(), "" );
    BOOST_CHECK_EQUAL( m_server.EventsSocketPath(), "" );
}


BOOST_AUTO_TEST_SUITE_END()
