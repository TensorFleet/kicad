/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 * @author Jon Evans <jon@craftyjon.com>
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

#include <atomic>
#include <chrono>
#include <csignal>
#include <memory>

#include <api/api_server.h>
#include <api/api_server_host.h>
#include <cli/exit_codes.h>
#include <wx/app.h>
#include <wx/crt.h>

#include "command_api_server.h"

#define ARG_PATH "path"
#define ARG_SOCKET "--socket"
#define ARG_TOKEN "--token"
#define ARG_NO_EVENTS "--no-events"


std::atomic_bool g_apiServerExitRequested{ false };

void apiServerSignalHandler( int )
{
    g_apiServerExitRequested.store( true );
}


CLI::API_SERVER_COMMAND::API_SERVER_COMMAND() :
        COMMAND( "api-server" )
{
    m_argParser.add_description( UTF8STDSTR( _( "Run the KiCad IPC API server in headless mode" ) ) );

    m_argParser.add_argument( ARG_PATH )
            .default_value( std::string() )
            .nargs( argparse::nargs_pattern::optional )
            .help( UTF8STDSTR( _( "Optional path to a .kicad_pro, .kicad_pcb, or .kicad_sch file to pre-load" ) ) )
            .metavar( "PROJECT_OR_FILE" );

    m_argParser.add_argument( ARG_SOCKET )
            .default_value( std::string() )
            .help( UTF8STDSTR( _( "Listen at this socket path or nng URL (ipc://path, tcp://host:port, "
                                  "ws://host:port/path) instead of the default socket" ) ) )
            .metavar( "SOCKET_PATH_OR_URL" );

    m_argParser.add_argument( ARG_TOKEN )
            .default_value( std::string() )
            .help( UTF8STDSTR( _( "Use this API token instead of a random one" ) ) )
            .metavar( "TOKEN" );

    m_argParser.add_argument( ARG_NO_EVENTS )
            .help( UTF8STDSTR( _( "Do not open the events socket" ) ) )
            .flag();
}


int CLI::API_SERVER_COMMAND::doPerform( KIWAY& aKiway )
{
    std::unique_ptr<KICAD_API_SERVER> server = std::make_unique<KICAD_API_SERVER>( false );

    // The document and project lifecycle (which the GUI's project manager owns) lives in
    // API_SERVER_HOST so that other headless hosts can serve it identically.  Declared after the
    // server so that it is torn down first.
    API_SERVER_HOST host( aKiway, *server );

    wxString socketPath = wxString::FromUTF8( m_argParser.get<std::string>( ARG_SOCKET ) );

    if( !socketPath.IsEmpty() )
        server->SetSocketPath( socketPath );

    server->SetToken( m_argParser.get<std::string>( ARG_TOKEN ) );
    server->SetPublishEvents( !m_argParser.get<bool>( ARG_NO_EVENTS ) );

    host.Install();
    server->Start();

    if( !server->Running() )
    {
        wxFprintf( stderr, _( "Failed to start API server\n" ) );
        return EXIT_CODES::ERR_UNKNOWN;
    }

    wxString preloadPath = wxString::FromUTF8( m_argParser.get<std::string>( ARG_PATH ) );
    wxString preloadError;

    if( !host.Preload( preloadPath, &preloadError ) )
    {
        wxFprintf( stderr, "%s\n", preloadError );
        return EXIT_CODES::ERR_ARGS;
    }

    server->SetReadyToReply( true );

    wxString listenPath = wxString::FromUTF8( server->SocketPath() );
    wxString eventsPath = wxString::FromUTF8( server->EventsSocketPath() );
    wxFprintf( stdout, "KiCad API server listening at %s\n", listenPath );

    if( !eventsPath.IsEmpty() )
        wxFprintf( stdout, "KiCad API events published at %s\n", eventsPath );
    else
        wxFprintf( stdout, "KiCad API events not published\n" );

    fflush( stdout );

    auto oldSigInt = std::signal( SIGINT, apiServerSignalHandler );
#ifdef SIGTERM
    auto oldSigTerm = std::signal( SIGTERM, apiServerSignalHandler );
#endif

    g_apiServerExitRequested.store( false );

    // There is no wx event loop in kicad-cli, so requests queued by the server thread are pumped
    // by hand.  Block until one arrives instead of polling: the timeout only bounds how long a
    // SIGINT/SIGTERM (which merely sets a flag) waits to be noticed.
    while( !g_apiServerExitRequested.load() )
    {
        server->WaitForRequest( std::chrono::milliseconds( 100 ) );
        wxTheApp->ProcessPendingEvents();
    }

    std::signal( SIGINT, oldSigInt );
#ifdef SIGTERM
    std::signal( SIGTERM, oldSigTerm );
#endif

    wxFprintf( stdout, "Shutting down\n" );

    host.Shutdown();

    return EXIT_CODES::OK;
}
