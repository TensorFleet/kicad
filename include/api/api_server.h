/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2024 Jon Evans <jon@craftyjon.com>
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

#ifndef KICAD_API_SERVER_H
#define KICAD_API_SERVER_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include <wx/event.h>
#include <wx/filename.h>

#include <kicommon.h>
#include <api/common/commands/base_commands.pb.h>
#include <api/common/events.pb.h>

class API_HANDLER;
class API_HANDLER_SERVER;
class KINNG_PUBLISHER;
class KINNG_REQUEST_SERVER;
class wxEvtHandler;


wxDECLARE_EVENT( API_REQUEST_EVENT, wxCommandEvent );


class KICOMMON_API KICAD_API_SERVER : public wxEvtHandler
{
public:
    KICAD_API_SERVER( bool aAutoStart = true );

    ~KICAD_API_SERVER();

    void Start();

    void Stop();

    bool Running() const;

    /**
     * Adds a new request handler to the server.  Each handler maintains its own list of API
     * messages that it knows how to handle, and the server will pass every incoming message to all
     * handlers in succession until one of them handles it.
     *
     * The caller is responsible for the lifetime of the handler and must call DeregisterHandler
     * before the pointer is freed.
     *
     * @param aHandler is a pointer (non-owned) to API_HANDLER
     */
    void RegisterHandler( API_HANDLER* aHandler );

    void DeregisterHandler( API_HANDLER* aHandler );

    /**
     * Enumerate every command served by the currently-registered handlers (including the
     * server's own commands).  Commands served by more than one handler are listed once; such a
     * command is reported as headless-capable if any of its handlers can serve it headless.
     *
     * @return the response for the GetSupportedCommands API command
     */
    kiapi::common::commands::GetSupportedCommandsResponse SupportedCommands() const;

    /**
     * @return the response for the GetServerInfo API command: socket URLs and token
     */
    kiapi::common::commands::GetServerInfoResponse ServerInfo() const;

    /**
     * Publish an event on the events (pub/sub) socket.  The event's sequence number is assigned
     * here.  Safe to call from any thread; does nothing if the events socket is not running.
     *
     * @return true if the event was handed to the transport
     */
    bool Publish( kiapi::common::events::Event aEvent );

    /// @return the number of events published so far by this server
    uint64_t PublishedEventCount() const { return m_eventSequence.load( std::memory_order_acquire ); }

    /**
     * Block the calling thread until the server thread has queued a request, or aTimeout has
     * elapsed.  Hosts without a running wxWidgets event loop (kicad-cli api-server) use this to
     * know when wxApp::ProcessPendingEvents() has work to do instead of polling.
     *
     * The pending flag is cleared on return, so a request that arrives while the caller is
     * still processing events wakes the next call immediately.
     *
     * @return true if a request is waiting to be processed
     */
    bool WaitForRequest( std::chrono::milliseconds aTimeout );

    void SetReadyToReply( bool aReady = true )
    {
        m_readyToReply.store( aReady, std::memory_order_release );
    }

    void SetSocketPath( const wxString& aSocketPath )
    {
        m_socketPathOverride = aSocketPath;
    }

    std::string SocketPath() const;

    /// @return the URL of the events socket, or an empty string if events are not published
    std::string EventsSocketPath() const;

    const std::string& Token() const { return m_token; }

    /**
     * Return the default API socket path (without the ipc:// scheme).
     */
    static wxFileName StandardSocketPath();

    /**
     * Return the default API socket URL (including the ipc:// scheme).
     */
    static std::string StandardSocketUrl();

    /**
     * Derive the events socket path from a request socket path: "api.sock" becomes
     * "api-events.sock" (and "api-1234.sock" becomes "api-1234-events.sock").
     */
    static wxFileName EventsSocketPathFor( const wxFileName& aSocketPath );

private:

    /**
     * Callback that executes on the server thread and generates an event that will be handled by
     * the wxWidgets event loop to process an incoming request.  Temporarily takes ownership of the
     * request pointer so that it can be passed through the event system.
     *
     * @param aRequest is a pointer to a string containing bytes that came in over the wire
     */
    void onApiRequest( std::string* aRequest );

    /**
     * Event handler that receives the event on the main thread sent by onApiRequest
     * @param aEvent will contain a pointer to an incoming API request string in the client data
     */
    void handleApiEvent( wxCommandEvent& aEvent );

    void handleApiRequestString( std::string& aRequestString );

    void log( const std::string& aOutput );

    std::unique_ptr<KINNG_REQUEST_SERVER> m_server;

    /// Pushes kiapi.common.events.Event messages to subscribers; see Publish
    std::unique_ptr<KINNG_PUBLISHER> m_publisher;

    std::atomic<uint64_t> m_eventSequence;

    /// Serves commands that concern the server itself (GetSupportedCommands); always registered
    std::unique_ptr<API_HANDLER_SERVER> m_serverHandler;

    std::set<API_HANDLER*> m_handlers;

    std::string m_token;

    std::atomic<bool> m_readyToReply;

    /// Signals WaitForRequest from the server thread; see onApiRequest
    std::mutex m_wakeMutex;

    std::condition_variable m_wakeCondition;

    bool m_requestPending;

    wxString m_socketPathOverride;

    static wxString s_logFileName;

    wxFileName m_logFilePath;
};

#endif //KICAD_API_SERVER_H
