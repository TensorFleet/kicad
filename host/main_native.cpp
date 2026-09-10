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
 * kicad-api-host-native: KiCad's headless API core behind three pipes.
 *
 * stdin carries serialized kiapi.common.ApiRequest frames, stdout the ApiResponse frames, and
 * fd 3 (or --events-fd N) the kiapi.common.events.Event frames.  Every frame is a uint32
 * big-endian length followed by that many bytes.  One request is in flight at a time and replies
 * come back in order, which is what fab_pcb's StdioTransport matches on.
 *
 * The same binary runs a --selftest that drives the API through the C ABI and checks the status
 * codes, so the build can be gated without the TypeScript stack.
 */

#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <google/protobuf/any.pb.h>

#include <api/common/envelope.pb.h>
#include <api/common/events.pb.h>
#include <api/common/commands/base_commands.pb.h>
#include <api/common/commands/editor_commands.pb.h>
#include <api/common/commands/project_commands.pb.h>
#include <api/common/types/base_types.pb.h>
#include <api/common/types/enums.pb.h>
#include <api/board/board_commands.pb.h>

#include "kicad_api_c.h"
#include "kicad_api_host.h"

using namespace kiapi::common;


namespace
{

/// Where replies go.  stdout is dup'ed here at startup and fd 1 is then pointed at stderr, so
/// that a stray printf() from anywhere in KiCad cannot corrupt the framing.
int g_replyFd = -1;

/// Where events go, or -1 when the parent gave us no events pipe.
int g_eventsFd = -1;


bool writeAll( int aFd, const void* aData, size_t aLength )
{
    const char* p = static_cast<const char*>( aData );
    size_t      remaining = aLength;

    while( remaining > 0 )
    {
        ssize_t written = ::write( aFd, p, remaining );

        if( written < 0 )
        {
            if( errno == EINTR )
                continue;

            return false;
        }

        p += written;
        remaining -= static_cast<size_t>( written );
    }

    return true;
}


bool writeFrame( int aFd, const void* aData, size_t aLength )
{
    if( aFd < 0 )
        return false;

    uint8_t header[4];
    header[0] = static_cast<uint8_t>( ( aLength >> 24 ) & 0xff );
    header[1] = static_cast<uint8_t>( ( aLength >> 16 ) & 0xff );
    header[2] = static_cast<uint8_t>( ( aLength >> 8 ) & 0xff );
    header[3] = static_cast<uint8_t>( aLength & 0xff );

    return writeAll( aFd, header, 4 ) && ( aLength == 0 || writeAll( aFd, aData, aLength ) );
}


/// @return true on a full read, false at a clean EOF (aOutEof) or on error
bool readAll( int aFd, void* aData, size_t aLength, bool* aOutEof )
{
    char*  p = static_cast<char*>( aData );
    size_t remaining = aLength;

    *aOutEof = false;

    while( remaining > 0 )
    {
        ssize_t got = ::read( aFd, p, remaining );

        if( got == 0 )
        {
            // A clean EOF is only clean between frames
            *aOutEof = ( remaining == aLength );
            return false;
        }

        if( got < 0 )
        {
            if( errno == EINTR )
                continue;

            return false;
        }

        p += got;
        remaining -= static_cast<size_t>( got );
    }

    return true;
}


void eventCallback( const uint8_t* aBytes, size_t aLength, void* )
{
    if( g_eventsFd >= 0 )
        writeFrame( g_eventsFd, aBytes, aLength );
}


void logLine( const char* aFormat, ... )
{
    va_list args;
    va_start( args, aFormat );
    std::vfprintf( stderr, aFormat, args );
    va_end( args );
    std::fputc( '\n', stderr );
    std::fflush( stderr );
}


/// Everything the command line can say
struct HOST_ARGS
{
    std::string configJson;
    std::string home;
    std::string share;
    std::string fonts;
    std::string preload;
    std::string token;
    bool        publishEvents = true;
    bool        selftest = false;
    int         eventsFd = 3;
    bool        help = false;
};


bool parseArgs( int argc, char** argv, HOST_ARGS* aArgs, std::string* aError )
{
    auto needsValue =
            [&]( int& i, const char* aName ) -> const char*
            {
                if( i + 1 >= argc )
                {
                    *aError = std::string( aName ) + " needs a value";
                    return nullptr;
                }

                return argv[++i];
            };

    for( int i = 1; i < argc; ++i )
    {
        std::string arg( argv[i] );

        if( arg == "--help" || arg == "-h" )
        {
            aArgs->help = true;
        }
        else if( arg == "--selftest" )
        {
            aArgs->selftest = true;
        }
        else if( arg == "--no-events" )
        {
            aArgs->publishEvents = false;
        }
        else if( arg == "--config" )
        {
            const char* value = needsValue( i, "--config" );

            if( !value )
                return false;

            aArgs->configJson = value;
        }
        else if( arg == "--home" )
        {
            const char* value = needsValue( i, "--home" );

            if( !value )
                return false;

            aArgs->home = value;
        }
        else if( arg == "--share" )
        {
            const char* value = needsValue( i, "--share" );

            if( !value )
                return false;

            aArgs->share = value;
        }
        else if( arg == "--fonts" )
        {
            const char* value = needsValue( i, "--fonts" );

            if( !value )
                return false;

            aArgs->fonts = value;
        }
        else if( arg == "--preload" )
        {
            const char* value = needsValue( i, "--preload" );

            if( !value )
                return false;

            aArgs->preload = value;
        }
        else if( arg == "--token" )
        {
            const char* value = needsValue( i, "--token" );

            if( !value )
                return false;

            aArgs->token = value;
        }
        else if( arg == "--events-fd" )
        {
            const char* value = needsValue( i, "--events-fd" );

            if( !value )
                return false;

            aArgs->eventsFd = std::atoi( value );
        }
        else if( !arg.empty() && arg[0] == '{' )
        {
            // A JSON object on argv is the config, as the wasm loader passes it
            aArgs->configJson = arg;
        }
        else if( !arg.empty() && arg[0] == '-' )
        {
            *aError = "unknown option " + arg;
            return false;
        }
        else
        {
            // The bare positional is a document to preload, like kicad-cli api-server's
            aArgs->preload = arg;
        }
    }

    return true;
}


bool buildConfig( const HOST_ARGS& aArgs, KICAD_API_HOST_CONFIG* aConfig, std::string* aError )
{
    if( !KICAD_API_HOST_CONFIG::FromJson( aArgs.configJson, aConfig, aError ) )
        return false;

    if( !aArgs.home.empty() )
        aConfig->home = aArgs.home;

    if( !aArgs.share.empty() )
        aConfig->share = aArgs.share;

    if( !aArgs.fonts.empty() )
        aConfig->fonts = aArgs.fonts;

    if( !aArgs.preload.empty() )
        aConfig->preload = aArgs.preload;

    if( !aArgs.token.empty() )
        aConfig->token = aArgs.token;

    if( !aArgs.publishEvents )
        aConfig->publishEvents = false;

    return true;
}


std::string toJson( const KICAD_API_HOST_CONFIG& aConfig )
{
    // Small enough to hand-roll rather than pull nlohmann into this translation unit; only the
    // fields the C ABI reads back are emitted.
    auto escape =
            []( const std::string& aValue )
            {
                std::string out;

                for( char c : aValue )
                {
                    if( c == '"' || c == '\\' )
                        out += '\\';

                    out += c;
                }

                return out;
            };

    std::string js = "{";
    js += "\"home\":\"" + escape( aConfig.home ) + "\",";
    js += "\"share\":\"" + escape( aConfig.share ) + "\",";
    js += "\"fonts\":\"" + escape( aConfig.fonts ) + "\",";
    js += "\"preload\":\"" + escape( aConfig.preload ) + "\",";
    js += "\"token\":\"" + escape( aConfig.token ) + "\",";
    js += "\"publishEvents\":";
    js += aConfig.publishEvents ? "true" : "false";
    js += ",\"env\":{";

    bool first = true;

    for( const auto& [name, value] : aConfig.env )
    {
        if( !first )
            js += ",";

        js += "\"" + escape( name ) + "\":\"" + escape( value ) + "\"";
        first = false;
    }

    js += "}}";

    return js;
}


// ---------------------------------------------------------------------------------------------
// selftest
// ---------------------------------------------------------------------------------------------

int         g_checks = 0;
int         g_failures = 0;


void check( bool aOk, const std::string& aWhat )
{
    ++g_checks;

    if( aOk )
    {
        logLine( "  ok    %s", aWhat.c_str() );
    }
    else
    {
        ++g_failures;
        logLine( "  FAIL  %s", aWhat.c_str() );
    }
}


/// Send one command through the C ABI and parse the reply
bool call( const google::protobuf::Message& aCommand, ApiResponse* aResponse )
{
    ApiRequest request;
    request.mutable_header()->set_client_name( "kicad-api-host-native/selftest" );
    request.mutable_message()->PackFrom( aCommand );

    std::string bytes = request.SerializeAsString();
    size_t      length = 0;

    uint8_t* reply = kiapi_dispatch( reinterpret_cast<const uint8_t*>( bytes.data() ),
                                     bytes.size(), &length );

    if( !reply )
    {
        logLine( "  dispatch returned NULL: %s", kiapi_last_error() );
        return false;
    }

    bool parsed = aResponse->ParseFromArray( reply, static_cast<int>( length ) );
    kiapi_free( reply );

    return parsed;
}


/// Run aCommand and check that it answered AS_OK; the unpacked payload comes back in aPayload
bool expectOk( const google::protobuf::Message& aCommand, const std::string& aWhat,
               google::protobuf::Message* aPayload = nullptr )
{
    ApiResponse response;

    if( !call( aCommand, &response ) )
    {
        check( false, aWhat + " (no parseable reply)" );
        return false;
    }

    if( response.status().status() != ApiStatusCode::AS_OK )
    {
        check( false, aWhat + " -> status " + std::to_string( response.status().status() ) + " "
                      + response.status().error_message() );
        return false;
    }

    if( aPayload && !response.message().UnpackTo( aPayload ) )
    {
        check( false, aWhat + " (reply payload was " + response.message().type_url() + ")" );
        return false;
    }

    check( true, aWhat );
    return true;
}


int runSelftest( const KICAD_API_HOST_CONFIG& aConfig, const std::string& aBoardPath )
{
    logLine( "kicad-api-host-native selftest" );

    std::string configJson = toJson( aConfig );

    if( kiapi_init( configJson.c_str() ) != 0 )
    {
        logLine( "  FAIL  kiapi_init: %s", kiapi_last_error() );
        return 1;
    }

    check( true, "kiapi_init" );

    expectOk( commands::Ping(), "Ping" );

    commands::GetVersionResponse version;

    if( expectOk( commands::GetVersion(), "GetVersion", &version ) )
        logLine( "        version %s", version.version().full_version().c_str() );

    commands::GetServerInfoResponse info;

    if( expectOk( commands::GetServerInfo(), "GetServerInfo", &info ) )
    {
        check( info.socket_url() == "inproc://kicad",
               "GetServerInfo reports inproc://kicad (" + info.socket_url() + ")" );
        logLine( "        events %s, token %s", info.events_socket_url().c_str(),
                 info.kicad_token().c_str() );
    }

    if( aBoardPath.empty() )
    {
        logLine( "no board given: skipping the document checks" );
    }
    else
    {
        commands::OpenDocument open;
        open.set_type( types::DocumentType::DOCTYPE_PCB );
        open.set_path( aBoardPath );

        commands::OpenDocumentResponse opened;

        if( expectOk( open, "OpenDocument " + aBoardPath, &opened ) )
        {
            types::DocumentSpecifier board = opened.document();

            commands::GetItems getItems;
            *getItems.mutable_header()->mutable_document() = board;
            getItems.add_types( types::KiCadObjectType::KOT_PCB_FOOTPRINT );

            commands::GetItemsResponse items;

            if( expectOk( getItems, "GetItems (footprints)", &items ) )
            {
                check( items.items_size() > 0, "GetItems returned at least one footprint" );
                logLine( "        %d footprints", items.items_size() );
            }

            commands::RunAction fill;
            fill.set_action( "pcbnew.ZoneFiller.zoneFillAll" );

            commands::RunActionResponse fillResult;

            if( expectOk( fill, "RunAction zoneFillAll", &fillResult ) )
            {
                check( fillResult.status() == commands::RunActionStatus::RAS_OK,
                       "zoneFillAll reported RAS_OK" );
            }

            kiapi::board::commands::RunBoardJobDrc drc;
            *drc.mutable_board() = board;
            drc.set_refill_zones( false );

            kiapi::board::commands::DrcResultsResponse drcResults;

            if( expectOk( drc, "RunBoardJobDrc", &drcResults ) )
            {
                logLine( "        %u errors, %u warnings, %u unconnected",
                         drcResults.error_count(), drcResults.warning_count(),
                         drcResults.unconnected_count() );
            }

            commands::CloseDocument close;
            *close.mutable_document() = board;
            expectOk( close, "CloseDocument" );
        }
    }

    kiapi_shutdown();
    check( true, "kiapi_shutdown" );

    logLine( "%d checks, %d failures", g_checks, g_failures );

    return g_failures == 0 ? 0 : 1;
}


// ---------------------------------------------------------------------------------------------
// stdio server
// ---------------------------------------------------------------------------------------------

int runStdio( const KICAD_API_HOST_CONFIG& aConfig )
{
    std::string configJson = toJson( aConfig );

    if( kiapi_init( configJson.c_str() ) != 0 )
    {
        logLine( "kicad-api-host-native: init failed: %s", kiapi_last_error() );
        return 1;
    }

    logLine( "kicad-api-host-native ready (events %s)",
             g_eventsFd >= 0 ? "on" : "off" );

    for( ;; )
    {
        uint8_t header[4];
        bool    eof = false;

        if( !readAll( STDIN_FILENO, header, 4, &eof ) )
        {
            if( !eof )
                logLine( "kicad-api-host-native: short read on the request header" );

            break;
        }

        uint32_t length = ( static_cast<uint32_t>( header[0] ) << 24 )
                          | ( static_cast<uint32_t>( header[1] ) << 16 )
                          | ( static_cast<uint32_t>( header[2] ) << 8 )
                          | static_cast<uint32_t>( header[3] );

        std::string request;
        request.resize( length );

        if( length > 0 && !readAll( STDIN_FILENO, request.data(), length, &eof ) )
        {
            logLine( "kicad-api-host-native: short read on a %u byte request", length );
            break;
        }

        size_t   replyLength = 0;
        uint8_t* reply = kiapi_dispatch( reinterpret_cast<const uint8_t*>( request.data() ),
                                         request.size(), &replyLength );

        if( !reply )
        {
            // Never leave the client without a reply: the transport matches positionally.
            std::string error = KICAD_API_HOST::MakeErrorResponse( ApiStatusCode::AS_UNKNOWN,
                                                                   kiapi_last_error() );

            if( !writeFrame( g_replyFd, error.data(), error.size() ) )
                break;

            continue;
        }

        bool written = writeFrame( g_replyFd, reply, replyLength );
        kiapi_free( reply );

        if( !written )
        {
            logLine( "kicad-api-host-native: could not write a reply" );
            break;
        }
    }

    kiapi_shutdown();

    return 0;
}

} // namespace


int main( int argc, char** argv )
{
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    HOST_ARGS   args;
    std::string error;

    if( !parseArgs( argc, argv, &args, &error ) )
    {
        logLine( "kicad-api-host-native: %s", error.c_str() );
        return 2;
    }

    if( args.help )
    {
        logLine( "usage: kicad-api-host-native [options] [document]\n"
                 "  --selftest             run the built-in API checks and exit\n"
                 "  --config <json>        the host config object (see docs/08-wasm.md)\n"
                 "  --home <dir>           settings directory ($HOME)\n"
                 "  --share <dir>          stock data root (KICAD_STOCK_DATA_HOME)\n"
                 "  --fonts <dir>          outline font directory (KICAD_FONTS_DIR)\n"
                 "  --preload <path>       document or project to open at startup\n"
                 "  --token <token>        use a fixed API token\n"
                 "  --events-fd <n>        write event frames to this descriptor (default 3)\n"
                 "  --no-events            do not publish events at all\n"
                 "Without --selftest the process speaks the stdio framing: uint32 big-endian\n"
                 "length + bytes, ApiRequest on stdin, ApiResponse on stdout, Event on fd 3." );
        return 0;
    }

    KICAD_API_HOST_CONFIG config;

    if( !buildConfig( args, &config, &error ) )
    {
        logLine( "kicad-api-host-native: %s", error.c_str() );
        return 2;
    }

    // Nothing but frames may reach the real stdout, and KiCad is full of code that prints.  Move
    // the reply channel out of the way and point fd 1 at stderr.
    g_replyFd = ::dup( STDOUT_FILENO );

    if( g_replyFd < 0 )
    {
        logLine( "kicad-api-host-native: could not duplicate stdout" );
        return 2;
    }

    ::dup2( STDERR_FILENO, STDOUT_FILENO );
    std::setvbuf( stdout, nullptr, _IOLBF, 0 );

    // The selftest has nowhere to put event frames; a descriptor 3 inherited from an
    // interactive shell would spray protobuf over the terminal.
    if( args.selftest )
        config.publishEvents = false;
    else if( args.publishEvents && args.eventsFd >= 0
             && ::fcntl( args.eventsFd, F_GETFD ) != -1 )
    {
        g_eventsFd = args.eventsFd;
    }
    else
    {
        config.publishEvents = false;
    }

    kiapi_set_event_callback( &eventCallback, nullptr );

    int rc;

    if( args.selftest )
    {
        // The selftest opens the board itself, so it must not also be preloaded
        std::string boardPath = config.preload;
        config.preload.clear();

        rc = runSelftest( config, boardPath );
    }
    else
    {
        rc = runStdio( config );
    }

    // Leave without running static destructors.  kiapi_shutdown has already torn down
    // everything this host owns; what is left at exit() is wx's and KiCad's file-scope
    // teardown, which in a wxBase-only build walks objects whose types came from the GUI
    // half and are only present here as link stubs (a wxAnyValueType registration, for one).
    // The process is on its way out and there is nothing left to flush but stdio.
    std::fflush( nullptr );
    ::_exit( rc );
}
