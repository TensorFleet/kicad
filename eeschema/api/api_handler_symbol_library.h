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

#ifndef KICAD_API_HANDLER_SYMBOL_LIBRARY_H
#define KICAD_API_HANDLER_SYMBOL_LIBRARY_H

#include <api/api_handler_library.h>

class PROJECT;
class SYMBOL_LIBRARY_ADAPTER;

/**
 * Serves the symbol library commands (LT_SYMBOL) for a project.  Registered by the eeschema
 * kiface while a project is open through the API.
 *
 * Since 11.0
 */
class API_HANDLER_SYMBOL_LIBRARY : public API_HANDLER_LIBRARY
{
public:
    API_HANDLER_SYMBOL_LIBRARY( PROJECT* aProject );

    ~API_HANDLER_SYMBOL_LIBRARY() override {}

protected:
    HANDLER_RESULT<kiapi::common::commands::ListLibraryEntriesResponse> handleListLibraryEntries(
            const HANDLER_CONTEXT<kiapi::common::commands::ListLibraryEntries>& aCtx ) override;

    HANDLER_RESULT<kiapi::common::commands::GetLibraryItemResponse> handleGetLibraryItem(
            const HANDLER_CONTEXT<kiapi::common::commands::GetLibraryItem>& aCtx ) override;

    HANDLER_RESULT<kiapi::common::commands::SaveLibraryItemResponse> handleSaveLibraryItem(
            const HANDLER_CONTEXT<kiapi::common::commands::SaveLibraryItem>& aCtx ) override;

    HANDLER_RESULT<google::protobuf::Empty> handleDeleteLibraryItem(
            const HANDLER_CONTEXT<kiapi::common::commands::DeleteLibraryItem>& aCtx ) override;

    wxString defaultLibraryExtension() const override;

private:
    /// @return an error status if aNickname is not a library in the tables, after loading it
    std::optional<ApiResponseStatus> ensureLibraryLoaded( const wxString& aNickname );

    SYMBOL_LIBRARY_ADAPTER* adapter() const;
};

#endif // KICAD_API_HANDLER_SYMBOL_LIBRARY_H
