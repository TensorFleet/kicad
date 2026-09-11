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

#pragma once

/**
 * @file headless_grid_table.h
 *
 * A stand-in for wxGridTableBase (and for KiCad's WX_GRID_TABLE_BASE on top of it) for the
 * KICAD_HEADLESS_API build.
 *
 * The fields tables' data models are two things at once: the BOM's data source, which the CLI
 * and the API job handlers need, and a wxGrid table, which only the dialogs need.  wxGrid lives
 * in wxCore, which the headless link does not have (and which a wxBase-only / wasm link cannot
 * have), so under KICAD_HEADLESS_API the data model derives from this instead.  It declares the
 * same virtuals the model overrides, so the model's own class definition is unchanged; only the
 * grid-facing members (cell attributes, renderers, table messages) are compiled out.
 *
 * This header is deliberately dependency-free: wxString and ROW_STATE, nothing else.
 */

#include <cstddef>

#include <wx/string.h>

#include <widgets/wx_grid.h>    // ROW_STATE


/**
 * The subset of the wxGridTableBase / WX_GRID_TABLE_BASE interface that the fields tables'
 * data models implement, with no wxCore behind it.
 *
 * There is no view: GetView() has no headless equivalent, so every call site that talks to a
 * grid is guarded with `#ifndef KICAD_HEADLESS_API` rather than being given a null view to
 * check, which would leave wxGrid symbols in the link.
 */
class HEADLESS_GRID_TABLE_BASE
{
public:
    HEADLESS_GRID_TABLE_BASE() = default;
    virtual ~HEADLESS_GRID_TABLE_BASE() = default;

    HEADLESS_GRID_TABLE_BASE( const HEADLESS_GRID_TABLE_BASE& ) = delete;
    HEADLESS_GRID_TABLE_BASE& operator=( const HEADLESS_GRID_TABLE_BASE& ) = delete;

    virtual int      GetNumberRows() = 0;
    virtual int      GetNumberCols() = 0;
    virtual wxString GetValue( int aRow, int aCol ) = 0;
    virtual void     SetValue( int aRow, int aCol, const wxString& aValue ) = 0;

    virtual bool IsEmptyCell( int aRow, int aCol ) { return GetValue( aRow, aCol ).IsEmpty(); }

    virtual wxString GetColLabelValue( int aCol ) { return wxEmptyString; }
    virtual void     SetColLabelValue( int aCol, const wxString& aLabel ) {}
    virtual wxString GetRowLabelValue( int aRow ) { return wxEmptyString; }

    virtual bool AppendRows( size_t aNumRows = 1 ) { return false; }
    virtual bool DeleteRows( size_t aPos = 0, size_t aNumRows = 1 ) { return false; }

    virtual void Clear()
    {
        if( GetNumberRows() )
            DeleteRows( 0, GetNumberRows() );
    }

    virtual bool      IsExpanderColumn( int aCol ) const { return false; }
    virtual ROW_STATE GetRowState( int aRow ) const { return ROW_STATE::NON_EXPANDABLE; }

    virtual bool     HasUndoStateSerialization() const { return false; }
    virtual wxString SerializeUndoState() const { return wxEmptyString; }
    virtual void     RestoreUndoState( const wxString& aState ) {}
};
