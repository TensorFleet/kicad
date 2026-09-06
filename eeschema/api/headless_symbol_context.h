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

#ifndef KICAD_HEADLESS_SYMBOL_CONTEXT_H
#define KICAD_HEADLESS_SYMBOL_CONTEXT_H

#include <memory>

#include <lib_id.h>

#include <api/symbol_context.h>

class KIWAY;
class LIB_SYMBOL;
class PROJECT;


/// Symbol context for kicad-cli api-server: a symbol loaded from a library with no editor frame
class HEADLESS_SYMBOL_CONTEXT : public SYMBOL_CONTEXT
{
public:
    HEADLESS_SYMBOL_CONTEXT( std::unique_ptr<LIB_SYMBOL> aSymbol, const LIB_ID& aLibId, PROJECT* aProject,
                             KIWAY* aKiway = nullptr );

    ~HEADLESS_SYMBOL_CONTEXT() override;

    LIB_SYMBOL* GetSymbol() const override;

    LIB_ID GetLoadedLibId() const override;

    PROJECT& Prj() const override;

    KIWAY* GetKiway() const override { return m_kiway; }

    bool CanAcceptApiCommands() const override { return true; }

    bool SaveSymbol() override;

    bool SaveSymbolCopy( const wxString& aLibraryName, const wxString& aSymbolName, bool aOverwrite,
                         wxString* aError ) override;

private:
    std::unique_ptr<LIB_SYMBOL> m_symbol;
    LIB_ID                      m_libId;
    PROJECT*                    m_project;
    KIWAY*                      m_kiway;
};

#endif // KICAD_HEADLESS_SYMBOL_CONTEXT_H
