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

#ifndef PG_CHOICES_HEADLESS_H
#define PG_CHOICES_HEADLESS_H

/*
 * A headless stand-in for wxPropertyGrid's wxPGChoices (KICAD_HEADLESS_API).
 *
 * The property system uses wxPGChoices as the label/value list of an enumerated property:
 * ENUM_MAP<T> owns one, PROPERTY_ENUM copies and translates it, and the file-scope
 * registration constructors in board_item.cpp, zone.cpp, pad.cpp and friends fill it in.  All
 * of that runs headless -- it is how GetItems reports an enum field -- but wxPGChoices itself
 * lives in wxPropertyGrid, whose entries carry a wxPGCell holding a wxBitmapBundle, a wxColour
 * and a wxFont.  None of those exist in a wxBase-only build.
 *
 * This is the same class name and the same subset of the API, over std::vector and a shared
 * entry list, so no call site changes.  What is left out is everything that only a grid cell
 * would use: bitmaps, colours, fonts and sorting by display order.
 */

#include <algorithm>
#include <memory>
#include <vector>

#include <wx/arrstr.h>
#include <wx/string.h>

#ifndef wxPG_INVALID_VALUE
#define wxPG_INVALID_VALUE INT_MAX
#endif

#include <climits>


class wxPGChoiceEntry
{
public:
    wxPGChoiceEntry() : m_value( wxPG_INVALID_VALUE ) {}

    wxPGChoiceEntry( const wxString& aLabel, int aValue = wxPG_INVALID_VALUE ) :
            m_text( aLabel ), m_value( aValue )
    {}

    void            SetValue( int aValue ) { m_value = aValue; }
    int             GetValue() const { return m_value; }

    void            SetText( const wxString& aText ) { m_text = aText; }
    const wxString& GetText() const { return m_text; }

private:
    wxString m_text;
    int      m_value;
};


class wxPGChoicesData
{
public:
    unsigned int GetCount() const { return (unsigned int) m_items.size(); }

    const wxPGChoiceEntry& Item( unsigned int i ) const { return m_items[i]; }
    wxPGChoiceEntry&       Item( unsigned int i ) { return m_items[i]; }

    void Clear() { m_items.clear(); }

    void CopyDataFrom( const wxPGChoicesData* aOther )
    {
        if( aOther )
            m_items = aOther->m_items;
    }

    std::vector<wxPGChoiceEntry> m_items;
};


class wxPGChoices
{
public:
    typedef long ValArrItem;

    wxPGChoices() {}

    wxPGChoices( const wxArrayString& aLabels, const wxArrayInt& aValues = wxArrayInt() )
    {
        Add( aLabels, aValues );
    }

    /// Reference semantics, as wxPGChoices has: a copy shares the entry list.
    wxPGChoices( const wxPGChoices& aOther ) : m_data( aOther.m_data ) {}

    wxPGChoices& operator=( const wxPGChoices& aOther )
    {
        if( this != &aOther )
            m_data = aOther.m_data;

        return *this;
    }

    wxPGChoiceEntry& Add( const wxString& aLabel, int aValue = wxPG_INVALID_VALUE )
    {
        EnsureData();
        m_data->m_items.emplace_back( aLabel, aValue == wxPG_INVALID_VALUE
                                                      ? (int) m_data->m_items.size()
                                                      : aValue );
        return m_data->m_items.back();
    }

    void Add( const wxArrayString& aLabels, const wxArrayInt& aValues = wxArrayInt() )
    {
        for( size_t ii = 0; ii < aLabels.GetCount(); ++ii )
        {
            Add( aLabels[ii],
                 ii < aValues.GetCount() ? aValues[ii] : (int) ii );
        }
    }

    wxPGChoiceEntry& AddAsSorted( const wxString& aLabel, int aValue = wxPG_INVALID_VALUE )
    {
        return Add( aLabel, aValue );
    }

    wxPGChoiceEntry& Insert( const wxString& aLabel, int aIndex,
                             int aValue = wxPG_INVALID_VALUE )
    {
        EnsureData();

        if( aIndex < 0 || (size_t) aIndex >= m_data->m_items.size() )
            return Add( aLabel, aValue );

        auto it = m_data->m_items.insert( m_data->m_items.begin() + aIndex,
                                          wxPGChoiceEntry( aLabel, aValue ) );
        return *it;
    }

    void Assign( const wxPGChoices& aOther ) { m_data = aOther.m_data; }

    void Clear()
    {
        if( m_data )
            m_data->Clear();
    }

    wxPGChoices Copy() const
    {
        wxPGChoices dst;
        dst.EnsureData();
        dst.m_data->CopyDataFrom( m_data.get() );
        return dst;
    }

    void EnsureData()
    {
        if( !m_data )
            m_data = std::make_shared<wxPGChoicesData>();
    }

    unsigned int GetCount() const { return m_data ? m_data->GetCount() : 0; }

    const wxString& GetLabel( unsigned int aIndex ) const { return Item( aIndex ).GetText(); }

    int GetValue( unsigned int aIndex ) const { return Item( aIndex ).GetValue(); }

    wxArrayString GetLabels() const
    {
        wxArrayString labels;

        for( unsigned int ii = 0; ii < GetCount(); ++ii )
            labels.Add( GetLabel( ii ) );

        return labels;
    }

    wxArrayInt GetValuesForStrings( const wxArrayString& aStrings ) const
    {
        wxArrayInt values;

        for( const wxString& str : aStrings )
        {
            int index = Index( str );
            values.Add( index >= 0 ? GetValue( index ) : wxPG_INVALID_VALUE );
        }

        return values;
    }

    wxArrayInt GetIndicesForStrings( const wxArrayString& aStrings,
                                     wxArrayString* aUnmatched = nullptr ) const
    {
        wxArrayInt indices;

        for( const wxString& str : aStrings )
        {
            int index = Index( str );

            if( index >= 0 )
                indices.Add( index );
            else if( aUnmatched )
                aUnmatched->Add( str );
        }

        return indices;
    }

    int Index( const wxString& aLabel ) const
    {
        for( unsigned int ii = 0; ii < GetCount(); ++ii )
        {
            if( Item( ii ).GetText() == aLabel )
                return (int) ii;
        }

        return -1;
    }

    int Index( int aValue ) const
    {
        for( unsigned int ii = 0; ii < GetCount(); ++ii )
        {
            if( Item( ii ).GetValue() == aValue )
                return (int) ii;
        }

        return -1;
    }

    bool IsOk() const { return m_data != nullptr; }

    const wxPGChoiceEntry& Item( unsigned int i ) const { return m_data->Item( i ); }
    wxPGChoiceEntry&       Item( unsigned int i ) { return m_data->Item( i ); }

    wxPGChoiceEntry& operator[]( unsigned int i ) { return Item( i ); }
    const wxPGChoiceEntry& operator[]( unsigned int i ) const { return Item( i ); }

    void RemoveAt( size_t aIndex, size_t aCount = 1 )
    {
        if( !m_data || aIndex >= m_data->m_items.size() )
            return;

        aCount = std::min( aCount, m_data->m_items.size() - aIndex );
        m_data->m_items.erase( m_data->m_items.begin() + aIndex,
                               m_data->m_items.begin() + aIndex + aCount );
    }

    void Set( const wxArrayString& aLabels, const wxArrayInt& aValues = wxArrayInt() )
    {
        m_data.reset();
        Add( aLabels, aValues );
    }

    /// wxPGChoices hands out an exclusive copy here; sharing is all this build needs.
    void AllocExclusive() { EnsureData(); }

    wxPGChoicesData* GetDataPtr() const { return m_data.get(); }

private:
    std::shared_ptr<wxPGChoicesData> m_data;
};

#endif // PG_CHOICES_HEADLESS_H
