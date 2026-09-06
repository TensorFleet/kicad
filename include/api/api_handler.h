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

#ifndef KICAD_API_HANDLER_H
#define KICAD_API_HANDLER_H

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <tl/expected.hpp>

#include <wx/debug.h>
#include <wx/string.h>

#include <google/protobuf/message.h>

#include <kicommon.h>
#include <api/common/envelope.pb.h>
#include <api/common/events.pb.h>
#include <api/common/types/base_types.pb.h>
#include <core/typeinfo.h>

class KICAD_API_SERVER;

using kiapi::common::ApiRequest, kiapi::common::ApiResponse;
using kiapi::common::ApiResponseStatus, kiapi::common::ApiStatusCode;

typedef tl::expected<ApiResponse, ApiResponseStatus> API_RESULT;

template <typename T>
using HANDLER_RESULT = tl::expected<T, ApiResponseStatus>;


template <typename RequestMessageType>
struct HANDLER_CONTEXT
{
    std::string ClientName;
    RequestMessageType Request;
};


class KICOMMON_API API_HANDLER
{
public:
    API_HANDLER() : m_server( nullptr ) {}

    virtual ~API_HANDLER() {}

    /**
     * Attempt to handle the given API request, if a handler exists in this class for the message.
     * @param aMsg is a request to attempt to handle
     * @return a response to send to the client, or an appropriate error
     */
    API_RESULT Handle( ApiRequest& aMsg );

    /**
     * Whether a command can be served when KiCad is running without an editor window.
     * Commands registered as GUI_ONLY are still dispatched in headless mode; they are expected to
     * answer AS_UNIMPLEMENTED themselves (see checkForHeadless in the editor handlers).  The mode
     * is reported to clients through GetSupportedCommands.
     */
    enum class HANDLER_MODE
    {
        HEADLESS_CAPABLE,
        GUI_ONLY
    };

    /// Description of one command served by this handler; see SupportedCommands.
    struct SUPPORTED_COMMAND
    {
        std::string  RequestTypeName;   ///< Protobuf full name, e.g. kiapi.common.commands.Ping
        std::string  ResponseTypeName;  ///< Protobuf full name of the success response
        HANDLER_MODE Mode;
    };

    /**
     * @return a description of every command registered with this handler, in registration
     *         order.  Used by the API server to answer GetSupportedCommands.
     */
    std::vector<SUPPORTED_COMMAND> SupportedCommands() const;

    /**
     * @return the document this handler serves, if it serves exactly one (editor handlers).  The
     *         API server publishes DocumentOpened / DocumentClosed events for it when the handler
     *         is registered and deregistered.
     */
    virtual std::optional<kiapi::common::types::DocumentSpecifier> Document() const
    {
        return std::nullopt;
    }

    /// @return the server this handler is registered with, or nullptr
    KICAD_API_SERVER* Server() const { return m_server; }

protected:
    friend class KICAD_API_SERVER;

    /// Called by the server on RegisterHandler / DeregisterHandler
    void attachServer( KICAD_API_SERVER* aServer ) { m_server = aServer; }

    /**
     * Publish an event on the server's events socket.  Does nothing if the handler is not
     * registered with a server or the server is not publishing.
     */
    void publish( const kiapi::common::events::Event& aEvent );

    /**
     * A handler for outer messages (envelopes) that will unpack to inner messages and call a
     * specific handler function.  @see registerHandler.
     */
    typedef std::function<HANDLER_RESULT<ApiResponse>( ApiRequest& )> REQUEST_HANDLER;

    /**
     * Registers an API command handler for the given message types.
     *
     * When an API request matching the given type comes in, the handler will be called and its
     * response will be packed into an envelope for sending back to the API client.
     *
     * If the given message does not unpack into the request type, an envelope is returned with
     * status AS_BAD_REQUEST, which probably indicates corruption in the message.
     *
     * @tparam RequestType is a protobuf message type containing a command
     * @tparam ResponseType is a protobuf message type containing a command response
     * @tparam HandlerType is the implied type of the API_HANDLER subclass
     * @param aHandler is the handler function for the given request and response types
     * @param aMode declares whether the command can be served without an editor window; this is
     *              only metadata for GetSupportedCommands and does not affect dispatch
     */
    template <class RequestType, class ResponseType, class HandlerType>
    void registerHandler( HANDLER_RESULT<ResponseType> ( HandlerType::*aHandler )(
                                  const HANDLER_CONTEXT<RequestType>& ),
                          HANDLER_MODE aMode = HANDLER_MODE::HEADLESS_CAPABLE )
    {
        std::string typeName { RequestType().GetTypeName() };

        wxASSERT_MSG( !m_handlers.contains( typeName ),
                      wxString::Format( "Duplicate API handler for type %s", typeName ) );

        m_registrationOrder.push_back( typeName );

        REGISTERED_HANDLER& entry = m_handlers[typeName];
        entry.ResponseTypeName = ResponseType().GetTypeName();
        entry.Mode = aMode;

        entry.Handler =
                [this, aHandler]( ApiRequest& aRequest ) -> API_RESULT
                {
                    HANDLER_CONTEXT<RequestType> ctx;
                    ApiResponse envelope;

                    if( !tryUnpack( aRequest, envelope, ctx.Request ) )
                        return envelope;

                    ctx.ClientName = aRequest.header().client_name();

                    HANDLER_RESULT<ResponseType> response =
                            std::invoke( aHandler, static_cast<HandlerType*>( this ), ctx );

                    if( response.has_value() )
                    {
                        envelope.mutable_status()->set_status( ApiStatusCode::AS_OK );
                        envelope.mutable_message()->PackFrom( *response );
                        return envelope;
                    }
                    else
                    {
                        return tl::unexpected( response.error() );
                    }
                };
    }

    /// A handler method together with the metadata reported by GetSupportedCommands
    struct REGISTERED_HANDLER
    {
        REQUEST_HANDLER Handler;
        std::string     ResponseTypeName;
        HANDLER_MODE    Mode;
    };

    /// Maps type name (without the URL prefix) to a handler method
    std::map<std::string, REGISTERED_HANDLER> m_handlers;

    /// Request type names in the order they were registered, for stable command listings
    std::vector<std::string> m_registrationOrder;

    static const wxString m_defaultCommitMessage;

    /// The server this handler is registered with (non-owning); see attachServer
    KICAD_API_SERVER* m_server;

private:

    template<typename MessageType>
    bool tryUnpack( ApiRequest& aRequest, ApiResponse& aReply, MessageType& aDest )
    {
        if( !aRequest.message().UnpackTo( &aDest ) )
        {
            std::string msg = fmt::format( "could not unpack message of type {} from request",
                                           aDest.GetTypeName() );
            aReply.mutable_status()->set_status( ApiStatusCode::AS_BAD_REQUEST );
            aReply.mutable_status()->set_error_message( msg );
            return false;
        }

        return true;
    }
};

#endif //KICAD_API_HANDLER_H
