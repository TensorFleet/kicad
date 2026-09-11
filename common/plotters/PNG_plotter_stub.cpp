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
 * Headless stand-in for PNG_PLOTTER (KICAD_HEADLESS_API).
 *
 * PNG_PLOTTER rasterizes through Cairo, which the headless API core does not link (there is
 * no Cairo in the Emscripten dependency set).  The class still has to exist, because
 * PLOT_FORMAT::PNG is a plain enumerator that plot_board_layers.cpp and sch_plotter.cpp
 * construct a plotter for.  Everything here is inert: OpenFile()/StartPlot() report that PNG
 * output is not available and fail, so a PNG plot job ends with an error instead of writing
 * an empty file or crashing, and every drawing primitive is a no-op.
 */

#include <plotters/plotter_png.h>

#include <geometry/shape_poly_set.h>
#include <wx/log.h>
#include <wx/translation.h>


PNG_PLOTTER::PNG_PLOTTER() :
        m_surface( nullptr ),
        m_context( nullptr ),
        m_dpi( DEFAULT_PNG_DPI ),
        m_width( 0 ),
        m_height( 0 ),
        m_antialias( false ),
        m_backgroundColor( COLOR4D( 0, 0, 0, 0 ) ),
        m_currentColor( COLOR4D::BLACK )
{
    // The base class destructor calls fclose() on m_outputFile; ours is unused.
    m_outputFile = nullptr;
}


PNG_PLOTTER::~PNG_PLOTTER()
{
}


bool PNG_PLOTTER::OpenFile( const wxString& aFullFilename )
{
    wxLogError( _( "PNG output is not available in this build." ) );
    return false;
}


bool PNG_PLOTTER::StartPlot( const wxString& aPageNumber )
{
    wxLogError( _( "PNG output is not available in this build." ) );
    return false;
}


bool PNG_PLOTTER::EndPlot()
{
    return false;
}


bool PNG_PLOTTER::SaveFile( const wxString& aPath )
{
    return false;
}


void PNG_PLOTTER::SetCurrentLineWidth( int aWidth, void* aData )
{
    m_currentPenWidth = aWidth;
}


void PNG_PLOTTER::SetColor( const COLOR4D& aColor )
{
    m_currentColor = aColor;
}


void PNG_PLOTTER::SetDash( int aLineWidth, LINE_STYLE aLineStyle )
{
}


void PNG_PLOTTER::SetClearCompositing( bool aClear )
{
}


void PNG_PLOTTER::SetViewport( const VECTOR2I& aOffset, double aIusPerDecimil, double aScale, bool aMirror )
{
    m_plotOffset = aOffset;
    m_plotScale = aScale;
    m_IUsPerDecimil = aIusPerDecimil;
    m_plotMirror = aMirror;
}


void PNG_PLOTTER::Rect( const VECTOR2I& p1, const VECTOR2I& p2, FILL_T aFill, int aWidth, int aCornerRadius )
{
}


void PNG_PLOTTER::Circle( const VECTOR2I& aCenter, int aDiameter, FILL_T aFill, int aWidth )
{
}


void PNG_PLOTTER::Arc( const VECTOR2D& aCenter, const EDA_ANGLE& aStartAngle, const EDA_ANGLE& aAngle,
                       double aRadius, FILL_T aFill, int aWidth )
{
}


void PNG_PLOTTER::PenTo( const VECTOR2I& aPos, char aPlume )
{
    m_penState = aPlume;
    m_penLastpos = aPos;
}


void PNG_PLOTTER::PlotPoly( const std::vector<VECTOR2I>& aCornerList, FILL_T aFill, int aWidth, void* aData )
{
}


void PNG_PLOTTER::PlotImage( const wxImage& aImage, const VECTOR2I& aPos, double aScaleFactor )
{
}


void PNG_PLOTTER::FlashPadCircle( const VECTOR2I& aPadPos, int aDiameter, void* aData )
{
}


void PNG_PLOTTER::FlashPadOval( const VECTOR2I& aPadPos, const VECTOR2I& aSize, const EDA_ANGLE& aPadOrient,
                                void* aData )
{
}


void PNG_PLOTTER::FlashPadRect( const VECTOR2I& aPadPos, const VECTOR2I& aSize, const EDA_ANGLE& aPadOrient,
                                void* aData )
{
}


void PNG_PLOTTER::FlashPadRoundRect( const VECTOR2I& aPadPos, const VECTOR2I& aSize, int aCornerRadius,
                                     const EDA_ANGLE& aOrient, void* aData )
{
}


void PNG_PLOTTER::FlashPadCustom( const VECTOR2I& aPadPos, const VECTOR2I& aSize, const EDA_ANGLE& aPadOrient,
                                  SHAPE_POLY_SET* aPolygons, void* aData )
{
}


void PNG_PLOTTER::FlashPadTrapez( const VECTOR2I& aPadPos, const VECTOR2I* aCorners,
                                  const EDA_ANGLE& aPadOrient, void* aData )
{
}


void PNG_PLOTTER::FlashRegularPolygon( const VECTOR2I& aShapePos, int aDiameter, int aCornerCount,
                                       const EDA_ANGLE& aOrient, void* aData )
{
}


VECTOR2D PNG_PLOTTER::userToDeviceCoordinates( const VECTOR2I& aCoordinate )
{
    return VECTOR2D( 0.0, 0.0 );
}


VECTOR2D PNG_PLOTTER::userToDeviceSize( const VECTOR2I& aSize )
{
    return VECTOR2D( 0.0, 0.0 );
}


double PNG_PLOTTER::userToDeviceSize( double aSize ) const
{
    return 0.0;
}
