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
 * Headless stand-in for ole_image.cpp / ole_emf.cpp (KICAD_HEADLESS_API).
 *
 * Those two decode the OLE preview pictures an OrCAD schematic can embed, through libwmf
 * (WMF) and libemf2svg + nanosvgrast (EMF), painting the result into a wxImage.  None of
 * that is available headless: emf2svg links Fontconfig, and the rasterizers are pure GUI
 * value-type work.  The OrCAD reader itself is untouched -- the only thing missing is the
 * bitmap preview of an embedded OLE object, which every caller already treats as optional.
 */

#include <sch_io/ole_image.h>

#include <wx/buffer.h>
#include <wx/image.h>


OLE_IMAGE_PAYLOAD ExtractOleImage( const uint8_t* aCfb, size_t aSize )
{
    return OLE_IMAGE_PAYLOAD();
}


std::optional<std::pair<size_t, size_t>> OleEmbeddedCompoundFile( const std::vector<uint8_t>& aPayload )
{
    return std::nullopt;
}


OLE_IMAGE_PAYLOAD ExtractOleImageFromPayload( const std::vector<uint8_t>& aPayload )
{
    return OLE_IMAGE_PAYLOAD();
}


std::vector<uint8_t> OleExtractEmbeddedEmf( const std::vector<uint8_t>& aWmf )
{
    return {};
}


std::vector<uint8_t> OleExtractCiImage( const std::vector<uint8_t>& aPayload )
{
    return {};
}


wxString OleDescribeImagePayload( const std::vector<uint8_t>& aPayload )
{
    return wxString( wxT( "OLE image decoding is not available in this build" ) );
}


bool OleMakeBmpFromDib( const std::vector<uint8_t>& aDib, wxMemoryBuffer& aOut )
{
    return false;
}


bool OleRenderWmf( const std::vector<uint8_t>& aWmf, int aMaxWidth, int aMaxHeight, wxImage& aImage,
                   double aTargetAspect )
{
    return false;
}


bool OleRenderMetafilePreview( const std::vector<uint8_t>& aWmf, int aMaxWidth, int aMaxHeight,
                               wxImage& aImage, double aTargetAspect, bool* aUsedEmbeddedEmf )
{
    if( aUsedEmbeddedEmf )
        *aUsedEmbeddedEmf = false;

    return false;
}


bool OleRenderEmf( const std::vector<uint8_t>& aEmf, int aMaxWidth, int aMaxHeight, wxImage& aImage,
                   double aTargetAspect )
{
    return false;
}


VECTOR2I OleWmfRenderSize( int aNaturalWidth, int aNaturalHeight, int aMaxWidth, int aMaxHeight,
                           double aTargetAspect )
{
    return VECTOR2I( 0, 0 );
}
