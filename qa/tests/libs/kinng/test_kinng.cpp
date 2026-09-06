/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2023 Jon Evans <jon@craftyjon.com>
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

#include <string>

#include <wx/filename.h>
#include <wx/stdpaths.h>

#include <nng/nng.h>
#include <nng/protocol/pubsub0/sub.h>

#include <qa_utils/wx_utils/unit_test_utils.h>
#include <kinng.h>

#include <import_export.h>
#include <api/common/envelope.pb.h>


namespace
{

/// A unique ipc:// URL in the temp directory; the file is removed so the listener can bind it
std::string tempSocketUrl( const wxString& aPrefix )
{
    wxString path = wxFileName::CreateTempFileName( aPrefix );
    wxRemoveFile( path );
    return "ipc://" + path.ToStdString();
}


/// RAII nng sub0 socket dialed to aUrl and subscribed to everything
struct SUBSCRIBER
{
    SUBSCRIBER( const std::string& aUrl )
    {
        BOOST_REQUIRE_EQUAL( nng_sub0_open( &socket ), 0 );
        BOOST_REQUIRE_EQUAL( nng_socket_set( socket, NNG_OPT_SUB_SUBSCRIBE, "", 0 ), 0 );
        BOOST_REQUIRE_EQUAL( nng_socket_set_ms( socket, NNG_OPT_RECVTIMEO, 2000 ), 0 );
        BOOST_REQUIRE_EQUAL( nng_dial( socket, aUrl.c_str(), nullptr, 0 ), 0 );
    }

    ~SUBSCRIBER() { nng_close( socket ); }

    /// @return the next message, or nullopt on timeout
    std::optional<std::string> Receive()
    {
        char*  buf = nullptr;
        size_t sz = 0;
        int    rc = nng_recv( socket, &buf, &sz, NNG_FLAG_ALLOC );

        if( rc != 0 )
            return std::nullopt;

        std::string msg( buf, sz );
        nng_free( buf, sz );
        return msg;
    }

    nng_socket socket;
};

} // namespace


BOOST_AUTO_TEST_SUITE( KiNNG )

BOOST_AUTO_TEST_CASE( CreateIPCResponder )
{
    KINNG_REQUEST_SERVER server( wxFileName::CreateTempFileName( "test-kinng" ).ToStdString() );
}


BOOST_AUTO_TEST_CASE( PublisherLifecycle )
{
    KINNG_PUBLISHER publisher( tempSocketUrl( "test-kinng-pub" ) );

    BOOST_CHECK( !publisher.Running() );

    // Publishing before Start() is a no-op, not an error
    BOOST_CHECK( !publisher.Publish( "dropped" ) );

    BOOST_REQUIRE( publisher.Start() );
    BOOST_CHECK( publisher.Running() );
    BOOST_CHECK( publisher.Start() ); // idempotent

    // No subscriber: nng drops the message but the send itself succeeds
    BOOST_CHECK( publisher.Publish( "nobody listening" ) );

    publisher.Stop();
    BOOST_CHECK( !publisher.Running() );
    BOOST_CHECK( !publisher.Publish( "stopped" ) );
}


BOOST_AUTO_TEST_CASE( PublisherDeliversToSubscriber )
{
    KINNG_PUBLISHER publisher( tempSocketUrl( "test-kinng-pub" ) );
    BOOST_REQUIRE( publisher.Start() );

    SUBSCRIBER subscriber( publisher.SocketPath() );

    // pub/sub has no handshake visible to the publisher: messages sent before the dial has
    // completed are dropped, so retry until the first one arrives.
    bool connected = false;

    for( int attempt = 0; attempt < 50 && !connected; ++attempt )
    {
        BOOST_REQUIRE( publisher.Publish( "hello" ) );
        nng_socket_set_ms( subscriber.socket, NNG_OPT_RECVTIMEO, 100 );
        connected = subscriber.Receive().has_value();
    }

    BOOST_REQUIRE_MESSAGE( connected, "subscriber never received a message" );
    nng_socket_set_ms( subscriber.socket, NNG_OPT_RECVTIMEO, 2000 );

    // Once connected, messages arrive in order and byte-exact (including embedded NULs)
    std::string binary( "\x00\x01\x02proto", 8 );
    BOOST_REQUIRE( publisher.Publish( "first" ) );
    BOOST_REQUIRE( publisher.Publish( binary ) );

    std::optional<std::string> first = subscriber.Receive();
    std::optional<std::string> second = subscriber.Receive();

    BOOST_REQUIRE( first.has_value() );
    BOOST_REQUIRE( second.has_value() );
    BOOST_CHECK_EQUAL( *first, "first" );
    BOOST_CHECK_EQUAL( second->size(), binary.size() );
    BOOST_CHECK( *second == binary );
}

BOOST_AUTO_TEST_SUITE_END()
