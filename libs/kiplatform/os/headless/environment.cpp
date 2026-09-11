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
 * Headless backend.  This mirrors os/unix/environment.cpp but resolves the user
 * paths from the XDG environment variables directly instead of going through
 * GLib, so the headless core needs neither GTK nor GIO.
 */

#include <kiplatform/environment.h>

#include <wx/filename.h>
#include <wx/utils.h>


namespace
{
/**
 * Return $aVar when it names an absolute path, else $HOME plus @a aFallback.
 *
 * This is the XDG base directory resolution rule: a relative value in the
 * environment variable is required to be ignored.
 */
wxString xdgDir( const wxString& aVar, const wxString& aFallback )
{
    wxString value;

    if( wxGetEnv( aVar, &value ) && !value.IsEmpty() && wxFileName( value, wxEmptyString ).IsAbsolute() )
        return value;

    wxFileName path;
    path.AssignDir( wxFileName::GetHomeDir() );

    for( const wxString& part : wxSplit( aFallback, '/' ) )
    {
        if( !part.IsEmpty() )
            path.AppendDir( part );
    }

    path.MakeAbsolute();

    // GetPath() gives the directory without a trailing separator, which is what the
    // GLib g_get_user_*_dir() calls this replaces return.
    return path.GetPath();
}
} // namespace


void KIPLATFORM::ENV::Init()
{
    // Nothing to set up: no window manager, no display server, no input devices.
}


bool KIPLATFORM::ENV::MoveToTrash( const wxString& aPath, wxString& aError )
{
    // There is no desktop trash can.  Report failure rather than silently deleting:
    // callers fall back to asking before a permanent removal.
    aError = wxT( "Moving files to the trash is not supported in a headless build" );
    return false;
}


bool KIPLATFORM::ENV::IsNetworkPath( const wxString& aPath )
{
    // Matches the unix backend: behaviour is "nerfed" for network paths, and we
    // cannot tell, so claim local.
    return false;
}


wxString KIPLATFORM::ENV::GetDocumentsPath()
{
    return xdgDir( wxT( "XDG_DATA_HOME" ), wxT( ".local/share" ) );
}


wxString KIPLATFORM::ENV::GetUserConfigPath()
{
    return xdgDir( wxT( "XDG_CONFIG_HOME" ), wxT( ".config" ) );
}


wxString KIPLATFORM::ENV::GetUserDataPath()
{
    return xdgDir( wxT( "XDG_DATA_HOME" ), wxT( ".local/share" ) );
}


wxString KIPLATFORM::ENV::GetUserLocalDataPath()
{
    return xdgDir( wxT( "XDG_DATA_HOME" ), wxT( ".local/share" ) );
}


wxString KIPLATFORM::ENV::GetUserCachePath()
{
    return xdgDir( wxT( "XDG_CACHE_HOME" ), wxT( ".cache" ) );
}


bool KIPLATFORM::ENV::GetSystemProxyConfig( const wxString& aURL, PROXY_CONFIG& aCfg )
{
    // The headless core has no network stack.
    return false;
}


bool KIPLATFORM::ENV::VerifyFileSignature( const wxString& aPath )
{
    return true;
}


wxString KIPLATFORM::ENV::GetAppUserModelId()
{
    return wxEmptyString;
}


void KIPLATFORM::ENV::SetAppDetailsForWindow( wxWindow* aWindow, const wxString& aRelaunchCommand,
                                              const wxString& aRelaunchDisplayName )
{
}


wxString KIPLATFORM::ENV::GetCommandLineStr()
{
    return wxEmptyString;
}


void KIPLATFORM::ENV::AddToRecentDocs( const wxString& aPath )
{
}
