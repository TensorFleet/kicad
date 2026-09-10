#
#  This program source code file is part of KICAD, a free EDA CAD application.
#
#  Copyright (C) 2007-2020 Kicad Developers, see AUTHORS.txt for contributors.
#
#  This program is free software; you can redistribute it and/or
#  modify it under the terms of the GNU General Public License
#  as published by the Free Software Foundation; either version 2
#  of the License, or (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program; if not, you may find one here:
#  http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
#  or you may search the http://www.gnu.org website for the version 2 license,
#  or you may write to the Free Software Foundation, Inc.,
#  51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
#

# Disable shared library building for all thirdparty libraries
set( BUILD_SHARED_LIBS OFF )

# If this is not set, then we cannot set things like ARGPARSE_INSTALL to OFF before the
# argparse subdirectory is added.  Under the new policy, add_option will allow itself to
# be overridden by a previously-set variable like we want to do here.  Doing it this way
# instead of with cmake_policy will apply the change to the third-party CMakeLists files.
set( CMAKE_POLICY_DEFAULT_CMP0077 NEW )

# Avoid spurious warnings on g++
if(CMAKE_C_COMPILER_ID MATCHES "GNU")
    set_source_files_properties(
        ${CMAKE_SOURCE_DIR}/thirdparty/3dxware_sdk/src/navlib_stub.c
        PROPERTIES COMPILE_FLAGS "-Wno-attributes"
    )
endif()

set( ARGPARSE_INSTALL OFF )
add_subdirectory( argparse )
add_subdirectory( clipper2 )
add_subdirectory( compoundfilereader )
add_subdirectory( delaunator )
add_subdirectory( dxflib_qcad )
add_subdirectory( dynamic_bitset )
add_subdirectory( eigen )
add_subdirectory( expected )
set( FASTFLOAT_INSTALL OFF )
add_subdirectory( fast_float )
set( FMT_INSTALL OFF )
add_subdirectory( fmt )
add_subdirectory( glad )
add_subdirectory( gzip-hpp )
add_subdirectory( lemon )
# libcontext is the coroutine stack switcher (hand-written assembly).  The headless
# build uses the run-to-completion COROUTINE stand-in instead; see coroutine_sync.h.
if( NOT KICAD_HEADLESS_API )
    add_subdirectory( libcontext )
endif()
# libemf2svg links Fontconfig and libwmf is only reached from eeschema's OLE image
# helpers; both are dropped under the headless option along with sch_io/ole_*.cpp.
if( NOT KICAD_HEADLESS_API )
    add_subdirectory( libemf2svg )
endif()
add_subdirectory( libpopcnt )
if( NOT KICAD_HEADLESS_API )
    add_subdirectory( libwmf )
endif()
add_subdirectory( magic_enum )
add_subdirectory( markdown2html )
add_subdirectory( nanoflann )
# nanodbc is only reached from common/database/database_connection.cpp, which the
# headless build drops.
if( NOT KICAD_HEADLESS_API )
    add_subdirectory( nanodbc )
endif()
add_subdirectory( nanosvg )
add_subdirectory( rectpack2d )
add_subdirectory( rtree )
add_subdirectory( tinyspline_lib )
add_subdirectory( potrace )
add_subdirectory( planegcs )
add_subdirectory( nlohmann_json )
add_subdirectory( picosha2 )
add_subdirectory( json_schema_validator )
add_subdirectory( pegtl )
add_subdirectory( 3dxware_sdk )
add_subdirectory( rapidcsv )
add_subdirectory( turtle )
# The headless core runs every task on the calling thread; compat/inline-thread-pool
# provides a header-compatible facade in place of the real BS::thread_pool.  Selected
# by pointing the same INTERFACE target at a different include directory, so no call
# site changes.
add_subdirectory( thread-pool )

if( KICAD_HEADLESS_API )
    set_target_properties( thread-pool PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/compat/inline-thread-pool
        INTERFACE_SOURCES ${CMAKE_SOURCE_DIR}/compat/inline-thread-pool/bs_thread_pool.hpp
        )
endif()
add_subdirectory( zint )
