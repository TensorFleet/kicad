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
 * Tests for the API server's transport URLs: how the events socket is derived from the request
 * socket for each nng scheme.  Since 11.0.
 */

#include <boost/test/unit_test.hpp>

#include <api/api_server.h>


BOOST_AUTO_TEST_SUITE( ApiTransport )


BOOST_AUTO_TEST_CASE( EventsUrlFor )
{
    BOOST_CHECK_EQUAL( KICAD_API_SERVER::EventsUrlFor( "ipc:///tmp/kicad/api.sock" ), "ipc:///tmp/kicad/api-events.sock" );
    BOOST_CHECK_EQUAL( KICAD_API_SERVER::EventsUrlFor( "tcp://127.0.0.1:5555" ), "tcp://127.0.0.1:5556" );
    BOOST_CHECK_EQUAL( KICAD_API_SERVER::EventsUrlFor( "tcp6://[::1]:5555" ), "tcp6://[::1]:5556" );
    BOOST_CHECK_EQUAL( KICAD_API_SERVER::EventsUrlFor( "ws://127.0.0.1:5555/kicad" ), "ws://127.0.0.1:5555/kicad/events" );
    BOOST_CHECK_EQUAL( KICAD_API_SERVER::EventsUrlFor( "ws://127.0.0.1:5555/kicad/" ), "ws://127.0.0.1:5555/kicad/events" );
    BOOST_CHECK_EQUAL( KICAD_API_SERVER::EventsUrlFor( "wss://host:443/api" ), "wss://host:443/api/events" );
    BOOST_CHECK_EQUAL( KICAD_API_SERVER::EventsUrlFor( "inproc://kicad" ), "inproc://kicad-events" );

    // Unknown schemes and malformed URLs give no events socket
    BOOST_CHECK_EQUAL( KICAD_API_SERVER::EventsUrlFor( "tcp://127.0.0.1" ), "" );
    BOOST_CHECK_EQUAL( KICAD_API_SERVER::EventsUrlFor( "tcp://127.0.0.1:port" ), "" );
    BOOST_CHECK_EQUAL( KICAD_API_SERVER::EventsUrlFor( "tls+tcp://127.0.0.1:5555" ), "" );
    BOOST_CHECK_EQUAL( KICAD_API_SERVER::EventsUrlFor( "/tmp/kicad/api.sock" ), "" );
}


BOOST_AUTO_TEST_SUITE_END()
