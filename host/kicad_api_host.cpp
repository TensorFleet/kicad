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

#include <clocale>
#include <cstdlib>
#include <exception>

#include <wx/app.h>
#include <wx/init.h>
#include <wx/log.h>
#include <wx/string.h>
#include <wx/utils.h>

#include <nlohmann/json.hpp>

#include <api/api_job_registry.h>
#include <api/api_server.h>
#include <api/api_server_host.h>
#include <api/common/envelope.pb.h>
#include <bin_mod.h>
#include <thread_pool.h>
#include <kiplatform/app.h>
#include <kiplatform/environment.h>
#include <kiway.h>
#include <libraries/library_manager.h>
#include <pgm_base.h>
#include <project.h>
#include <settings/kicad_settings.h>
#include <settings/settings_manager.h>

#include "kicad_api_host.h"
#include "static_kifaces.h"


/// KiCad's one KIWAY.  include/kiway.h declares it extern for every binary that links common.
KIWAY Kiway( KFCTL_CPP_PROJECT_SUITE | KFCTL_CLI );


namespace
{

/**
 * PGM_BASE for a headless API host: the kicad-cli program object minus the argument parser,
 * the git backend (libgit2 is on its way out of this build) and MacOpenFile.
 */
class HOST_PGM : public PGM_BASE
{
public:
    HOST_PGM() :
            m_bm( "kicad" )     // the same config file name kicad-cli uses
    {}

    ~HOST_PGM() throw() override { Destroy(); }

    bool OnPgmInit();
    void OnPgmExit();

    void MacOpenFile( const wxString& aFileName ) override {}

    APP_SETTINGS_BASE* PgmSettings() { return m_bm.m_config; }

    /// Designed, like PGM_KICAD's, to be safe to call more than once
    void Destroy()
    {
        m_bm.End();
        PGM_BASE::Destroy();
    }

protected:
    BIN_MOD m_bm;
};


HOST_PGM g_program;


bool HOST_PGM::OnPgmInit()
{
    PGM_BASE::BuildArgvUtf8();
    App().SetAppDisplayName( wxT( "kicad-api-host" ) );

    if( !InitPgm( true ) )
        return false;

    m_bm.InitSettings( new KICAD_SETTINGS );
    GetSettingsManager().RegisterSettings( PgmSettings() );
    GetSettingsManager().SetKiway( &Kiway );
    m_bm.Init();

    GetLibraryManager().LoadGlobalTables();

    return true;
}


void HOST_PGM::OnPgmExit()
{
    // Abort and wait on any background jobs (inline under this build, but purge anyway)
    GetKiCadThreadPool().purge();
    GetKiCadThreadPool().wait();

    for( KIWAY::FACE_T face : { KIWAY::FACE_SCH, KIWAY::FACE_PCB } )
    {
        if( KIFACE* kiface = Kiway.KiFACE( face, false ) )
            kiface->Reset();
    }

    if( m_settings_manager && m_settings_manager->IsOK() )
    {
        SaveCommonSettings();
        m_settings_manager->Save();

        // Unload projects while the kiface DRC/ERC severity tables their PROJECT_FILE serializes
        // against are still alive; deferring to static teardown crashes
        for( const wxString& projectPath : m_settings_manager->GetOpenProjects() )
        {
            if( PROJECT* project = m_settings_manager->GetProject( projectPath ) )
                m_settings_manager->UnloadProject( project, false );
        }
    }

    Kiway.OnKiwayEnd();

    // Release module settings while the settings manager is still available.
    Destroy();

    m_settings_manager.reset();
}


/**
 * The wxAppConsole the API host runs under.  There is no event loop: wxEntryStart() brings wx
 * up, and requests are answered on the caller's thread.
 */
class HOST_APP : public wxAppConsole
{
public:
    HOST_APP() :
            wxAppConsole()
    {
        SetPgm( &g_program );

        // Init the environment each platform wants
        KIPLATFORM::ENV::Init();
    }

    bool OnInit() override { return true; }

    int OnRun() override { return 0; }

    int FilterEvent( wxEvent& aEvent ) override { return Event_Skip; }
};


void setEnvVar( const std::string& aName, const std::string& aValue )
{
    wxSetEnv( wxString::FromUTF8( aName ), wxString::FromUTF8( aValue ) );
}

} // namespace


KICAD_API_HOST_CONFIG KICAD_API_HOST_CONFIG::Defaults()
{
    KICAD_API_HOST_CONFIG config;

    return config;
}


bool KICAD_API_HOST_CONFIG::FromJson( const std::string& aJson, KICAD_API_HOST_CONFIG* aConfig,
                                      std::string* aError )
{
    KICAD_API_HOST_CONFIG config = Defaults();

    std::string trimmed = aJson;
    trimmed.erase( 0, trimmed.find_first_not_of( " \t\r\n" ) );

    if( !trimmed.empty() )
    {
        try
        {
            nlohmann::json js = nlohmann::json::parse( trimmed );

            if( !js.is_object() )
            {
                if( aError )
                    *aError = "host config must be a JSON object";

                return false;
            }

            if( js.contains( "home" ) && js.at( "home" ).is_string() )
                config.home = js.at( "home" ).get<std::string>();

            if( js.contains( "share" ) && js.at( "share" ).is_string() )
                config.share = js.at( "share" ).get<std::string>();

            if( js.contains( "fonts" ) && js.at( "fonts" ).is_string() )
                config.fonts = js.at( "fonts" ).get<std::string>();

            if( js.contains( "preload" ) && js.at( "preload" ).is_string() )
                config.preload = js.at( "preload" ).get<std::string>();

            if( js.contains( "token" ) && js.at( "token" ).is_string() )
                config.token = js.at( "token" ).get<std::string>();

            if( js.contains( "publishEvents" ) && js.at( "publishEvents" ).is_boolean() )
                config.publishEvents = js.at( "publishEvents" ).get<bool>();

            if( js.contains( "env" ) && js.at( "env" ).is_object() )
            {
                for( const auto& [key, value] : js.at( "env" ).items() )
                {
                    if( value.is_string() )
                        config.env[key] = value.get<std::string>();
                }
            }
        }
        catch( const std::exception& e )
        {
            if( aError )
                *aError = std::string( "could not parse the host config: " ) + e.what();

            return false;
        }
    }

    *aConfig = config;
    return true;
}


KICAD_API_HOST::KICAD_API_HOST()
{
}


KICAD_API_HOST::~KICAD_API_HOST()
{
    Shutdown();
}


bool KICAD_API_HOST::Init( const KICAD_API_HOST_CONFIG& aConfig, EVENT_SINK aSink,
                           std::string* aError )
{
    auto fail =
            [&]( const std::string& aMessage )
            {
                if( aError )
                    *aError = aMessage;

                return false;
            };

    if( m_ready )
        return fail( "the API host is already initialized" );

    // musl starts in a "C" locale whose wctomb() rejects non-ASCII, which makes swprintf() (and
    // therefore wxString::Format()) silently return an empty string.  Ask for UTF-8 explicitly;
    // this is a no-op where "C" is already UTF-8 capable, and falls back where it is unsupported.
    if( !std::setlocale( LC_ALL, "C.UTF-8" ) )
    {
        if( !std::setlocale( LC_ALL, "en_US.UTF-8" ) )
            std::setlocale( LC_ALL, "C" );
    }

    if( !aConfig.home.empty() )
        setEnvVar( "HOME", aConfig.home );

    if( !aConfig.share.empty() )
    {
        // PATHS::GetStockDataPath() honours this, and the stock symbol/footprint/template paths
        // (and therefore the KICAD10_* defaults PGM_BASE writes) all derive from it.
        setEnvVar( "KICAD_STOCK_DATA_HOME", aConfig.share );
    }

    if( !aConfig.fonts.empty() )
    {
        // common/font/fontconfig_manifest.cpp reads this on its first lookup; nothing else in
        // KiCad looks at it, and leaving it unset is the stroke-font-only behaviour.
        setEnvVar( "KICAD_FONTS_DIR", aConfig.fonts );
    }

    for( const auto& [name, value] : aConfig.env )
        setEnvVar( name, value );

    try
    {
        // wx owns the app object from here on: wxEntryCleanup() deletes it.
        wxApp::SetInstance( new HOST_APP() );

        static char  arg0[] = "kicad-api-host";
        static char* argv[] = { arg0, nullptr };
        int          argc = 1;

        if( !wxEntryStart( argc, argv ) )
            return fail( "wxEntryStart() failed" );

        m_wxStarted = true;

        if( !KIPLATFORM::APP::Init() )
            return fail( "KIPLATFORM::APP::Init() failed" );

        // The kifaces are linked into this image; KIWAY::KiFACE() prefers these over dlopen.
        // Registered before OnPgmInit() because loading the global library tables reaches them.
        KIWAY::RegisterStaticKiface( KIWAY::FACE_PCB, &KifaceGetterPcb );
        KIWAY::RegisterStaticKiface( KIWAY::FACE_SCH, &KifaceGetterSch );
        KIWAY::RegisterStaticKiface( KIWAY::FACE_CVPCB, &KifaceGetterCvPcb );

        if( !g_program.OnPgmInit() )
            return fail( "KiCad program initialization failed" );

        m_pgmStarted = true;

        // There is no thread to run jobs on, and no event loop to report their progress from.
        API_JOB_REGISTRY::Instance().SetInlineMode( true );

        m_server = std::make_unique<KICAD_API_SERVER>( false );
        m_server->SetToken( aConfig.token );
        m_server->SetPublishEvents( aConfig.publishEvents );

        m_serverHost = std::make_unique<API_SERVER_HOST>( Kiway, *m_server );
        m_serverHost->Install();

        m_server->StartInProcess( std::move( aSink ) );

        if( !m_server->Running() )
            return fail( "failed to start the in-process API server" );

        wxString preloadError;

        if( !m_serverHost->Preload( wxString::FromUTF8( aConfig.preload ), &preloadError ) )
            return fail( preloadError.utf8_string() );

        m_server->SetReadyToReply( true );
    }
    catch( const std::exception& e )
    {
        return fail( std::string( "exception during initialization: " ) + e.what() );
    }
    catch( ... )
    {
        return fail( "unknown exception during initialization" );
    }

    m_ready = true;
    return true;
}


std::string KICAD_API_HOST::Dispatch( const std::string& aRequestBytes )
{
    if( !m_ready || !m_server )
        return MakeErrorResponse( kiapi::common::ApiStatusCode::AS_NOT_READY,
                                  "the KiCad API host is not initialized" );

    try
    {
        // Between requests is the only time this host has to itself, so it is where the jobs
        // inline mode is holding actually run.  Doing it here rather than inside Run() gives
        // the client the same chance to subscribe to JobProgress that a socket host gives it.
        API_JOB_REGISTRY::Instance().RunDeferred();

        return m_server->DispatchBytes( aRequestBytes );
    }
    catch( const std::exception& e )
    {
        return MakeErrorResponse( kiapi::common::ApiStatusCode::AS_UNKNOWN,
                                  std::string( "unhandled exception: " ) + e.what() );
    }
    catch( ... )
    {
        return MakeErrorResponse( kiapi::common::ApiStatusCode::AS_UNKNOWN,
                                  "unhandled exception" );
    }
}


void KICAD_API_HOST::Shutdown()
{
    m_ready = false;

    try
    {
        if( m_serverHost )
        {
            m_serverHost->Shutdown();
            m_serverHost.reset();
        }

        if( m_server )
        {
            m_server->Stop();
            m_server.reset();
        }

        if( m_pgmStarted )
        {
            g_program.OnPgmExit();
            m_pgmStarted = false;
        }

        if( m_wxStarted )
        {
            wxEntryCleanup();
            m_wxStarted = false;
        }
    }
    catch( ... )
    {
        // Shutdown is best effort; there is nobody left to report to.
    }
}


std::string KICAD_API_HOST::Token() const
{
    return m_server ? m_server->Token() : std::string();
}


std::string KICAD_API_HOST::MakeErrorResponse( int aStatusCode, const std::string& aMessage )
{
    kiapi::common::ApiResponse response;

    response.mutable_status()->set_status(
            static_cast<kiapi::common::ApiStatusCode>( aStatusCode ) );
    response.mutable_status()->set_error_message( aMessage );

    return response.SerializeAsString();
}
