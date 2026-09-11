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

#ifndef KICAD_HOST_STATIC_KIFACES_H
#define KICAD_HOST_STATIC_KIFACES_H

#include <kiway.h>

/**
 * The two KIFACE entry points that KICAD_HEADLESS_API renames per target (see
 * pcbnew/CMakeLists.txt and eeschema/CMakeLists.txt, which compile their object sets with
 * -DKIFACE_GETTER=KifaceGetterPcb / KifaceGetterSch).  They are extern "C" in the object
 * sets because kiway.h declares KIFACE_GETTER inside an extern "C" block.
 */
extern "C"
{
KIFACE* KifaceGetterPcb( int* aKIFACEversion, int aKIWAYversion, PGM_BASE* aProgram );
KIFACE* KifaceGetterSch( int* aKIFACEversion, int aKIWAYversion, PGM_BASE* aProgram );

/// Not a renamed module entry point: host/cvpcb_headless_kiface.cpp reimplements the two
/// functions ERC asks cvpcb for, because cvpcb/ is not built at all under the option.
KIFACE* KifaceGetterCvPcb( int* aKIFACEversion, int aKIWAYversion, PGM_BASE* aProgram );
}

#endif // KICAD_HOST_STATIC_KIFACES_H
