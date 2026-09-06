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

#include <algorithm>
#include <map>

#include <fmt/format.h>
#include <wx/app.h>
#include <wx/datetime.h>
#include <wx/event.h>
#include <wx/stdpaths.h>

#include <advanced_config.h>
#include <api/api_handler.h>
#include <api/api_utils.h> // traceApi
#include <api/api_server.h>
#include <ki_exception.h>
#include <kiid.h>
#include <kinng.h>
#include <paths.h>
#include <pgm_base.h>
#include <settings/common_settings.h>
#include <string_utils.h>

#include <api/common/commands/editor_commands.pb.h>
#include <api/common/envelope.pb.h>

#ifdef __UNIX__
#include <sys/file.h>
#endif

using kiapi::common::ApiRequest, kiapi::common::ApiResponse, kiapi::common::ApiStatusCode;
using kiapi::common::commands::GetSupportedCommands, kiapi::common::commands::GetSupportedCommandsResponse;
using kiapi::common::commands::SupportedCommand;
using kiapi::common::commands::GetServerInfo, kiapi::common::commands::GetServerInfoResponse;
using kiapi::common::commands::GetOpenDocuments, kiapi::common::commands::GetOpenDocumentsResponse;


/**
 * Handler for commands that concern the API server itself rather than a document or editor.
 * Owned by the server and registered before any other handler.
 */
class API_HANDLER_SERVER : public API_HANDLER
{
public:
    API_HANDLER_SERVER( KICAD_API_SERVER* aServer ) :
            API_HANDLER(),
            m_server( aServer )
    {
        registerHandler<GetSupportedCommands, GetSupportedCommandsResponse>(
                &API_HANDLER_SERVER::handleGetSupportedCommands );
        registerHandler<GetServerInfo, GetServerInfoResponse>( &API_HANDLER_SERVER::handleGetServerInfo );
    }

private:
    HANDLER_RESULT<GetSupportedCommandsResponse> handleGetSupportedCommands(
            const HANDLER_CONTEXT<GetSupportedCommands>& aCtx )
    {
        return m_server->SupportedCommands();
    }

    HANDLER_RESULT<GetServerInfoResponse> handleGetServerInfo( const HANDLER_CONTEXT<GetServerInfo>& aCtx )
    {
        return m_server->ServerInfo();
    }

    KICAD_API_SERVER* m_server;
};


/**
 * Answers commands that no registered handler claimed, for commands whose "nothing to report"
 * answer is a success rather than AS_UNHANDLED.  Consulted after every registered handler.
 */
class API_HANDLER_FALLBACK : public API_HANDLER
{
public:
    API_HANDLER_FALLBACK() : API_HANDLER()
    {
        registerHandler<GetOpenDocuments, GetOpenDocumentsResponse>( &API_HANDLER_FALLBACK::handleGetOpenDocuments );
    }

private:
    /// No editor of the requested type is open: an empty list rather than "no handler available"
    HANDLER_RESULT<GetOpenDocumentsResponse> handleGetOpenDocuments( const HANDLER_CONTEXT<GetOpenDocuments>& aCtx )
    {
        return GetOpenDocumentsResponse();
    }
};


wxString KICAD_API_SERVER::s_logFileName = "api.log";


wxDEFINE_EVENT( API_REQUEST_EVENT, wxCommandEvent );


KICAD_API_SERVER::KICAD_API_SERVER( bool aAutoStart ) :
        wxEvtHandler(),
        m_serverHandler( std::make_unique<API_HANDLER_SERVER>( this ) ),
        m_fallbackHandler( std::make_unique<API_HANDLER_FALLBACK>() ),
        m_eventSequence( 0 ),
        m_token( KIID().AsStdString() ),
        m_readyToReply( false ),
        m_requestPending( false )
{
    m_handlers.push_back( m_serverHandler.get() );
    m_serverHandler->attachServer( this );
    m_fallbackHandler->attachServer( this );

    if( !aAutoStart )
        return;

    if( !Pgm().GetCommonSettings()->m_Api.enable_server )
    {
        wxLogTrace( traceApi, "Server: disabled by user preferences." );
        return;
    }

    Start();
}


KICAD_API_SERVER::~KICAD_API_SERVER()
{
    Stop();
}


wxFileName KICAD_API_SERVER::StandardSocketPath()
{
    wxFileName socket;

#ifdef __WXMAC__
    socket.AssignDir( wxS( "/tmp" ) );
#else
    socket.AssignDir( wxStandardPaths::Get().GetTempDir() );
#endif

    socket.AppendDir( wxS( "kicad" ) );
    socket.SetFullName( wxS( "api.sock" ) );

    return socket;
}


std::string KICAD_API_SERVER::StandardSocketUrl()
{
    return fmt::format( "ipc://{}", StandardSocketPath().GetFullPath().ToUTF8().data() );
}


wxFileName KICAD_API_SERVER::EventsSocketPathFor( const wxFileName& aSocketPath )
{
    wxFileName events( aSocketPath );
    events.SetName( aSocketPath.GetName() + wxS( "-events" ) );
    return events;
}


void KICAD_API_SERVER::Start()
{
    if( Running() )
        return;

    wxFileName socket;

    if( m_socketPathOverride.IsEmpty() )
    {
        socket = StandardSocketPath();
    }
    else
    {
        socket.Assign( m_socketPathOverride );

        if( !socket.IsAbsolute() )
            socket.MakeAbsolute();
    }

    if( !PATHS::EnsurePathExists( socket.GetPath() ) )
    {
        wxLogTrace( traceApi, wxString::Format( "Server: socket path %s could not be created",
                                                socket.GetPath() ) );
        return;
    }

#ifndef __WINDOWS__
    // We use non-abstract sockets because macOS and some other non-Linux platforms don't support
    // abstract sockets, which means there might be an old socket to unlink.  In order to try to
    // recover this, we lock a file (which will be unlocked on process exit) and if we get the lock,
    // we know the old socket is orphaned and can be removed.
    wxFileName lockFilePath( socket.GetPath(), wxS( "api.lock" ) );

    int lockFile = open( lockFilePath.GetFullPath().c_str(), O_RDONLY | O_CREAT, 0600 );

    if( lockFile >= 0 && flock( lockFile, LOCK_EX | LOCK_NB ) == 0 )
    {
        if( socket.Exists() )
        {
            wxLogTrace( traceApi, wxString::Format( "Server: cleaning up stale socket path %s",
                                                    socket.GetFullPath() ) );
            wxRemoveFile( socket.GetFullPath() );
        }
    }
#endif

    if( socket.Exists() )
    {
        socket.SetFullName( wxString::Format( wxS( "api-%lu.sock" ), ::wxGetProcessId() ) );

        if( socket.Exists() )
        {
            wxLogTrace( traceApi, wxString::Format( "Server: PID socket path %s already exists!",
                                                    socket.GetFullPath() ) );
            return;
        }
    }

    m_server = std::make_unique<KINNG_REQUEST_SERVER>(
            fmt::format( "ipc://{}", socket.GetFullPath().ToStdString() ) );
    m_server->SetCallback( [&]( std::string* aRequest ) { onApiRequest( aRequest ); } );

    if( !m_server->Start() )
    {
        wxLogTrace( traceApi, "Server: failed to start KINNG listener thread" );
        m_server.reset( nullptr );
        return;
    }

    // The events socket sits next to the request socket.  Failing to open it is not fatal:
    // clients see an empty events_socket_url in GetServerInfo and fall back to polling.
    wxFileName eventsSocket = EventsSocketPathFor( socket );

    if( eventsSocket.Exists() )
        wxRemoveFile( eventsSocket.GetFullPath() );

    m_publisher = std::make_unique<KINNG_PUBLISHER>(
            fmt::format( "ipc://{}", eventsSocket.GetFullPath().ToStdString() ) );

    if( !m_publisher->Start() )
    {
        wxLogTrace( traceApi, "Server: failed to start events publisher" );
        m_publisher.reset( nullptr );
    }

    m_logFilePath.AssignDir( PATHS::GetLogsPath() );
    m_logFilePath.SetName( s_logFileName );

    if( ADVANCED_CFG::GetCfg().m_EnableAPILogging )
    {
        PATHS::EnsurePathExists( PATHS::GetLogsPath() );
        log( fmt::format( "--- KiCad API server started at {} ---\n", SocketPath() ) );
    }

    wxLogTrace( traceApi, wxString::Format( "Server: listening at %s", SocketPath() ) );
    Bind( API_REQUEST_EVENT, &KICAD_API_SERVER::handleApiEvent, this );
}


void KICAD_API_SERVER::Stop()
{
    if( !Running() )
        return;

    wxLogTrace( traceApi, "Stopping server" );
    Unbind( API_REQUEST_EVENT, &KICAD_API_SERVER::handleApiEvent, this );

    if( m_publisher )
    {
        kiapi::common::events::Event shutdown;
        shutdown.mutable_server_shutdown();
        Publish( std::move( shutdown ) );

        m_publisher->Stop();
        m_publisher.reset( nullptr );
    }

    m_server->Stop();
    m_server.reset( nullptr );

    // Release anyone blocked in WaitForRequest
    m_wakeCondition.notify_all();
}


bool KICAD_API_SERVER::Running() const
{
    return m_server && m_server->Running();
}


void KICAD_API_SERVER::RegisterHandler( API_HANDLER* aHandler )
{
    wxCHECK( aHandler, /* void */ );

    if( std::ranges::find( m_handlers, aHandler ) != m_handlers.end() )
        return;

    m_handlers.push_back( aHandler );

    aHandler->attachServer( this );

    if( std::optional<kiapi::common::types::DocumentSpecifier> doc = aHandler->Document() )
    {
        kiapi::common::events::Event event;
        *event.mutable_document_opened()->mutable_document() = std::move( *doc );
        Publish( std::move( event ) );
    }
}


void KICAD_API_SERVER::DeregisterHandler( API_HANDLER* aHandler )
{
    if( aHandler == m_serverHandler.get() )
        return;

    auto it = std::ranges::find( m_handlers, aHandler );

    if( it == m_handlers.end() )
        return;

    m_handlers.erase( it );

    if( std::optional<kiapi::common::types::DocumentSpecifier> doc = aHandler->Document() )
    {
        kiapi::common::events::Event event;
        *event.mutable_document_closed()->mutable_document() = std::move( *doc );
        Publish( std::move( event ) );
    }

    aHandler->attachServer( nullptr );
}


GetServerInfoResponse KICAD_API_SERVER::ServerInfo() const
{
    GetServerInfoResponse response;
    response.set_socket_url( SocketPath() );
    response.set_events_socket_url( EventsSocketPath() );
    response.set_kicad_token( m_token );
    return response;
}


bool KICAD_API_SERVER::Publish( kiapi::common::events::Event aEvent )
{
    if( !m_publisher )
        return false;

    aEvent.set_sequence( m_eventSequence.fetch_add( 1, std::memory_order_acq_rel ) + 1 );

    if( ADVANCED_CFG::GetCfg().m_EnableAPILogging )
        log( "Event: " + aEvent.ShortDebugString() + "\n" );

    return m_publisher->Publish( aEvent.SerializeAsString() );
}


GetSupportedCommandsResponse KICAD_API_SERVER::SupportedCommands() const
{
    // Keyed by request type name so that a command served by several handlers (for example,
    // GetOpenDocuments in both the board and schematic handlers) is reported once, sorted.
    std::map<std::string, SupportedCommand> commands;

    std::vector<API_HANDLER*> handlers = m_handlers;
    handlers.push_back( m_fallbackHandler.get() );

    for( API_HANDLER* handler : handlers )
    {
        for( const API_HANDLER::SUPPORTED_COMMAND& cmd : handler->SupportedCommands() )
        {
            bool headless = ( cmd.Mode == API_HANDLER::HANDLER_MODE::HEADLESS_CAPABLE );
            auto it = commands.find( cmd.RequestTypeName );

            if( it != commands.end() )
            {
                it->second.set_headless( it->second.headless() || headless );
                continue;
            }

            SupportedCommand& entry = commands[cmd.RequestTypeName];
            entry.set_type_url( fmt::format( "type.googleapis.com/{}", cmd.RequestTypeName ) );
            entry.set_response_type_url( fmt::format( "type.googleapis.com/{}", cmd.ResponseTypeName ) );
            entry.set_headless( headless );
        }
    }

    GetSupportedCommandsResponse response;

    for( auto& [name, cmd] : commands )
        *response.add_commands() = std::move( cmd );

    return response;
}


std::string KICAD_API_SERVER::SocketPath() const
{
    return m_server ? m_server->SocketPath() : "";
}


std::string KICAD_API_SERVER::EventsSocketPath() const
{
    return m_publisher ? m_publisher->SocketPath() : "";
}


void KICAD_API_SERVER::onApiRequest( std::string* aRequest )
{
    if( !m_readyToReply.load( std::memory_order_acquire ) )
    {
        ApiResponse notHandled;
        notHandled.mutable_status()->set_status( ApiStatusCode::AS_NOT_READY );
        notHandled.mutable_status()->set_error_message( "KiCad is not ready to reply" );
        m_server->Reply( notHandled.SerializeAsString() );
        log( "Got incoming request but was not yet ready to reply." );
        return;
    }

    wxCommandEvent* evt = new wxCommandEvent( API_REQUEST_EVENT );

    // We don't actually need write access to this string, but client data is non-const
    evt->SetClientData( static_cast<void*>( aRequest ) );

    // Takes ownership and frees the wxCommandEvent
    QueueEvent( evt );

    // Wake a host that is blocked in WaitForRequest rather than running an event loop
    {
        std::lock_guard<std::mutex> lock( m_wakeMutex );
        m_requestPending = true;
    }

    m_wakeCondition.notify_all();
}


bool KICAD_API_SERVER::WaitForRequest( std::chrono::milliseconds aTimeout )
{
    std::unique_lock<std::mutex> lock( m_wakeMutex );

    bool pending = m_wakeCondition.wait_for( lock, aTimeout, [&]() { return m_requestPending; } );

    m_requestPending = false;
    return pending;
}


void KICAD_API_SERVER::handleApiEvent( wxCommandEvent& aEvent )
{
    std::string& requestString = *static_cast<std::string*>( aEvent.GetClientData() );
    handleApiRequestString( requestString );
}


void KICAD_API_SERVER::handleApiRequestString( std::string& aRequestString )
{
    ApiRequest request;

    if( !request.ParseFromString( aRequestString ) )
    {
        ApiResponse error;
        error.mutable_header()->set_kicad_token( m_token );
        error.mutable_status()->set_status( ApiStatusCode::AS_BAD_REQUEST );
        error.mutable_status()->set_error_message( "request could not be parsed" );
        m_server->Reply( error.SerializeAsString() );

        if( ADVANCED_CFG::GetCfg().m_EnableAPILogging )
            log( "Response (ERROR): " + error.Utf8DebugString() );

        return;
    }

    if( ADVANCED_CFG::GetCfg().m_EnableAPILogging )
        log( "Request: " + request.Utf8DebugString() );

    if( !request.header().kicad_token().empty() &&
        request.header().kicad_token().compare( m_token ) != 0 )
    {
        ApiResponse error;
        error.mutable_header()->set_kicad_token( m_token );
        error.mutable_status()->set_status( ApiStatusCode::AS_TOKEN_MISMATCH );
        error.mutable_status()->set_error_message(
                "the provided kicad_token did not match this KiCad instance's token" );
        m_server->Reply( error.SerializeAsString() );

        if( ADVANCED_CFG::GetCfg().m_EnableAPILogging )
            log( "Response (ERROR): " + error.Utf8DebugString() );

        return;
    }

    API_RESULT result = Dispatch( request );

    // Note: at the point we call Reply(), we no longer own requestString.

    if( result.has_value() )
    {
        result->mutable_header()->set_kicad_token( m_token );
        m_server->Reply( result->SerializeAsString() );

        if( ADVANCED_CFG::GetCfg().m_EnableAPILogging )
            log( "Response: " + result->Utf8DebugString() );
    }
    else
    {
        ApiResponse error;
        error.mutable_status()->CopyFrom( result.error() );
        error.mutable_header()->set_kicad_token( m_token );

        if( result.error().status() == ApiStatusCode::AS_UNHANDLED )
        {
            std::string type = "<unparseable Any>";
            google::protobuf::Any::ParseAnyTypeUrl( request.message().type_url(), &type );
            std::string msg = fmt::format( "no handler available for request of type {}", type );
            error.mutable_status()->set_error_message( msg );
        }

        m_server->Reply( error.SerializeAsString() );

        if( ADVANCED_CFG::GetCfg().m_EnableAPILogging )
            log( "Response (ERROR): " + error.Utf8DebugString() );
    }
}


API_RESULT KICAD_API_SERVER::Dispatch( ApiRequest& aRequest )
{
    API_RESULT result;

    // A handler that throws must still produce a reply: the request/reply socket cannot receive
    // the next request until this one is answered, so an escaped exception would wedge the
    // server for every later client.
    try
    {
        // Indexed rather than iterator-based: a handler may register or deregister other handlers
        // while it runs (OpenDocument / CloseDocument), which invalidates iterators.
        for( size_t i = 0; i < m_handlers.size(); ++i )
        {
            result = m_handlers[i]->Handle( aRequest );

            if( result.has_value() )
                break;
            else if( result.error().status() != ApiStatusCode::AS_UNHANDLED )
                break;
        }

        if( !result.has_value() && result.error().status() == ApiStatusCode::AS_UNHANDLED )
            result = m_fallbackHandler->Handle( aRequest );
    }
    catch( const IO_ERROR& ioe )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "request failed: {}", ioe.What().ToUTF8().data() ) );
        result = tl::unexpected( e );
    }
    catch( const std::exception& exc )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "request failed: {}", exc.what() ) );
        result = tl::unexpected( e );
    }

    return result;
}


void KICAD_API_SERVER::log( const std::string& aOutput )
{
    FILE* fp = wxFopen( m_logFilePath.GetFullPath(), wxT( "a" ) );

    if( !fp )
        return;

    wxString out;
    wxDateTime now = wxDateTime::Now();

    fprintf( fp, "%s", TO_UTF8( out.Format( wxS( "%s: %s" ),
                                            now.FormatISOCombined(), aOutput ) ) );
    fclose( fp );
}
