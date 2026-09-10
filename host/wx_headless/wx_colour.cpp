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
 * A real, headless wxColour (KICAD_HEADLESS_WX_BASE_ONLY).
 *
 * wxColour lives in wxCore, and the platform build backs it with CoreGraphics.  It is also a
 * plain value type that the headless paths genuinely execute: COLOR4D converts to and from it,
 * the stackup and layer colour tables are file-scope arrays of it, and colour settings are
 * parsed through it.  An aborting stub is therefore not an option -- the process dies in a
 * static initializer before main().
 *
 * The class declaration comes from wx's own header, so nothing here changes the ABI; what this
 * file supplies is the out-of-line half: the virtuals (which is what makes the compiler emit
 * the vtable here), the reference data holding four channels, and the name/hex parsing and
 * formatting that wxColourBase leaves to the port.  There is no CGColor: GetCGColor() returns
 * null, which is only ever asked for by a drawing back end this build does not have.
 */

#include <wx/colour.h>
#include <wx/tokenzr.h>

#include <cstdio>
#include <cstdlib>


namespace
{

/*
 * Derived from wxGDIRefData rather than wxColourRefData on purpose: wxColourRefData is
 * declared in wx's header but its typeinfo lives in wxCore, whereas wxGDIRefData is inline
 * all the way down.  wxColour only ever sees this through wxGDIRefData*.
 */
class HEADLESS_COLOUR_REF_DATA : public wxGDIRefData
{
public:
    HEADLESS_COLOUR_REF_DATA( unsigned char r = 0, unsigned char g = 0, unsigned char b = 0,
                              unsigned char a = wxALPHA_OPAQUE ) :
            m_red( r ), m_green( g ), m_blue( b ), m_alpha( a )
    {}

    bool IsOk() const override { return true; }

    HEADLESS_COLOUR_REF_DATA* Clone() const
    {
        return new HEADLESS_COLOUR_REF_DATA( m_red, m_green, m_blue, m_alpha );
    }

    unsigned char m_red;
    unsigned char m_green;
    unsigned char m_blue;
    unsigned char m_alpha;
};


inline const HEADLESS_COLOUR_REF_DATA* data( const wxColour& aColour )
{
    return static_cast<const HEADLESS_COLOUR_REF_DATA*>( aColour.GetRefData() );
}

} // namespace


wxIMPLEMENT_DYNAMIC_CLASS( wxColour, wxColourBase );


wxColour::ChannelType wxColour::Red() const
{
    return m_refData ? (ChannelType) ( data( *this )->m_red ) : 0;
}


wxColour::ChannelType wxColour::Green() const
{
    return m_refData ? (ChannelType) ( data( *this )->m_green ) : 0;
}


wxColour::ChannelType wxColour::Blue() const
{
    return m_refData ? (ChannelType) ( data( *this )->m_blue ) : 0;
}


wxColour::ChannelType wxColour::Alpha() const
{
    return m_refData ? (ChannelType) ( data( *this )->m_alpha ) : wxALPHA_OPAQUE;
}


#if defined( __WXOSX__ )
bool wxColour::IsSolid() const
{
    return true;
}
#endif


bool wxColour::operator==( const wxColour& aOther ) const
{
    if( m_refData == aOther.m_refData )
        return true;

    if( !m_refData || !aOther.m_refData )
        return false;

    return Red() == aOther.Red() && Green() == aOther.Green() && Blue() == aOther.Blue()
           && Alpha() == aOther.Alpha();
}


void wxColour::InitRGBA( ChannelType r, ChannelType g, ChannelType b, ChannelType a )
{
    UnRef();
    m_refData = new HEADLESS_COLOUR_REF_DATA( r, g, b, a );
}


/*
 * The port half.  wxOSX declares the two wxGDIObject hooks and a CoreGraphics accessor on
 * wxColour itself; wxGTK declares a destructor, a GdkRGBA conversion and its own FromString
 * override instead, and inherits the (wxFAIL, unused) wxColourBase hooks.  Everything above
 * this comment is common: the reference data, the channels and the operators.
 */
#if defined( __WXOSX__ )

wxGDIRefData* wxColour::CreateGDIRefData() const
{
    return new HEADLESS_COLOUR_REF_DATA();
}


wxGDIRefData* wxColour::CloneGDIRefData( const wxGDIRefData* aData ) const
{
    return static_cast<const HEADLESS_COLOUR_REF_DATA*>( aData )->Clone();
}


CGColorRef wxColour::GetCGColor() const
{
    return nullptr;
}

#elif defined( __WXGTK__ )

wxColour::~wxColour()
{
}


// wxColour redeclares FromString under wxGTK, so the wxColourBase definition further down is
// not enough on its own; forward to it.
bool wxColour::FromString( const wxString& aStr )
{
    return wxColourBase::FromString( aStr );
}


// No GDK: nothing headless ever asks a colour for its toolkit representation, and every caller
// that would is a drawing back end this build does not link.
wxColour::operator const GdkRGBA*() const
{
    return nullptr;
}


const GdkColor* wxColour::GetColor() const
{
    return nullptr;
}

#else
#error "host/wx_headless/wx_colour.cpp: unknown wxWidgets port"
#endif


/*
 * wxColourBase's string half.  COLOR4D::SetFromWxString feeds this whatever a settings file
 * or a text variable holds, after SetFromHexString has already had a go at it; the formats
 * below are the ones KiCad writes and the CSS spellings it accepts.  Colour *names* resolve
 * through a short table rather than wxTheColourDatabase, which is a wxCore singleton.
 */

#include <wx/string.h>

#include <cstring>

namespace
{

struct NAMED_COLOUR
{
    const char*   name;
    unsigned char r;
    unsigned char g;
    unsigned char b;
};

const NAMED_COLOUR namedColours[] = {
    { "AQUAMARINE", 112, 219, 147 }, { "BLACK", 0, 0, 0 },        { "BLUE", 0, 0, 255 },
    { "BROWN", 165, 42, 42 },        { "CYAN", 0, 255, 255 },     { "DARK GREY", 47, 47, 47 },
    { "GREY", 128, 128, 128 },       { "GREEN", 0, 255, 0 },      { "LIGHT GREY", 192, 192, 192 },
    { "MAGENTA", 255, 0, 255 },      { "ORANGE", 255, 128, 0 },   { "PURPLE", 128, 0, 128 },
    { "RED", 255, 0, 0 },            { "VIOLET", 159, 95, 159 },  { "WHITE", 255, 255, 255 },
    { "YELLOW", 255, 255, 0 },
};


bool parseChannel( const wxString& aText, long& aOut )
{
    return aText.Strip( wxString::both ).ToLong( &aOut );
}

} // namespace


bool wxColourBase::FromString( const wxString& aStr )
{
    wxString str = aStr.Strip( wxString::both );

    if( str.IsEmpty() )
        return false;

    if( str.StartsWith( wxS( "#" ) ) )
    {
        unsigned long value = 0;

        if( !str.Mid( 1 ).ToULong( &value, 16 ) )
            return false;

        if( str.length() == 7 )
        {
            Set( (ChannelType) ( ( value >> 16 ) & 0xFF ), (ChannelType) ( ( value >> 8 ) & 0xFF ),
                 (ChannelType) ( value & 0xFF ) );
            return true;
        }
        else if( str.length() == 9 )
        {
            Set( (ChannelType) ( ( value >> 24 ) & 0xFF ), (ChannelType) ( ( value >> 16 ) & 0xFF ),
                 (ChannelType) ( ( value >> 8 ) & 0xFF ), (ChannelType) ( value & 0xFF ) );
            return true;
        }

        return false;
    }

    if( str.Lower().StartsWith( wxS( "rgb" ) ) )
    {
        size_t open = str.find( '(' );
        size_t close = str.find( ')' );

        if( open == wxString::npos || close == wxString::npos || close < open )
            return false;

        wxArrayString parts = wxSplit( str.Mid( open + 1, close - open - 1 ), ',' );

        if( parts.GetCount() < 3 )
            return false;

        long r = 0, g = 0, b = 0;

        if( !parseChannel( parts[0], r ) || !parseChannel( parts[1], g )
            || !parseChannel( parts[2], b ) )
        {
            return false;
        }

        double alpha = 1.0;

        if( parts.GetCount() > 3 && !parts[3].Strip( wxString::both ).ToDouble( &alpha ) )
            return false;

        Set( (ChannelType) r, (ChannelType) g, (ChannelType) b,
             (ChannelType) ( alpha * 255.0 + 0.5 ) );
        return true;
    }

    wxString upper = str.Upper();

    for( const NAMED_COLOUR& named : namedColours )
    {
        if( upper == wxString::FromAscii( named.name ) )
        {
            Set( named.r, named.g, named.b );
            return true;
        }
    }

    return false;
}


wxString wxColourBase::GetAsString( long aFlags ) const
{
    if( !IsOk() )
        return wxString();

    if( ( aFlags & wxC2S_CSS_SYNTAX ) && Alpha() != wxALPHA_OPAQUE )
    {
        return wxString::Format( wxS( "rgba(%d, %d, %d, %.3f)" ), (int) Red(), (int) Green(),
                                 (int) Blue(), Alpha() / 255.0 );
    }

    if( aFlags & wxC2S_CSS_SYNTAX )
        return wxString::Format( wxS( "rgb(%d, %d, %d)" ), (int) Red(), (int) Green(), (int) Blue() );

    return wxString::Format( wxS( "#%02X%02X%02X" ), (int) Red(), (int) Green(), (int) Blue() );
}
