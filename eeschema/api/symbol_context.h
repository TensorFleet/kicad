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

#ifndef KICAD_SYMBOL_CONTEXT_H
#define KICAD_SYMBOL_CONTEXT_H

#include <wx/string.h>

class KIWAY;
class LIB_ID;
class LIB_SYMBOL;
class PROJECT;


/// The surface the symbol API handler needs from its host: a headless context today, a symbol
/// editor frame in the future.
class SYMBOL_CONTEXT
{
public:
    virtual ~SYMBOL_CONTEXT() = default;

    /// @return the symbol being edited (owned by the context)
    virtual LIB_SYMBOL* GetSymbol() const = 0;

    /// @return the library identifier the symbol was loaded from
    virtual LIB_ID GetLoadedLibId() const = 0;

    virtual PROJECT& Prj() const = 0;

    virtual KIWAY* GetKiway() const = 0;

    virtual bool CanAcceptApiCommands() const = 0;

    /// Write the symbol back to the library it was loaded from
    virtual bool SaveSymbol() = 0;

    /**
     * Write a copy of the symbol to a library.
     * @param aLibraryName is the library nickname
     * @param aSymbolName is the name the copy is saved under
     * @param aOverwrite allows replacing an existing symbol of that name
     * @param aError receives a description on failure
     */
    virtual bool SaveSymbolCopy( const wxString& aLibraryName, const wxString& aSymbolName, bool aOverwrite,
                                 wxString* aError ) = 0;
};

#endif // KICAD_SYMBOL_CONTEXT_H
