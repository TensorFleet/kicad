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
 * A real, headless wxImage (KICAD_HEADLESS_WX_BASE_ONLY).
 *
 * BITMAP_BASE owns one for every reference image on a board or schematic, and constructs it
 * while the file is being parsed -- so, like wxColour, an aborting stub takes the process down
 * rather than degrading.  This is the pixel container only: an RGB plane, an optional alpha
 * plane, a mask colour and the option map, which is what the model, the plotters and the API
 * ask about (size, resolution options, pixel access).
 *
 * What is deliberately missing is the codecs.  wxImage's PNG/JPEG handlers are wxCore plus
 * libpng/libjpeg, none of which this build links, so LoadFile() and SaveFile() fail cleanly
 * and leave the image not-ok.  A board that carries a reference image still loads, and every
 * property except the decoded pixels is intact; what a caller cannot get is the raster.
 * Adding a decoder later means implementing the handler here -- zlib is already linked.
 *
 * The class declaration is wx's own, so there is no ABI of our own here; this file supplies
 * the out-of-line half, which is also what makes the compiler emit wxImage's vtable.
 */

#include <wx/image.h>
#include <wx/colour.h>   // wxALPHA_OPAQUE
#include <wx/log.h>
#include <wx/wfstream.h>

#include <cstring>

#include <map>
#include <vector>


namespace
{

class HEADLESS_IMAGE_REF_DATA : public wxObjectRefData
{
public:
    HEADLESS_IMAGE_REF_DATA() = default;

    HEADLESS_IMAGE_REF_DATA( const HEADLESS_IMAGE_REF_DATA& aOther ) :
            wxObjectRefData(),
            m_width( aOther.m_width ), m_height( aOther.m_height ), m_ok( aOther.m_ok ),
            m_data( aOther.m_data ), m_alpha( aOther.m_alpha ), m_hasMask( aOther.m_hasMask ),
            m_maskRed( aOther.m_maskRed ), m_maskGreen( aOther.m_maskGreen ),
            m_maskBlue( aOther.m_maskBlue ), m_type( aOther.m_type ),
            m_options( aOther.m_options )
    {}

    int  m_width = 0;
    int  m_height = 0;
    bool m_ok = false;

    std::vector<unsigned char> m_data;    ///< 3 bytes per pixel, empty when not ok
    std::vector<unsigned char> m_alpha;   ///< 1 byte per pixel, empty when there is none

    bool          m_hasMask = false;
    unsigned char m_maskRed = 0;
    unsigned char m_maskGreen = 0;
    unsigned char m_maskBlue = 0;

    wxBitmapType m_type = wxBITMAP_TYPE_INVALID;

    std::map<wxString, wxString> m_options;
};


inline HEADLESS_IMAGE_REF_DATA* imgData( const wxImage& aImage )
{
    return static_cast<HEADLESS_IMAGE_REF_DATA*>( aImage.GetRefData() );
}


int s_defaultLoadFlags = wxImage::Load_Verbose;

} // namespace


wxIMPLEMENT_DYNAMIC_CLASS( wxImage, wxObject );

wxList wxImage::sm_handlers;


wxObjectRefData* wxImage::CreateRefData() const
{
    return new HEADLESS_IMAGE_REF_DATA();
}


wxObjectRefData* wxImage::CloneRefData( const wxObjectRefData* aData ) const
{
    return new HEADLESS_IMAGE_REF_DATA( *static_cast<const HEADLESS_IMAGE_REF_DATA*>( aData ) );
}


bool wxImage::Create( int aWidth, int aHeight, bool aClear )
{
    UnRef();

    if( aWidth <= 0 || aHeight <= 0 )
        return false;

    HEADLESS_IMAGE_REF_DATA* d = new HEADLESS_IMAGE_REF_DATA();
    d->m_width = aWidth;
    d->m_height = aHeight;
    d->m_ok = true;
    d->m_data.assign( (size_t) aWidth * aHeight * 3, aClear ? 0 : 0 );

    m_refData = d;
    return true;
}


bool wxImage::IsOk() const
{
    return m_refData && imgData( *this )->m_ok;
}


int wxImage::GetWidth() const
{
    return m_refData ? imgData( *this )->m_width : 0;
}


int wxImage::GetHeight() const
{
    return m_refData ? imgData( *this )->m_height : 0;
}


wxBitmapType wxImage::GetType() const
{
    return m_refData ? imgData( *this )->m_type : wxBITMAP_TYPE_INVALID;
}


unsigned char* wxImage::GetData() const
{
    if( !m_refData )
        return nullptr;

    HEADLESS_IMAGE_REF_DATA* d = imgData( *this );
    return d->m_data.empty() ? nullptr : d->m_data.data();
}


unsigned char* wxImage::GetAlpha() const
{
    if( !m_refData )
        return nullptr;

    HEADLESS_IMAGE_REF_DATA* d = imgData( *this );
    return d->m_alpha.empty() ? nullptr : d->m_alpha.data();
}


long wxImage::XYToIndex( int x, int y ) const
{
    if( !IsOk() )
        return -1;

    HEADLESS_IMAGE_REF_DATA* d = imgData( *this );

    if( x < 0 || y < 0 || x >= d->m_width || y >= d->m_height )
        return -1;

    return (long) y * d->m_width + x;
}


void wxImage::SetRGB( int x, int y, unsigned char r, unsigned char g, unsigned char b )
{
    long index = XYToIndex( x, y );

    if( index < 0 )
        return;

    AllocExclusive();
    unsigned char* data = GetData() + index * 3;
    data[0] = r;
    data[1] = g;
    data[2] = b;
}


void wxImage::SetRGB( const wxRect& aRect, unsigned char r, unsigned char g, unsigned char b )
{
    if( !IsOk() )
        return;

    AllocExclusive();

    wxRect rect = aRect;

    if( rect == wxRect() )
        rect = wxRect( 0, 0, GetWidth(), GetHeight() );

    for( int y = rect.GetTop(); y <= rect.GetBottom(); ++y )
    {
        for( int x = rect.GetLeft(); x <= rect.GetRight(); ++x )
            SetRGB( x, y, r, g, b );
    }
}


unsigned char wxImage::GetRed( int x, int y ) const
{
    long index = XYToIndex( x, y );
    return index < 0 ? 0 : GetData()[index * 3];
}


unsigned char wxImage::GetGreen( int x, int y ) const
{
    long index = XYToIndex( x, y );
    return index < 0 ? 0 : GetData()[index * 3 + 1];
}


unsigned char wxImage::GetBlue( int x, int y ) const
{
    long index = XYToIndex( x, y );
    return index < 0 ? 0 : GetData()[index * 3 + 2];
}


void wxImage::InitAlpha()
{
    if( !IsOk() || HasAlpha() )
        return;

    AllocExclusive();
    HEADLESS_IMAGE_REF_DATA* d = imgData( *this );
    d->m_alpha.assign( (size_t) d->m_width * d->m_height, wxALPHA_OPAQUE );
}


void wxImage::SetAlpha( int x, int y, unsigned char aAlpha )
{
    long index = XYToIndex( x, y );

    if( index < 0 )
        return;

    if( !HasAlpha() )
        InitAlpha();

    AllocExclusive();
    imgData( *this )->m_alpha[index] = aAlpha;
}


unsigned char wxImage::GetAlpha( int x, int y ) const
{
    long index = XYToIndex( x, y );

    if( index < 0 || !HasAlpha() )
        return wxALPHA_OPAQUE;

    return imgData( *this )->m_alpha[index];
}


bool wxImage::HasMask() const
{
    return m_refData && imgData( *this )->m_hasMask;
}


unsigned char wxImage::GetMaskRed() const
{
    return m_refData ? imgData( *this )->m_maskRed : 0;
}


unsigned char wxImage::GetMaskGreen() const
{
    return m_refData ? imgData( *this )->m_maskGreen : 0;
}


unsigned char wxImage::GetMaskBlue() const
{
    return m_refData ? imgData( *this )->m_maskBlue : 0;
}


void wxImage::SetOption( const wxString& aName, const wxString& aValue )
{
    AllocExclusive();
    imgData( *this )->m_options[aName] = aValue;
}


void wxImage::SetOption( const wxString& aName, int aValue )
{
    SetOption( aName, wxString::Format( wxS( "%d" ), aValue ) );
}


wxString wxImage::GetOption( const wxString& aName ) const
{
    if( !m_refData )
        return wxString();

    HEADLESS_IMAGE_REF_DATA* d = imgData( *this );
    auto                     it = d->m_options.find( aName );

    return it == d->m_options.end() ? wxString() : it->second;
}


int wxImage::GetOptionInt( const wxString& aName ) const
{
    long value = 0;
    return GetOption( aName ).ToLong( &value ) ? (int) value : 0;
}


wxImage wxImage::Copy() const
{
    wxImage image;

    if( m_refData )
        image.m_refData = CloneRefData( m_refData );

    return image;
}


wxImage wxImage::Scale( int aWidth, int aHeight, wxImageResizeQuality ) const
{
    // Nearest-neighbour: the headless callers scale for a plot bounding box, never for
    // display, so the resampling quality is not observable.
    wxImage out;

    if( !IsOk() || aWidth <= 0 || aHeight <= 0 )
        return out;

    out.Create( aWidth, aHeight, false );

    if( HasAlpha() )
        out.InitAlpha();

    for( int y = 0; y < aHeight; ++y )
    {
        int srcY = (int) ( (double) y * GetHeight() / aHeight );

        for( int x = 0; x < aWidth; ++x )
        {
            int srcX = (int) ( (double) x * GetWidth() / aWidth );
            out.SetRGB( x, y, GetRed( srcX, srcY ), GetGreen( srcX, srcY ), GetBlue( srcX, srcY ) );

            if( HasAlpha() )
                out.SetAlpha( x, y, GetAlpha( srcX, srcY ) );
        }
    }

    return out;
}


wxImage wxImage::Rotate90( bool aClockwise ) const
{
    wxImage out;

    if( !IsOk() )
        return out;

    const int w = GetWidth();
    const int h = GetHeight();

    out.Create( h, w, false );

    if( HasAlpha() )
        out.InitAlpha();

    for( int y = 0; y < h; ++y )
    {
        for( int x = 0; x < w; ++x )
        {
            int dstX = aClockwise ? h - 1 - y : y;
            int dstY = aClockwise ? x : w - 1 - x;

            out.SetRGB( dstX, dstY, GetRed( x, y ), GetGreen( x, y ), GetBlue( x, y ) );

            if( HasAlpha() )
                out.SetAlpha( dstX, dstY, GetAlpha( x, y ) );
        }
    }

    return out;
}


wxImage wxImage::ConvertToGreyscale() const
{
    return ConvertToGreyscale( 0.299, 0.587, 0.114 );
}


wxImage wxImage::ConvertToGreyscale( double aWeightR, double aWeightG, double aWeightB ) const
{
    wxImage out = Copy();

    if( !out.IsOk() )
        return out;

    for( int y = 0; y < GetHeight(); ++y )
    {
        for( int x = 0; x < GetWidth(); ++x )
        {
            double grey = aWeightR * GetRed( x, y ) + aWeightG * GetGreen( x, y )
                          + aWeightB * GetBlue( x, y );
            unsigned char v = (unsigned char) ( grey > 255.0 ? 255.0 : grey );
            out.SetRGB( x, y, v, v, v );
        }
    }

    return out;
}


void wxImage::Paste( const wxImage& aImage, int aX, int aY, wxImageAlphaBlendMode )
{
    if( !IsOk() || !aImage.IsOk() )
        return;

    AllocExclusive();

    for( int y = 0; y < aImage.GetHeight(); ++y )
    {
        for( int x = 0; x < aImage.GetWidth(); ++x )
        {
            SetRGB( aX + x, aY + y, aImage.GetRed( x, y ), aImage.GetGreen( x, y ),
                    aImage.GetBlue( x, y ) );
        }
    }
}


void wxImage::SetDefaultLoadFlags( int aFlags )
{
    s_defaultLoadFlags = aFlags;
}


int wxImage::GetDefaultLoadFlags()
{
    return s_defaultLoadFlags;
}


/*
 * Loading: geometry only.
 *
 * wxImage's PNG/JPEG handlers are wxCore over libpng and libjpeg, neither of which this build
 * links, so there are no pixels to be had.  What every headless caller actually needs from a
 * reference image is its geometry -- BITMAP_BASE turns width, height and the stored resolution
 * into the item's size on the board, and keeps the undecoded bytes for saving -- so that is
 * what is read here, straight out of the container header.  The raster comes back blank.
 *
 * Formats: PNG (IHDR, plus pHYs for resolution), JPEG (the first SOFn frame header), BMP and
 * GIF.  Anything else fails, as does a truncated header.
 */

namespace
{

inline unsigned be32( const unsigned char* p )
{
    return ( (unsigned) p[0] << 24 ) | ( (unsigned) p[1] << 16 ) | ( (unsigned) p[2] << 8 ) | p[3];
}


inline unsigned be16( const unsigned char* p )
{
    return ( (unsigned) p[0] << 8 ) | p[1];
}


inline unsigned le32( const unsigned char* p )
{
    return ( (unsigned) p[3] << 24 ) | ( (unsigned) p[2] << 16 ) | ( (unsigned) p[1] << 8 ) | p[0];
}


struct IMAGE_GEOMETRY
{
    int          width = 0;
    int          height = 0;
    double       resX = 0.0;         ///< in the unit below
    double       resY = 0.0;
    int          resUnit = wxIMAGE_RESOLUTION_NONE;
    wxBitmapType type = wxBITMAP_TYPE_INVALID;
};


bool sniffPng( const std::vector<unsigned char>& aBytes, IMAGE_GEOMETRY& aOut )
{
    static const unsigned char signature[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };

    if( aBytes.size() < 33 || memcmp( aBytes.data(), signature, 8 ) != 0 )
        return false;

    if( memcmp( aBytes.data() + 12, "IHDR", 4 ) != 0 )
        return false;

    aOut.type = wxBITMAP_TYPE_PNG;
    aOut.width = (int) be32( aBytes.data() + 16 );
    aOut.height = (int) be32( aBytes.data() + 20 );

    // Walk the chunk list for pHYs.  Stop at IDAT: everything positional comes before it.
    size_t pos = 8;

    while( pos + 12 <= aBytes.size() )
    {
        unsigned          length = be32( aBytes.data() + pos );
        const char* const name = (const char*) aBytes.data() + pos + 4;

        if( memcmp( name, "IDAT", 4 ) == 0 || memcmp( name, "IEND", 4 ) == 0 )
            break;

        if( memcmp( name, "pHYs", 4 ) == 0 && length >= 9 && pos + 8 + 9 <= aBytes.size() )
        {
            const unsigned char* phys = aBytes.data() + pos + 8;

            // The unit byte is 1 for metres; wx reports px/cm in that case, as KiCad expects.
            if( phys[8] == 1 )
            {
                aOut.resX = be32( phys ) / 100.0;
                aOut.resY = be32( phys + 4 ) / 100.0;
                aOut.resUnit = wxIMAGE_RESOLUTION_CM;
            }
        }

        pos += 12 + (size_t) length;   // length + type + data + CRC
    }

    return aOut.width > 0 && aOut.height > 0;
}


bool sniffJpeg( const std::vector<unsigned char>& aBytes, IMAGE_GEOMETRY& aOut )
{
    if( aBytes.size() < 4 || aBytes[0] != 0xFF || aBytes[1] != 0xD8 )
        return false;

    aOut.type = wxBITMAP_TYPE_JPEG;
    size_t pos = 2;

    while( pos + 4 <= aBytes.size() )
    {
        if( aBytes[pos] != 0xFF )
        {
            ++pos;
            continue;
        }

        unsigned char marker = aBytes[pos + 1];

        if( marker == 0xD8 || marker == 0x01 || ( marker >= 0xD0 && marker <= 0xD7 ) )
        {
            pos += 2;
            continue;
        }

        unsigned length = be16( aBytes.data() + pos + 2 );

        // SOF0..SOF15, skipping the four that are not frame headers.
        if( marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8
            && marker != 0xCC )
        {
            if( pos + 9 > aBytes.size() )
                return false;

            aOut.height = (int) be16( aBytes.data() + pos + 5 );
            aOut.width = (int) be16( aBytes.data() + pos + 7 );
            break;
        }

        // JFIF APP0 carries the density, in dpi or in dots per cm.
        if( marker == 0xE0 && length >= 14 && pos + 4 + 14 <= aBytes.size()
            && memcmp( aBytes.data() + pos + 4, "JFIF", 5 ) == 0 )
        {
            const unsigned char* app0 = aBytes.data() + pos + 4;
            unsigned             units = app0[7];

            if( units == 1 || units == 2 )
            {
                aOut.resX = be16( app0 + 8 );
                aOut.resY = be16( app0 + 10 );
                aOut.resUnit = units == 1 ? wxIMAGE_RESOLUTION_INCHES : wxIMAGE_RESOLUTION_CM;
            }
        }

        pos += 2 + (size_t) length;
    }

    return aOut.width > 0 && aOut.height > 0;
}


bool sniffBmp( const std::vector<unsigned char>& aBytes, IMAGE_GEOMETRY& aOut )
{
    if( aBytes.size() < 30 || aBytes[0] != 'B' || aBytes[1] != 'M' )
        return false;

    aOut.type = wxBITMAP_TYPE_BMP;
    aOut.width = (int) le32( aBytes.data() + 18 );
    aOut.height = (int) le32( aBytes.data() + 22 );

    if( aOut.height < 0 )
        aOut.height = -aOut.height;

    return aOut.width > 0 && aOut.height > 0;
}


bool sniffGif( const std::vector<unsigned char>& aBytes, IMAGE_GEOMETRY& aOut )
{
    if( aBytes.size() < 10 || memcmp( aBytes.data(), "GIF8", 4 ) != 0 )
        return false;

    aOut.type = wxBITMAP_TYPE_GIF;
    aOut.width = aBytes[6] | ( aBytes[7] << 8 );
    aOut.height = aBytes[8] | ( aBytes[9] << 8 );

    return aOut.width > 0 && aOut.height > 0;
}


std::vector<unsigned char> readAll( wxInputStream& aStream )
{
    std::vector<unsigned char> bytes;
    unsigned char              chunk[16384];

    while( aStream.IsOk() )
    {
        aStream.Read( chunk, sizeof( chunk ) );
        size_t got = aStream.LastRead();

        if( got == 0 )
            break;

        bytes.insert( bytes.end(), chunk, chunk + got );
    }

    return bytes;
}


bool noCodec()
{
    wxLogTrace( wxS( "KICAD_IMAGE" ),
                wxS( "wxImage: this build writes no image files" ) );
    return false;
}

} // namespace


bool wxImage::LoadFile( wxInputStream& aStream, wxBitmapType, int )
{
    UnRef();

    std::vector<unsigned char> bytes = readAll( aStream );
    IMAGE_GEOMETRY             geom;

    if( !sniffPng( bytes, geom ) && !sniffJpeg( bytes, geom ) && !sniffBmp( bytes, geom )
        && !sniffGif( bytes, geom ) )
    {
        wxLogTrace( wxS( "KICAD_IMAGE" ),
                    wxS( "wxImage: unrecognised image header (%zu bytes)" ), bytes.size() );
        return false;
    }

    if( !Create( geom.width, geom.height, true ) )
        return false;

    HEADLESS_IMAGE_REF_DATA* d = imgData( *this );
    d->m_type = geom.type;

    if( geom.resUnit != wxIMAGE_RESOLUTION_NONE )
    {
        SetOption( wxIMAGE_OPTION_RESOLUTIONX, wxString::FromCDouble( geom.resX, 6 ) );
        SetOption( wxIMAGE_OPTION_RESOLUTIONY, wxString::FromCDouble( geom.resY, 6 ) );
        SetOption( wxIMAGE_OPTION_RESOLUTIONUNIT, geom.resUnit );
    }

    return true;
}


bool wxImage::LoadFile( wxInputStream& aStream, const wxString&, int aIndex )
{
    return LoadFile( aStream, wxBITMAP_TYPE_ANY, aIndex );
}


bool wxImage::LoadFile( const wxString& aName, wxBitmapType aType, int aIndex )
{
    wxFileInputStream stream( aName );

    if( !stream.IsOk() )
        return false;

    return LoadFile( stream, aType, aIndex );
}


bool wxImage::LoadFile( const wxString& aName, const wxString&, int aIndex )
{
    return LoadFile( aName, wxBITMAP_TYPE_ANY, aIndex );
}


bool wxImage::SaveFile( wxOutputStream&, wxBitmapType ) const     { return noCodec(); }
bool wxImage::SaveFile( wxOutputStream&, const wxString& ) const  { return noCodec(); }
bool wxImage::SaveFile( const wxString& ) const                   { return noCodec(); }
bool wxImage::SaveFile( const wxString&, wxBitmapType ) const     { return noCodec(); }
bool wxImage::SaveFile( const wxString&, const wxString& ) const  { return noCodec(); }
