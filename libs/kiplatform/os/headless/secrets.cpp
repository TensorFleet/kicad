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
 * Headless backend: there is no OS keyring, so nothing is ever stored and every
 * lookup misses.  Callers all treat a false return as "no stored credential".
 */

#include <kiplatform/secrets.h>


bool KIPLATFORM::SECRETS::StoreSecret( const wxString& aService, const wxString& aKey,
                                       const wxString& aSecret )
{
    return false;
}


bool KIPLATFORM::SECRETS::GetSecret( const wxString& aService, const wxString& aKey,
                                     wxString& aSecret )
{
    return false;
}


bool KIPLATFORM::SECRETS::DeleteSecret( const wxString& aService, const wxString& aKey )
{
    return false;
}
