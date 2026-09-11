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

/*
 * The Emscripten module entry point.
 *
 * There is nothing here but the event bridge: everything a host can ask for is the kiapi_*
 * C ABI in host/kicad_api_c.h, which -sEXPORTED_FUNCTIONS puts on the Module object, and
 * fab_pcb's packages/kicad-wasm drives it directly.
 *
 * main() runs once, when the factory returned by -sMODULARIZE resolves, and only installs the
 * event sink.  -sEXIT_RUNTIME=0 (the default) means returning from it does NOT tear the runtime
 * down and does not run the file-scope destructors -- which is what we want, both because the
 * module has to stay usable and because that teardown is exactly what made the native host
 * leave through _exit() (host/STATUS-wasm-prep.md).
 */

#include <emscripten.h>

#include <clocale>
#include <cstddef>
#include <cstdint>

#include "../kicad_api_c.h"


/*
 * Hand one serialized kiapi.common.events.Event to JavaScript.
 *
 * Under -sMODULARIZE the generated code is one closure per instance and `Module` inside an
 * EM_JS body is that instance's own Module object, so this reaches the `__kiapiEvent` the
 * loader passes in the module argument (packages/kicad-wasm/src/index.ts installs it both
 * through moduleArg and again afterwards).
 *
 * The subarray is a live view into the wasm heap: the loader copies it before doing anything
 * that could grow, move or free it.
 */
EM_JS( void, kiapi_publish_event_js, ( const uint8_t* aBytes, size_t aLength ), {
    var sink = Module[ "__kiapiEvent" ];

    if( typeof sink !== "function" )
        return;

    try
    {
        sink( HEAPU8.subarray( aBytes, aBytes + aLength ) );
    }
    catch( e )
    {
        // A throwing listener must not unwind back into KiCad's publisher.
        if( typeof console !== "undefined" )
            console.error( "kiapi event listener threw:", e );
    }
} );


static void publishEvent( const uint8_t* aBytes, size_t aLength, void* )
{
    kiapi_publish_event_js( aBytes, aLength );
}


/*
 * musl starts in the "C" locale, where wctomb() rejects every non-ASCII character, so
 * swprintf() fails and wxString::Format() silently returns an EMPTY string for any format with
 * a non-ASCII argument (host/STATUS-wasm-deps.md).  kiapi_init() sets the locale too, but this
 * has to happen before the file-scope constructors in the link set -- KiCad has a lot of them,
 * and several build wxStrings.  Priority 101 is the lowest a user constructor may ask for.
 */
__attribute__( ( constructor( 101 ) ) ) static void kiapi_set_utf8_locale()
{
    if( !std::setlocale( LC_ALL, "C.UTF-8" ) )
        std::setlocale( LC_ALL, "C" );
}


int main()
{
    kiapi_set_event_callback( &publishEvent, nullptr );
    return 0;
}
