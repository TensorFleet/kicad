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

#ifndef KICAD_API_HANDLER_FOOTPRINT_LIBRARY_H
#define KICAD_API_HANDLER_FOOTPRINT_LIBRARY_H

#include <memory>

#include <api/api_handler_library.h>
#include <api/board/board_types.pb.h>
#include <api/common/types/wizards.pb.h>

class FOOTPRINT;
class FOOTPRINT_LIBRARY_ADAPTER;
class FOOTPRINT_WIZARD_MANAGER;
class PROJECT;

/**
 * Serves the footprint library commands (LT_FOOTPRINT) and the footprint wizards for a project.
 * Registered by the pcbnew kiface while a project is open through the API.
 *
 * Since 11.0
 */
class API_HANDLER_FOOTPRINT_LIBRARY : public API_HANDLER_LIBRARY
{
public:
    API_HANDLER_FOOTPRINT_LIBRARY( PROJECT* aProject );

    ~API_HANDLER_FOOTPRINT_LIBRARY() override;

    /**
     * Pack the library view of a footprint: FOOTPRINT::Serialize produces a FootprintInstance,
     * whose definition is completed here with the attributes, fields and overrides that the
     * instance carries.
     */
    static void PackLibraryFootprint( kiapi::board::types::Footprint& aOut, const FOOTPRINT& aFootprint );

    /**
     * Build a FOOTPRINT from its library view (the inverse of PackLibraryFootprint).
     * @return nullptr if the message could not be unpacked
     */
    static std::unique_ptr<FOOTPRINT> UnpackLibraryFootprint( const kiapi::board::types::Footprint& aIn );

protected:
    HANDLER_RESULT<kiapi::common::commands::ListLibraryEntriesResponse> handleListLibraryEntries(
            const HANDLER_CONTEXT<kiapi::common::commands::ListLibraryEntries>& aCtx ) override;

    HANDLER_RESULT<kiapi::common::commands::GetLibraryItemResponse> handleGetLibraryItem(
            const HANDLER_CONTEXT<kiapi::common::commands::GetLibraryItem>& aCtx ) override;

    HANDLER_RESULT<kiapi::common::commands::SaveLibraryItemResponse> handleSaveLibraryItem(
            const HANDLER_CONTEXT<kiapi::common::commands::SaveLibraryItem>& aCtx ) override;

    HANDLER_RESULT<google::protobuf::Empty> handleDeleteLibraryItem(
            const HANDLER_CONTEXT<kiapi::common::commands::DeleteLibraryItem>& aCtx ) override;

    HANDLER_RESULT<kiapi::common::commands::ListWizardsResponse> handleListWizards(
            const HANDLER_CONTEXT<kiapi::common::commands::ListWizards>& aCtx );

    HANDLER_RESULT<kiapi::common::types::WizardGeneratedContent> handleRunWizard(
            const HANDLER_CONTEXT<kiapi::common::commands::RunWizard>& aCtx );

    wxString defaultLibraryExtension() const override;

private:
    /// @return an error status if aNickname is not a library in the tables, after loading it
    std::optional<ApiResponseStatus> ensureLibraryLoaded( const wxString& aNickname );

    FOOTPRINT_LIBRARY_ADAPTER* adapter() const;

    std::unique_ptr<FOOTPRINT_WIZARD_MANAGER> m_wizards;
};

#endif // KICAD_API_HANDLER_FOOTPRINT_LIBRARY_H
