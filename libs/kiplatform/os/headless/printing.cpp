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

#include <printing.h>


KIPLATFORM::PRINTING::PRINT_RESULT KIPLATFORM::PRINTING::PrintPDF( const std::string& aFile )
{
    // There is no print spooler to hand the document to.
    return PRINT_RESULT::UNSUPPORTED;
}


void KIPLATFORM::PRINTING::ResetPrintToFilePath( wxPrintData& aData )
{
}
