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
 * Tests for the API server's events socket: GetServerInfo discovery and the events published
 * when documents are opened, changed, and closed.
 */

#include <memory>
#include <optional>
#include <string>

#include <boost/test/unit_test.hpp>

#include <nng/nng.h>
#include <nng/protocol/pubsub0/sub.h>

#include <qa_utils/wx_utils/unit_test_utils.h>
#include <pcbnew_utils/board_test_utils.h>

#include <api/api_handler_pcb.h>
#include <api/api_server.h>
#include <api/headless_pcb_context.h>
#include <api/common/commands/base_commands.pb.h>
#include <api/common/commands/editor_commands.pb.h>
#include <api/common/envelope.pb.h>
#include <api/common/events.pb.h>
#include <api/board/board_commands.pb.h>

#include <board.h>
#include <pcb_track.h>
#include <settings/settings_manager.h>
#include <wx/filename.h>


namespace
{

using kiapi::common::events::Event;


/// RAII nng sub0 socket dialed to aUrl and subscribed to everything
struct EVENT_SUBSCRIBER
{
    EVENT_SUBSCRIBER( const std::string& aUrl )
    {
        BOOST_REQUIRE_EQUAL( nng_sub0_open( &socket ), 0 );
        BOOST_REQUIRE_EQUAL( nng_socket_set( socket, NNG_OPT_SUB_SUBSCRIBE, "", 0 ), 0 );
        BOOST_REQUIRE_EQUAL( nng_socket_set_ms( socket, NNG_OPT_RECVTIMEO, 2000 ), 0 );
        BOOST_REQUIRE_EQUAL( nng_dial( socket, aUrl.c_str(), nullptr, 0 ), 0 );
    }

    ~EVENT_SUBSCRIBER() { nng_close( socket ); }

    std::optional<Event> Receive()
    {
        char*  buf = nullptr;
        size_t sz = 0;

        if( nng_recv( socket, &buf, &sz, NNG_FLAG_ALLOC ) != 0 )
            return std::nullopt;

        Event event;
        bool  ok = event.ParseFromArray( buf, static_cast<int>( sz ) );
        nng_free( buf, sz );

        if( !ok )
            return std::nullopt;

        return event;
    }

    nng_socket socket;
};


struct API_EVENTS_FIXTURE
{
    API_EVENTS_FIXTURE()
    {
        wxString path = wxFileName::CreateTempFileName( "qa-api-events" );
        wxRemoveFile( path );
        m_socketPath = wxFileName( path );
        m_socketPath.SetExt( "sock" );
        m_server.SetSocketPath( m_socketPath.GetFullPath() );
    }

    ~API_EVENTS_FIXTURE()
    {
        m_server.Stop();
        wxRemoveFile( m_socketPath.GetFullPath() );
        wxRemoveFile( KICAD_API_SERVER::EventsSocketPathFor( m_socketPath ).GetFullPath() );
    }

    void loadBoard( const wxString& aRelPath )
    {
        KI_TEST::LoadBoard( m_settingsManager, aRelPath, m_board );
        m_context = std::make_shared<HEADLESS_PCB_CONTEXT>( std::move( m_board ), &m_settingsManager.Prj(),
                                                            nullptr );
    }

    kiapi::common::types::DocumentSpecifier pcbDocument()
    {
        kiapi::common::types::DocumentSpecifier doc;
        doc.set_type( kiapi::common::types::DocumentType::DOCTYPE_PCB );
        doc.set_board_filename( wxFileName( m_context->GetBoard()->GetFileName() ).GetFullName().ToStdString() );
        return doc;
    }

    template <typename T>
    kiapi::common::ApiRequest makeRequest( const T& aCommand )
    {
        kiapi::common::ApiRequest request;
        request.mutable_header()->set_client_name( "kicad.qa.events" );
        request.mutable_message()->PackFrom( aCommand );
        return request;
    }

    /**
     * Start the server and connect a subscriber.  pub/sub drops messages sent before the dial
     * has completed, so a JobProgress heartbeat is published until one comes back.
     */
    std::unique_ptr<EVENT_SUBSCRIBER> startAndSubscribe()
    {
        m_server.Start();
        BOOST_REQUIRE( m_server.Running() );
        BOOST_REQUIRE( !m_server.EventsSocketPath().empty() );

        auto subscriber = std::make_unique<EVENT_SUBSCRIBER>( m_server.EventsSocketPath() );
        nng_socket_set_ms( subscriber->socket, NNG_OPT_RECVTIMEO, 100 );

        bool connected = false;

        for( int attempt = 0; attempt < 50 && !connected; ++attempt )
        {
            Event heartbeat;
            heartbeat.mutable_job_progress()->set_job_id( "heartbeat" );
            BOOST_REQUIRE( m_server.Publish( heartbeat ) );
            connected = subscriber->Receive().has_value();
        }

        BOOST_REQUIRE_MESSAGE( connected, "subscriber never received an event" );
        nng_socket_set_ms( subscriber->socket, NNG_OPT_RECVTIMEO, 2000 );

        return subscriber;
    }

    /// Receive events until one of the given kind arrives (skipping heartbeats), or time out
    std::optional<Event> receiveKind( EVENT_SUBSCRIBER& aSubscriber, Event::KindCase aKind )
    {
        for( int i = 0; i < 20; ++i )
        {
            std::optional<Event> event = aSubscriber.Receive();

            if( !event )
                return std::nullopt;

            if( event->kind_case() == aKind )
                return event;
        }

        return std::nullopt;
    }

    SETTINGS_MANAGER                      m_settingsManager;
    std::unique_ptr<BOARD>                m_board;
    std::shared_ptr<HEADLESS_PCB_CONTEXT> m_context;
    wxFileName                            m_socketPath;
    KICAD_API_SERVER                      m_server{ false };
};

} // namespace


BOOST_FIXTURE_TEST_SUITE( ApiEvents, API_EVENTS_FIXTURE )


BOOST_AUTO_TEST_CASE( ServerInfoBeforeAndAfterStart )
{
    // Not started: no sockets, but the token already exists
    kiapi::common::commands::GetServerInfoResponse info = m_server.ServerInfo();
    BOOST_CHECK( info.socket_url().empty() );
    BOOST_CHECK( info.events_socket_url().empty() );
    BOOST_CHECK_EQUAL( info.kicad_token(), m_server.Token() );

    // Publishing without a socket is a no-op
    Event event;
    event.mutable_server_shutdown();
    BOOST_CHECK( !m_server.Publish( event ) );
    BOOST_CHECK_EQUAL( m_server.PublishedEventCount(), 0u );

    m_server.Start();
    BOOST_REQUIRE( m_server.Running() );

    info = m_server.ServerInfo();
    BOOST_CHECK_EQUAL( info.socket_url(), m_server.SocketPath() );
    BOOST_CHECK_EQUAL( info.events_socket_url(), m_server.EventsSocketPath() );
    BOOST_CHECK( info.events_socket_url().starts_with( "ipc://" ) );
    BOOST_CHECK( info.events_socket_url().ends_with( "-events.sock" ) );
    BOOST_CHECK_EQUAL( wxString::FromUTF8( info.events_socket_url() ),
                       "ipc://" + KICAD_API_SERVER::EventsSocketPathFor( m_socketPath ).GetFullPath() );
}


BOOST_AUTO_TEST_CASE( EventsSocketPathDerivation )
{
    BOOST_CHECK_EQUAL( KICAD_API_SERVER::EventsSocketPathFor( wxFileName( "/tmp/kicad/api.sock" ) ).GetFullPath(),
                       "/tmp/kicad/api-events.sock" );
    BOOST_CHECK_EQUAL( KICAD_API_SERVER::EventsSocketPathFor( wxFileName( "/tmp/kicad/api-1234.sock" ) ).GetFullPath(),
                       "/tmp/kicad/api-1234-events.sock" );
}


BOOST_AUTO_TEST_CASE( DocumentLifecycleEventsArePublished )
{
    loadBoard( wxS( "issue5830" ) );

    std::unique_ptr<EVENT_SUBSCRIBER> subscriber = startAndSubscribe();
    uint64_t                          lastSequence = m_server.PublishedEventCount();

    // Registering a document handler announces its document
    auto handler = std::make_unique<API_HANDLER_PCB>( m_context );
    m_server.RegisterHandler( handler.get() );

    std::optional<Event> opened = receiveKind( *subscriber, Event::kDocumentOpened );
    BOOST_REQUIRE( opened.has_value() );
    BOOST_CHECK_GT( opened->sequence(), lastSequence );
    lastSequence = opened->sequence();
    BOOST_CHECK_EQUAL( opened->document_opened().document().type(), kiapi::common::types::DOCTYPE_PCB );
    BOOST_CHECK_EQUAL( opened->document_opened().document().board_filename(), pcbDocument().board_filename() );

    // A change outside of a commit: DocumentChanged without item ids, carrying the new revision
    kiapi::board::commands::SetBoardOrigin origin;
    *origin.mutable_board() = pcbDocument();
    origin.set_type( kiapi::board::commands::BOT_GRID );
    origin.mutable_origin()->set_x_nm( 1000000 );
    origin.mutable_origin()->set_y_nm( 2000000 );

    kiapi::common::ApiRequest request = makeRequest( origin );
    BOOST_REQUIRE( handler->Handle( request ).has_value() );

    std::optional<Event> changed = receiveKind( *subscriber, Event::kDocumentChanged );
    BOOST_REQUIRE( changed.has_value() );
    BOOST_CHECK_GT( changed->sequence(), lastSequence );
    lastSequence = changed->sequence();
    BOOST_CHECK_EQUAL( changed->document_changed().revision(), 1u );
    BOOST_CHECK( !changed->document_changed().has_commit_id() );
    BOOST_CHECK_EQUAL( changed->document_changed().created_size(), 0 );
    BOOST_CHECK_EQUAL( changed->document_changed().document().board_filename(), pcbDocument().board_filename() );

    // A commit that deletes an item: DocumentChanged with the commit id, client, message and ids
    BOARD*    board = m_context->GetBoard();
    PCB_TRACK* track = nullptr;

    for( PCB_TRACK* candidate : board->Tracks() )
    {
        track = candidate;
        break;
    }

    BOOST_REQUIRE( track );
    KIID trackId = track->m_Uuid;

    kiapi::common::commands::BeginCommit begin;
    *begin.mutable_header()->mutable_document() = pcbDocument();
    request = makeRequest( begin );
    API_RESULT beginResult = handler->Handle( request );
    BOOST_REQUIRE( beginResult.has_value() );

    kiapi::common::commands::BeginCommitResponse beginResponse;
    BOOST_REQUIRE( beginResult->message().UnpackTo( &beginResponse ) );

    kiapi::common::commands::DeleteItems del;
    *del.mutable_header()->mutable_document() = pcbDocument();
    del.add_item_ids()->set_value( trackId.AsStdString() );
    request = makeRequest( del );
    BOOST_REQUIRE( handler->Handle( request ).has_value() );

    kiapi::common::commands::EndCommit end;
    *end.mutable_header()->mutable_document() = pcbDocument();
    *end.mutable_id() = beginResponse.id();
    end.set_action( kiapi::common::commands::CMA_COMMIT );
    end.set_message( "delete a track" );
    request = makeRequest( end );
    BOOST_REQUIRE( handler->Handle( request ).has_value() );

    changed = receiveKind( *subscriber, Event::kDocumentChanged );
    BOOST_REQUIRE( changed.has_value() );
    BOOST_CHECK_GT( changed->sequence(), lastSequence );
    lastSequence = changed->sequence();
    BOOST_CHECK_EQUAL( changed->document_changed().revision(), 2u );
    BOOST_CHECK_EQUAL( changed->document_changed().commit_id().value(), beginResponse.id().value() );
    BOOST_CHECK_EQUAL( changed->document_changed().client_name(), "kicad.qa.events" );
    BOOST_CHECK_EQUAL( changed->document_changed().message(), "delete a track" );
    BOOST_REQUIRE_EQUAL( changed->document_changed().deleted_size(), 1 );
    BOOST_CHECK_EQUAL( changed->document_changed().deleted( 0 ).value(), trackId.AsStdString() );
    BOOST_CHECK_EQUAL( changed->document_changed().created_size(), 0 );
    BOOST_CHECK_EQUAL( changed->document_changed().updated_size(), 0 );

    // Deregistering announces the close
    m_server.DeregisterHandler( handler.get() );

    std::optional<Event> closed = receiveKind( *subscriber, Event::kDocumentClosed );
    BOOST_REQUIRE( closed.has_value() );
    BOOST_CHECK_GT( closed->sequence(), lastSequence );
    lastSequence = closed->sequence();
    BOOST_CHECK_EQUAL( closed->document_closed().document().board_filename(), pcbDocument().board_filename() );

    // A handler that is no longer registered publishes nothing
    BOOST_CHECK( handler->Server() == nullptr );

    // Stopping the server announces the shutdown
    m_server.Stop();

    std::optional<Event> shutdown = receiveKind( *subscriber, Event::kServerShutdown );
    BOOST_REQUIRE( shutdown.has_value() );
    BOOST_CHECK_GT( shutdown->sequence(), lastSequence );
}


BOOST_AUTO_TEST_SUITE_END()
