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

#ifndef KICAD_API_HOST_H
#define KICAD_API_HOST_H

#include <functional>
#include <map>
#include <memory>
#include <string>

class API_SERVER_HOST;
class KICAD_API_SERVER;


/**
 * How a host wants KiCad set up before the API server starts.  Parsed from the JSON object
 * documented in fab_pcb's docs/08-wasm.md; every member is optional.
 */
struct KICAD_API_HOST_CONFIG
{
    /// Writable settings directory; exported as $HOME.  Empty leaves $HOME alone.
    std::string home;

    /// Stock data root (symbols/footprints/templates); exported as KICAD_STOCK_DATA_HOME.
    /// Empty leaves KiCad's compiled-in resolution alone, which is what a native build wants.
    std::string share;

    /// Directory of outline font files, with an optional manifest.json; exported as
    /// KICAD_FONTS_DIR for common/font/fontconfig_manifest.cpp.  Empty means "stroke font only".
    std::string fonts;

    /// Extra environment variables, applied after home/share and able to override them.
    std::map<std::string, std::string> env;

    /// A document or project to open at startup, like kicad-cli api-server's positional argument.
    std::string preload;

    /// A fixed API token; empty means "generate one".
    std::string token;

    /// Whether events are published to the sink at all.
    bool publishEvents = true;

    /**
     * Parse the JSON object form.  An empty or blank string yields the defaults.
     *
     * @param aJson is the JSON text
     * @param aError is filled with the parse error when this returns false
     */
    static bool FromJson( const std::string& aJson, KICAD_API_HOST_CONFIG* aConfig,
                          std::string* aError );

    /// Defaults that suit the platform: MEMFS paths under Emscripten, the host's own under a
    /// native build.
    static KICAD_API_HOST_CONFIG Defaults();
};


/**
 * KiCad's headless API core, initialized in-process and driven one request at a time.
 *
 * This is the whole of what a host (the native stdio binary, the wasm C ABI) needs: bring
 * wxWidgets, PGM_BASE, the settings manager and the two statically-linked KIFACEs up, start a
 * KICAD_API_SERVER with no socket, install the document lifecycle handlers, and hand serialized
 * ApiRequest bytes to Dispatch().
 *
 * Exactly one instance may exist at a time (PGM_BASE and KIWAY are process-global).
 */
class KICAD_API_HOST
{
public:
    /// Receives the serialized bytes of each published kiapi.common.events.Event.
    using EVENT_SINK = std::function<void( const std::string& )>;

    KICAD_API_HOST();

    ~KICAD_API_HOST();

    /**
     * Bring KiCad up.  Safe to call once; a second call fails.
     *
     * @param aConfig is the host configuration
     * @param aSink receives published events (may be empty)
     * @param aError is filled with the failure reason when this returns false
     */
    bool Init( const KICAD_API_HOST_CONFIG& aConfig, EVENT_SINK aSink, std::string* aError );

    /**
     * Answer one request.  Never throws; a failure comes back as a serialized ApiResponse with
     * a non-AS_OK status.
     *
     * @param aRequestBytes is a serialized kiapi.common.ApiRequest
     * @return the serialized kiapi.common.ApiResponse
     */
    std::string Dispatch( const std::string& aRequestBytes );

    /**
     * Close the documents, stop the server, and tear wxWidgets down.  Safe to call more than
     * once, and called by the destructor.
     */
    void Shutdown();

    bool Ready() const { return m_ready; }

    /// The token the server generated (or the one from the config)
    std::string Token() const;

    /// Build a serialized ApiResponse carrying aStatus and aMessage, for hosts that have to
    /// answer without going through Dispatch() (re-entrancy, a request that never arrived).
    static std::string MakeErrorResponse( int aStatusCode, const std::string& aMessage );

private:
    std::unique_ptr<KICAD_API_SERVER> m_server;
    std::unique_ptr<API_SERVER_HOST>  m_serverHost;

    bool m_ready = false;
    bool m_wxStarted = false;
    bool m_pgmStarted = false;
};

#endif // KICAD_API_HOST_H
