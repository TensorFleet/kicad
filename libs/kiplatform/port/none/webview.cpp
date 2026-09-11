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
 * "none" port: there is no embedded browser, so there are no cookies to move.
 */

#include <kiplatform/webview.h>


bool KIPLATFORM::WEBVIEW::SaveCookies( wxWebView* aWebView, const wxString& aTargetFile )
{
    return false;
}


bool KIPLATFORM::WEBVIEW::LoadCookies( wxWebView* aWebView, const wxString& aSourceFile )
{
    return false;
}


bool KIPLATFORM::WEBVIEW::DeleteCookies( wxWebView* aWebView )
{
    return false;
}
