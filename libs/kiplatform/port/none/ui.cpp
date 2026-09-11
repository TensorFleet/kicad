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
 * "none" port: no toolkit is present.  Nothing here is reachable from the
 * headless API path; the definitions exist so that anything that merely
 * references them still links.
 */

#include <kiplatform/ui.h>

#include <wx/colour.h>
#include <wx/gdicmn.h>


bool KIPLATFORM::UI::IsDarkTheme()
{
    return false;
}


wxColour KIPLATFORM::UI::GetDialogBGColour()
{
    return wxColour( 255, 255, 255 );
}


void KIPLATFORM::UI::GetInfoBarColours( wxColour& aFGColour, wxColour& aBGColour )
{
    aFGColour = wxColour( 0, 0, 0 );
    aBGColour = wxColour( 255, 255, 255 );
}


void KIPLATFORM::UI::ForceFocus( wxWindow* aWindow )
{
}


bool KIPLATFORM::UI::IsWindowActive( wxWindow* aWindow )
{
    return false;
}


void KIPLATFORM::UI::ReparentModal( wxNonOwnedWindow* aWindow )
{
}


void KIPLATFORM::UI::ReparentWindow( wxNonOwnedWindow* aWindow, wxTopLevelWindow* aParent )
{
}


void KIPLATFORM::UI::FixupCancelButtonCmdKeyCollision( wxWindow* aWindow )
{
}


bool KIPLATFORM::UI::IsStockCursorOk( wxStockCursor aCursor )
{
    return false;
}


void KIPLATFORM::UI::LargeChoiceBoxHack( wxChoice* aChoice )
{
}


void KIPLATFORM::UI::EllipsizeChoiceBox( wxChoice* aChoice )
{
}


double KIPLATFORM::UI::GetPixelScaleFactor( const wxWindow* aWindow )
{
    return 1.0;
}


double KIPLATFORM::UI::GetContentScaleFactor( const wxWindow* aWindow )
{
    return 1.0;
}


wxSize KIPLATFORM::UI::GetUnobscuredSize( const wxWindow* aWindow )
{
    return wxSize();
}


void KIPLATFORM::UI::SetOverlayScrolling( const wxWindow* aWindow, bool overlay )
{
}


bool KIPLATFORM::UI::AllowIconsInMenus()
{
    return false;
}


wxPoint KIPLATFORM::UI::GetMousePosition()
{
    return wxPoint( 0, 0 );
}


bool KIPLATFORM::UI::WarpPointer( wxWindow* aWindow, int aX, int aY )
{
    return false;
}


void KIPLATFORM::UI::ImmControl( wxWindow* aWindow, bool aEnable )
{
}


void KIPLATFORM::UI::ImeNotifyCancelComposition( wxWindow* aWindow )
{
}


bool KIPLATFORM::UI::InfiniteDragPrepareWindow( wxWindow* aWindow )
{
    return false;
}


void KIPLATFORM::UI::InfiniteDragReleaseWindow()
{
}


void KIPLATFORM::UI::EnsureVisible( wxWindow* aWindow )
{
}


void KIPLATFORM::UI::StabilizeWindowPosition( wxWindow* aWindow )
{
}


void KIPLATFORM::UI::SetFloatLevel( wxWindow* aWindow )
{
}


void KIPLATFORM::UI::ReleaseChildWindow( wxNonOwnedWindow* aWindow )
{
}


void KIPLATFORM::UI::AllowNetworkFileSystems( wxDialog* aDialog )
{
}


void KIPLATFORM::UI::CancelPendingScroll( wxDataViewCtrl* aCtrl )
{
}


void KIPLATFORM::UI::SetWMClass( wxWindow* aWindow, const wxString& aClass )
{
}
