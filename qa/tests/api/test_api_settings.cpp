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
 * End-to-end tests for the settings commands (ListColorThemes, GetColorTheme, GetAppSettings,
 * GetGraphicsDefaults, SetGraphicsDefaults).  Since 11.0.
 */

#include <boost/test/unit_test.hpp>
#include <wx/filefn.h>
#include <wx/filename.h>

#include "api_e2e_utils.h"

#include <api/board/board_commands.pb.h>
#include <api/common/commands/settings_commands.pb.h>

using namespace kiapi::common::commands;
using kiapi::common::types::DocumentSpecifier;


namespace
{

template <typename REQUEST, typename RESPONSE>
bool Send( API_TEST_CLIENT& aClient, const REQUEST& aRequest, RESPONSE* aOut, wxString* aError )
{
    kiapi::common::ApiResponse response;

    if( !aClient.SendCommand( aRequest, &response ) )
    {
        *aError = aClient.LastError();
        return false;
    }

    if( response.status().status() != kiapi::common::AS_OK )
    {
        *aError = response.status().error_message();
        return false;
    }

    if( aOut && !response.message().UnpackTo( aOut ) )
    {
        *aError = wxS( "Failed to unpack response" );
        return false;
    }

    return true;
}


template <typename REQUEST>
kiapi::common::ApiStatusCode SendStatus( API_TEST_CLIENT& aClient, const REQUEST& aRequest )
{
    kiapi::common::ApiResponse response;

    if( !aClient.SendCommand( aRequest, &response ) )
        return kiapi::common::AS_UNKNOWN;

    return response.status().status();
}


/// A throw-away project folder holding a copy of the kitchen sink project and board
class TEMP_SETTINGS_PROJECT
{
public:
    ~TEMP_SETTINGS_PROJECT()
    {
        if( !m_dir.IsEmpty() && wxFileName::DirExists( m_dir ) )
            wxFileName::Rmdir( m_dir, wxPATH_RMDIR_RECURSIVE );
    }

    bool Create()
    {
        wxString token = wxFileName::CreateTempFileName( wxS( "kicad-api-settings-" ) );

        if( token.IsEmpty() )
            return false;

        wxRemoveFile( token );

        if( !wxFileName::Mkdir( token, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) )
            return false;

        m_dir = token;

        wxString testDataDir = wxString::FromUTF8( KI_TEST::GetPcbnewTestDataDir() );

        for( const wxString& ext : { wxS( "kicad_pro" ), wxS( "kicad_pcb" ), wxS( "kicad_dru" ) } )
        {
            wxFileName src( testDataDir, wxS( "api_kitchen_sink." ) + ext );

            if( !wxCopyFile( src.GetFullPath(), wxFileName( m_dir, src.GetFullName() ).GetFullPath(), true ) )
                return false;
        }

        m_projectPath = wxFileName( m_dir, wxS( "api_kitchen_sink.kicad_pro" ) ).GetFullPath();
        m_boardPath = wxFileName( m_dir, wxS( "api_kitchen_sink.kicad_pcb" ) ).GetFullPath();
        return true;
    }

    const wxString& ProjectPath() const { return m_projectPath; }
    const wxString& BoardPath() const { return m_boardPath; }

private:
    wxString m_dir;
    wxString m_projectPath;
    wxString m_boardPath;
};

} // namespace


BOOST_FIXTURE_TEST_CASE( SettingsColorThemes, API_SERVER_E2E_FIXTURE )
{
    BOOST_REQUIRE_MESSAGE( Start(), LastError() );

    wxString error;

    // The built-in themes are always there
    ColorThemesResponse themes;
    BOOST_REQUIRE_MESSAGE( Send( Client(), ListColorThemes(), &themes, &error ), error );
    BOOST_CHECK_GE( themes.themes_size(), 1 );

    bool sawDefault = false;

    for( const ColorThemeInfo& theme : themes.themes() )
        sawDefault |= theme.name() == "KiCad Default" && theme.read_only();

    BOOST_CHECK( sawDefault );

    // The default theme, by empty name and by display name, with a color for every key
    GetColorTheme request;

    ColorThemeResponse theme;
    BOOST_REQUIRE_MESSAGE( Send( Client(), request, &theme, &error ), error );
    BOOST_CHECK_EQUAL( theme.theme().name(), "KiCad Default" );
    BOOST_CHECK_GT( theme.colors_size(), 100 );

    bool sawCopper = false;
    bool sawWire = false;

    for( const ColorThemeEntry& entry : theme.colors() )
    {
        BOOST_CHECK( !entry.key().empty() );
        BOOST_CHECK( entry.color().a() > 0.0 || entry.key().find( "background" ) != std::string::npos );

        if( entry.key() == "board.copper.f" )
        {
            sawCopper = true;
            BOOST_CHECK_EQUAL( entry.layer(), static_cast<int>( F_Cu ) );
        }

        sawWire |= entry.key() == "schematic.wire";
    }

    BOOST_CHECK( sawCopper );
    BOOST_CHECK( sawWire );

    request.set_name( "KiCad Default" );
    BOOST_REQUIRE_MESSAGE( Send( Client(), request, &theme, &error ), error );

    request.set_name( "no such theme" );
    BOOST_CHECK_EQUAL( SendStatus( Client(), request ), kiapi::common::AS_BAD_REQUEST );
}


BOOST_FIXTURE_TEST_CASE( SettingsAppSettings, API_SERVER_E2E_FIXTURE )
{
    BOOST_REQUIRE_MESSAGE( Start(), LastError() );

    wxString error;

    // Readable without any document open
    GetAppSettings request;
    request.set_app( APP_PCB_EDITOR );

    AppSettings settings;
    BOOST_REQUIRE_MESSAGE( Send( Client(), request, &settings, &error ), error );
    BOOST_CHECK_EQUAL( settings.app(), APP_PCB_EDITOR );
    BOOST_CHECK_GT( settings.grids_size(), 0 );
    BOOST_CHECK( settings.units() != US_UNKNOWN );
    BOOST_CHECK( wxString::FromUTF8( settings.settings_file() ).EndsWith( wxS( "pcbnew.json" ) ) );

    request.set_app( APP_SCHEMATIC_EDITOR );
    BOOST_REQUIRE_MESSAGE( Send( Client(), request, &settings, &error ), error );
    BOOST_CHECK( settings.defaults().contains( "default_wire_thickness" ) );
    BOOST_CHECK( settings.defaults().contains( "default_text_size" ) );

    request.set_app( APP_UNKNOWN );
    BOOST_CHECK_EQUAL( SendStatus( Client(), request ), kiapi::common::AS_BAD_REQUEST );
}


BOOST_FIXTURE_TEST_CASE( SettingsGraphicsDefaults, API_SERVER_E2E_FIXTURE )
{
    BOOST_REQUIRE_MESSAGE( Start(), LastError() );

    TEMP_SETTINGS_PROJECT project;
    BOOST_REQUIRE( project.Create() );

    DocumentSpecifier projectDoc;
    DocumentSpecifier board;
    BOOST_REQUIRE_MESSAGE( Client().OpenDocument( project.ProjectPath(), kiapi::common::types::DOCTYPE_PROJECT,
                                                  &projectDoc ),
                           "OpenDocument failed: " + Client().LastError() );
    BOOST_REQUIRE_MESSAGE( Client().OpenDocument( project.BoardPath(), kiapi::common::types::DOCTYPE_PCB, &board ),
                           "OpenDocument failed: " + Client().LastError() );

    wxString error;

    kiapi::board::commands::GetGraphicsDefaults get;
    *get.mutable_board() = board;

    kiapi::board::commands::GraphicsDefaultsResponse before;
    BOOST_REQUIRE_MESSAGE( Send( Client(), get, &before, &error ), error );
    BOOST_REQUIRE_EQUAL( before.defaults().layers_size(), 6 );

    for( const kiapi::board::BoardLayerGraphicsDefaults& layer : before.defaults().layers() )
        BOOST_CHECK_GT( layer.text().size().x_nm(), 0 );

    kiapi::board::commands::SetGraphicsDefaults set;
    *set.mutable_board() = board;

    kiapi::board::BoardLayerGraphicsDefaults* silk = set.mutable_defaults()->add_layers();
    silk->set_layer( kiapi::board::BLC_SILKSCREEN );
    silk->mutable_line_thickness()->set_value_nm( 200000 );
    silk->mutable_text()->mutable_size()->set_x_nm( 1200000 );
    silk->mutable_text()->mutable_size()->set_y_nm( 1200000 );
    silk->mutable_text()->mutable_stroke_width()->set_value_nm( 180000 );
    silk->mutable_text()->set_italic( true );

    kiapi::board::commands::GraphicsDefaultsResponse after;
    BOOST_REQUIRE_MESSAGE( Send( Client(), set, &after, &error ), error );
    BOOST_REQUIRE_EQUAL( after.defaults().layers_size(), 6 );

    for( int ii = 0; ii < after.defaults().layers_size(); ii++ )
    {
        const kiapi::board::BoardLayerGraphicsDefaults& layer = after.defaults().layers( ii );

        if( layer.layer() == kiapi::board::BLC_SILKSCREEN )
        {
            BOOST_CHECK_EQUAL( layer.line_thickness().value_nm(), 200000 );
            BOOST_CHECK_EQUAL( layer.text().size().x_nm(), 1200000 );
            BOOST_CHECK_EQUAL( layer.text().stroke_width().value_nm(), 180000 );
            BOOST_CHECK( layer.text().italic() );
        }
        else
        {
            BOOST_CHECK_EQUAL( layer.line_thickness().value_nm(), before.defaults().layers( ii ).line_thickness().value_nm() );
            BOOST_CHECK_EQUAL( layer.text().size().x_nm(), before.defaults().layers( ii ).text().size().x_nm() );
        }
    }

    // A zero text size is refused
    silk->mutable_text()->mutable_size()->set_x_nm( 0 );
    BOOST_CHECK_EQUAL( SendStatus( Client(), set ), kiapi::common::AS_BAD_REQUEST );
}
