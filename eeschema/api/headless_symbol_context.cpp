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

#include <api/headless_symbol_context.h>

#include <lib_symbol.h>
#include <libraries/symbol_library_adapter.h>
#include <project.h>
#include <project_sch.h>
#include <wx/debug.h>


HEADLESS_SYMBOL_CONTEXT::HEADLESS_SYMBOL_CONTEXT( std::unique_ptr<LIB_SYMBOL> aSymbol, const LIB_ID& aLibId,
                                                  PROJECT* aProject, KIWAY* aKiway ) :
        m_symbol( std::move( aSymbol ) ),
        m_libId( aLibId ),
        m_project( aProject ),
        m_kiway( aKiway )
{
    wxCHECK( m_symbol, /* void */ );
    wxCHECK( m_project, /* void */ );
}


HEADLESS_SYMBOL_CONTEXT::~HEADLESS_SYMBOL_CONTEXT() = default;


LIB_SYMBOL* HEADLESS_SYMBOL_CONTEXT::GetSymbol() const
{
    return m_symbol.get();
}


LIB_ID HEADLESS_SYMBOL_CONTEXT::GetLoadedLibId() const
{
    return m_libId;
}


PROJECT& HEADLESS_SYMBOL_CONTEXT::Prj() const
{
    wxASSERT( m_project );
    return *m_project;
}


bool HEADLESS_SYMBOL_CONTEXT::SaveSymbol()
{
    wxString error;
    return SaveSymbolCopy( m_libId.GetUniStringLibNickname(), m_libId.GetUniStringLibItemName(), true, &error );
}


bool HEADLESS_SYMBOL_CONTEXT::SaveSymbolCopy( const wxString& aLibraryName, const wxString& aSymbolName,
                                              bool aOverwrite, wxString* aError )
{
    if( !m_symbol || aLibraryName.IsEmpty() || aSymbolName.IsEmpty() )
    {
        if( aError )
            *aError = wxS( "library and symbol name are required" );

        return false;
    }

    try
    {
        SYMBOL_LIBRARY_ADAPTER* adapter = PROJECT_SCH::SymbolLibAdapter( m_project );

        if( !adapter->IsSymbolLibWritable( aLibraryName ) )
        {
            if( aError )
                *aError = wxString::Format( wxS( "library '%s' is read-only" ), aLibraryName );

            return false;
        }

        // The adapter takes ownership of what it is given, so it always gets a copy
        std::unique_ptr<LIB_SYMBOL> copy = std::make_unique<LIB_SYMBOL>( *m_symbol );
        copy->SetName( aSymbolName );
        copy->SetLibId( LIB_ID( aLibraryName, aSymbolName ) );

        if( adapter->SaveSymbol( aLibraryName, copy.release(), aOverwrite ) != SYMBOL_LIBRARY_ADAPTER::SAVE_OK )
        {
            if( aError )
                *aError = wxString::Format( wxS( "symbol '%s' already exists in library '%s'" ), aSymbolName,
                                            aLibraryName );

            return false;
        }

        return true;
    }
    catch( const IO_ERROR& ioe )
    {
        if( aError )
            *aError = ioe.What();

        return false;
    }
}
