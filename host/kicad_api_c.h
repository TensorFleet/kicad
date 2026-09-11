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

#ifndef KICAD_API_C_H
#define KICAD_API_C_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The whole of KiCad's headless API, as five C functions.  This is what the wasm module exports
 * and what the native stdio host drives; see fab_pcb docs/08-wasm.md for the contract.
 *
 * Nothing here throws, and nothing here is thread safe: the module is single threaded and one
 * request is answered at a time.
 */

/// Set the callback that receives every published event (a serialized
/// kiapi.common.events.Event).  Install it before kiapi_init(); pass NULL to remove it.  The
/// bytes are only valid for the duration of the call.
void kiapi_set_event_callback( void ( *aCallback )( const uint8_t* aBytes, size_t aLength,
                                                    void* aUser ),
                               void* aUser );

/// Bring KiCad up.  aConfigJson may be NULL or "" for the defaults.  @return 0 on success,
/// non-zero on failure (see kiapi_last_error()).
int kiapi_init( const char* aConfigJson );

/// Answer one serialized kiapi.common.ApiRequest with a malloc'd serialized
/// kiapi.common.ApiResponse, whose length is written through aOutLength.  The caller releases
/// the buffer with kiapi_free().  Returns NULL only if the reply could not be produced at all;
/// every KiCad-level failure comes back as a well-formed ApiResponse with a non-AS_OK status.
uint8_t* kiapi_dispatch( const uint8_t* aRequest, size_t aLength, size_t* aOutLength );

/// Release a buffer returned by kiapi_dispatch().
void kiapi_free( void* aBuffer );

/// Close the documents and tear KiCad down.  Safe to call more than once.
void kiapi_shutdown( void );

/// The last failure message, or "" if there has not been one.  Owned by the module.
const char* kiapi_last_error( void );

#ifdef __cplusplus
}
#endif

#endif // KICAD_API_C_H
