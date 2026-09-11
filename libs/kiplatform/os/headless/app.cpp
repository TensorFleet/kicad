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
 * Headless backend: there is no desktop session, no window manager and no
 * dynamic module loading, so every entry point here is a no-op.
 */

#include <kiplatform/app.h>

#include <wx/string.h>


bool KIPLATFORM::APP::Init()
{
    return true;
}


void KIPLATFORM::APP::EnableDarkMode( bool aForce )
{
}


bool KIPLATFORM::APP::AttachConsole( bool aTryAlloc )
{
    // The headless build is always started from a console.
    return true;
}


bool KIPLATFORM::APP::IsOperatingSystemUnsupported()
{
    return false;
}


bool KIPLATFORM::APP::RegisterApplicationRestart( const wxString& aCommandLine )
{
    return true;
}


bool KIPLATFORM::APP::UnregisterApplicationRestart()
{
    return true;
}


bool KIPLATFORM::APP::SupportsShutdownBlockReason()
{
    return false;
}


void KIPLATFORM::APP::RemoveShutdownBlockReason( wxWindow* aWindow )
{
}


void KIPLATFORM::APP::SetShutdownBlockReason( wxWindow* aWindow, const wxString& aReason )
{
}


void KIPLATFORM::APP::ForceTimerMessagesToBeCreatedIfNecessary()
{
}


void KIPLATFORM::APP::AddDynamicLibrarySearchPath( const wxString& aPath )
{
    // The headless build links every kiface statically; nothing is ever dlopen'd.
}
