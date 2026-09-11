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
 * The handful of globals the wasm link is missing that deserve a REAL value rather than the
 * zeroed placeholder tools/wasm/gen_wasm_link_stubs.py would give them.  They are all C linkage
 * or plain data, which is why the generator (which keys off C++ mangling) does not see them.
 *
 * Each one is a definition that lives in a translation unit KICAD_HEADLESS_API does not build,
 * but that a unit it DOES build still reads -- not merely names.  A zeroed placeholder would be
 * a null dereference the first time, which is worse than the abort a function stub gives.
 */

#include <wx/event.h>
#include <wx/gdicmn.h>

#include <ui_events.h>

/*
 * common/database/database_connection.cpp and common/http_lib/http_lib_connection.cpp are not
 * built (no nanodbc, no libcurl), but eeschema's sch_io_database.cpp and sch_io_http_lib.cpp
 * are, and every wxLogTrace() call in them passes one of these masks.  wxLogTrace builds a
 * wxString from the pointer before it decides whether tracing is on, so a null here would crash
 * on the first call rather than on the first *traced* call.
 *
 * The `extern` is load-bearing: a `const` at namespace scope has INTERNAL linkage in C++, so
 * without it these compile to nothing and the link still fails.  The real definitions get away
 * with omitting it only because their own headers declare them extern first, and those headers
 * drag in nanodbc / libcurl.
 */
extern const char* const traceDatabase;
extern const char* const traceHTTPLib;

const char* const traceDatabase = "KICAD_DATABASE";
const char* const traceHTTPLib = "KICAD_HTTP_LIB";

/*
 * common/ui_events.cpp is in COMMON_GUI_SRCS, but common/kiway.cpp posts this event when the
 * language changes.  wxDEFINE_EVENT only allocates an id; nothing GUI is involved.
 */
wxDEFINE_EVENT( EDA_LANG_CHANGED, wxCommandEvent );

/*
 * wxCore's two most-referenced constants.  They are default arguments on a great many wx
 * signatures, so they are read (copied into a wxPoint/wxSize) even on calls that never reach a
 * toolkit.  wx spells them (-1, -1), and a zeroed placeholder would silently mean "position 0".
 */
const wxPoint wxDefaultPosition( -1, -1 );
const wxSize wxDefaultSize( -1, -1 );
