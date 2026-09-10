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
 * @file headless_gui.cpp
 * Headless replacements for the handful of free functions that live in GUI translation units
 * but are reached from the API path.  Only compiled under KICAD_HEADLESS_API, where the
 * originals (confirm.cpp, widgets/ui_common.cpp) are not built.
 *
 * The message boxes become log lines and answer the way an unattended run should: a question
 * takes its default, a confirmation is declined.  Nothing here waits for a user.
 */

#include <wx/log.h>
#include <wx/string.h>

#include <confirm.h>
#include <widgets/ui_common.h>


// ---------------------------------------------------------------------------------------------
// confirm.cpp
// ---------------------------------------------------------------------------------------------

void DisplayError( wxWindow* aParent, const wxString& aText )
{
    wxLogError( "%s", aText );
}


void DisplayErrorMessage( wxWindow* aParent, const wxString& aText, const wxString& aExtraInfo )
{
    if( aExtraInfo.IsEmpty() )
        wxLogError( "%s", aText );
    else
        wxLogError( "%s: %s", aText, aExtraInfo );
}


void DisplayInfoMessage( wxWindow* aParent, const wxString& aMessage, const wxString& aExtraInfo )
{
    if( aExtraInfo.IsEmpty() )
        wxLogMessage( "%s", aMessage );
    else
        wxLogMessage( "%s: %s", aMessage, aExtraInfo );
}


bool IsOK( wxWindow* aParent, const wxString& aMessage )
{
    // Nobody is there to say yes.
    wxLogMessage( "%s -- declined (headless)", aMessage );
    return false;
}


int OKOrCancelDialog( wxWindow* aParent, const wxString& aWarning, const wxString& aMessage,
                      const wxString& aDetailedMessage, const wxString& aOKLabel,
                      const wxString& aCancelLabel, bool* aApplyToAll )
{
    wxLogMessage( "%s -- cancelled (headless)", aMessage );

    if( aApplyToAll )
        *aApplyToAll = false;

    return wxID_CANCEL;
}


// ---------------------------------------------------------------------------------------------
// widgets/ui_common.cpp -- the two severity spellings are the report/settings file format, not
// UI, and BOARD_DESIGN_SETTINGS and ERC_SETTINGS read them from JSON.
// ---------------------------------------------------------------------------------------------

SEVERITY SeverityFromString( const wxString& aSeverity )
{
    if( aSeverity == wxT( "warning" ) )
        return RPT_SEVERITY_WARNING;
    else if( aSeverity == wxT( "ignore" ) )
        return RPT_SEVERITY_IGNORE;
    else
        return RPT_SEVERITY_ERROR;
}


wxString SeverityToString( const SEVERITY& aSeverity )
{
    if( aSeverity == RPT_SEVERITY_IGNORE )
        return wxT( "ignore" );
    else if( aSeverity == RPT_SEVERITY_WARNING )
        return wxT( "warning" );
    else
        return wxT( "error" );
}


wxString KIUI::EllipsizeStatusText( wxWindow* aWindow, const wxString& aString )
{
    return aString;
}


wxString KIUI::EllipsizeMenuText( const wxString& aString )
{
    return aString;
}
