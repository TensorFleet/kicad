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

#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string>

#include <api/common/envelope.pb.h>

#include "kicad_api_c.h"
#include "kicad_api_host.h"


namespace
{

std::unique_ptr<KICAD_API_HOST> g_host;
std::string                     g_lastError;
bool                            g_dispatching = false;

void ( *g_eventCallback )( const uint8_t*, size_t, void* ) = nullptr;
void* g_eventUser = nullptr;


/// Hand a std::string back over the C boundary as a malloc'd buffer.
uint8_t* copyOut( const std::string& aBytes, size_t* aOutLength )
{
    void* buffer = std::malloc( aBytes.size() ? aBytes.size() : 1 );

    if( !buffer )
    {
        g_lastError = "out of memory";

        if( aOutLength )
            *aOutLength = 0;

        return nullptr;
    }

    std::memcpy( buffer, aBytes.data(), aBytes.size() );

    if( aOutLength )
        *aOutLength = aBytes.size();

    return static_cast<uint8_t*>( buffer );
}

} // namespace


void kiapi_set_event_callback( void ( *aCallback )( const uint8_t*, size_t, void* ), void* aUser )
{
    g_eventCallback = aCallback;
    g_eventUser = aUser;
}


int kiapi_init( const char* aConfigJson )
{
    try
    {
        if( g_host )
        {
            g_lastError = "kiapi_init() has already been called";
            return 1;
        }

        KICAD_API_HOST_CONFIG config;
        std::string           error;

        if( !KICAD_API_HOST_CONFIG::FromJson( aConfigJson ? aConfigJson : "", &config, &error ) )
        {
            g_lastError = error;
            return 2;
        }

        auto host = std::make_unique<KICAD_API_HOST>();

        KICAD_API_HOST::EVENT_SINK sink =
                []( const std::string& aEvent )
                {
                    if( g_eventCallback )
                    {
                        g_eventCallback( reinterpret_cast<const uint8_t*>( aEvent.data() ),
                                         aEvent.size(), g_eventUser );
                    }
                };

        if( !host->Init( config, std::move( sink ), &error ) )
        {
            g_lastError = error;
            // Tear down whatever came up, so a later attempt is not blocked by half a KiCad.
            host->Shutdown();
            return 3;
        }

        g_host = std::move( host );
        g_lastError.clear();
        return 0;
    }
    catch( const std::exception& e )
    {
        g_lastError = std::string( "exception in kiapi_init: " ) + e.what();
        return 4;
    }
    catch( ... )
    {
        g_lastError = "unknown exception in kiapi_init";
        return 4;
    }
}


uint8_t* kiapi_dispatch( const uint8_t* aRequest, size_t aLength, size_t* aOutLength )
{
    if( aOutLength )
        *aOutLength = 0;

    try
    {
        if( !g_host )
        {
            g_lastError = "kiapi_init() has not been called";
            return copyOut( KICAD_API_HOST::MakeErrorResponse(
                                    kiapi::common::ApiStatusCode::AS_NOT_READY,
                                    "the KiCad API host is not initialized" ),
                            aOutLength );
        }

        if( g_dispatching )
        {
            // The core is single threaded and synchronous; a nested call would corrupt it.
            g_lastError = "a request is already being dispatched";
            return copyOut( KICAD_API_HOST::MakeErrorResponse(
                                    kiapi::common::ApiStatusCode::AS_BUSY,
                                    "KiCad is already handling a request" ),
                            aOutLength );
        }

        g_dispatching = true;

        std::string reply;

        try
        {
            reply = g_host->Dispatch(
                    std::string( reinterpret_cast<const char*>( aRequest ), aLength ) );
        }
        catch( ... )
        {
            g_dispatching = false;
            throw;
        }

        g_dispatching = false;

        return copyOut( reply, aOutLength );
    }
    catch( const std::exception& e )
    {
        g_lastError = std::string( "exception in kiapi_dispatch: " ) + e.what();
        return copyOut( KICAD_API_HOST::MakeErrorResponse(
                                kiapi::common::ApiStatusCode::AS_UNKNOWN, g_lastError ),
                        aOutLength );
    }
    catch( ... )
    {
        g_lastError = "unknown exception in kiapi_dispatch";
        return copyOut( KICAD_API_HOST::MakeErrorResponse(
                                kiapi::common::ApiStatusCode::AS_UNKNOWN, g_lastError ),
                        aOutLength );
    }
}


void kiapi_free( void* aBuffer )
{
    std::free( aBuffer );
}


void kiapi_shutdown( void )
{
    try
    {
        if( g_host )
        {
            g_host->Shutdown();
            g_host.reset();
        }
    }
    catch( ... )
    {
        g_lastError = "unknown exception in kiapi_shutdown";
    }
}


const char* kiapi_last_error( void )
{
    return g_lastError.c_str();
}
