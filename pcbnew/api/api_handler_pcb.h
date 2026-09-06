/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2023 Jon Evans <jon@craftyjon.com>
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

#ifndef KICAD_API_HANDLER_PCB_H
#define KICAD_API_HANDLER_PCB_H

#include <api/api_handler_board.h>
#include <api/pcb_context.h>
#include <undo_redo_container.h>
#include <api/board/board_jobs.pb.h>
#include <api/common/commands/cross_probe_commands.pb.h>
#include <api/common/commands/project_commands.pb.h>
#include <api/common/commands/variant_commands.pb.h>
#include <properties/property_mgr.h>

using namespace kiapi::board::jobs;


class JOB;
class PCB_EDIT_FRAME;
class PCB_TRACK;
class PROPERTY_BASE;


class API_HANDLER_PCB : public API_HANDLER_BOARD
{
public:
    API_HANDLER_PCB( PCB_EDIT_FRAME* aFrame );
    API_HANDLER_PCB( std::shared_ptr<PCB_CONTEXT> aContext, PCB_EDIT_FRAME* aFrame = nullptr );

    std::optional<DocumentSpecifier> Document() const override;

private:
    typedef std::map<std::string, PROPERTY_BASE*> PROTO_PROPERTY_MAP;

    HANDLER_RESULT<commands::GetOpenDocumentsResponse> handleGetOpenDocuments(
            const HANDLER_CONTEXT<commands::GetOpenDocuments>& aCtx );

    HANDLER_RESULT<Empty> handleSaveDocument( const HANDLER_CONTEXT<commands::SaveDocument>& aCtx );

    HANDLER_RESULT<Empty> handleSaveCopyOfDocument(
            const HANDLER_CONTEXT<commands::SaveCopyOfDocument>& aCtx );

    HANDLER_RESULT<Empty> handleRevertDocument(
            const HANDLER_CONTEXT<commands::RevertDocument>& aCtx );

    HANDLER_RESULT<commands::GetItemsResponse> handleGetItems(
            const HANDLER_CONTEXT<commands::GetItems>& aCtx );

    /**
     * Gather the board items of the given types, as GetItems serves them.
     * @param aTypesRequested receives the types to keep (dimension subtypes are expanded)
     * @return false if none of the types is served by the board editor
     */
    bool collectItems( const std::vector<KICAD_T>& aTypes, std::vector<BOARD_ITEM*>& aItems,
                       std::set<KICAD_T>& aTypesRequested ) const;

    std::map<KICAD_T, uint32_t> countItems( const DocumentSpecifier& aDocument ) override;

    HANDLER_RESULT<BoardEnabledLayersResponse> handleSetBoardEnabledLayers(
            const HANDLER_CONTEXT<SetBoardEnabledLayers>& aCtx );

    HANDLER_RESULT<BoardStackupResponse> handleUpdateBoardStackup(
            const HANDLER_CONTEXT<UpdateBoardStackup>& aCtx );

    HANDLER_RESULT<BoardDesignRulesResponse> handleGetBoardDesignRules(
            const HANDLER_CONTEXT<GetBoardDesignRules>& aCtx );

    HANDLER_RESULT<BoardDesignRulesResponse> handleSetBoardDesignRules(
            const HANDLER_CONTEXT<SetBoardDesignRules>& aCtx );

    HANDLER_RESULT<CustomRulesResponse> handleGetCustomDesignRules(
            const HANDLER_CONTEXT<GetCustomDesignRules>& aCtx );

    HANDLER_RESULT<CustomRulesResponse> handleSetCustomDesignRules(
            const HANDLER_CONTEXT<SetCustomDesignRules>& aCtx );

    HANDLER_RESULT<common::types::EmbeddedFiles> handleGetEmbeddedFiles(
            const HANDLER_CONTEXT<GetEmbeddedFiles>& aCtx );

    HANDLER_RESULT<google::protobuf::Empty> handleAddEmbeddedFiles(
            const HANDLER_CONTEXT<AddEmbeddedFiles>& aCtx );

    HANDLER_RESULT<google::protobuf::Empty> handleSetEmbeddedFiles(
            const HANDLER_CONTEXT<SetEmbeddedFiles>& aCtx );

    HANDLER_RESULT<types::Vector2> handleGetBoardOrigin(
            const HANDLER_CONTEXT<GetBoardOrigin>& aCtx );

    HANDLER_RESULT<Empty> handleSetBoardOrigin( const HANDLER_CONTEXT<SetBoardOrigin>& aCtx );

    HANDLER_RESULT<BoardLayerNameResponse> handleGetBoardLayerName(
            const HANDLER_CONTEXT<GetBoardLayerName>& aCtx );

    HANDLER_RESULT<BoardLayerResponse> handleGetBoardLayerByName(
            const HANDLER_CONTEXT<GetBoardLayerByName>& aCtx );

    HANDLER_RESULT<NetsResponse> handleGetNets( const HANDLER_CONTEXT<GetNets>& aCtx );

    HANDLER_RESULT<commands::GetItemsResponse> handleGetConnectedItems(
            const HANDLER_CONTEXT<GetConnectedItems>& aCtx );

    HANDLER_RESULT<commands::GetItemsResponse> handleGetItemsByNet(
            const HANDLER_CONTEXT<GetItemsByNet>& aCtx );

    HANDLER_RESULT<commands::GetItemsResponse> handleGetItemsByNetClass(
            const HANDLER_CONTEXT<GetItemsByNetClass>& aCtx );

    HANDLER_RESULT<NetClassForNetsResponse> handleGetNetClassForNets(
            const HANDLER_CONTEXT<GetNetClassForNets>& aCtx );

    HANDLER_RESULT<Empty> handleRefillZones( const HANDLER_CONTEXT<RefillZones>& aCtx );

    HANDLER_RESULT<ImportNetlistResponse> handleImportNetlist(
            const HANDLER_CONTEXT<ImportNetlist>& aCtx );

    HANDLER_RESULT<BoardEditorAppearanceSettings> handleGetBoardEditorAppearanceSettings(
            const HANDLER_CONTEXT<GetBoardEditorAppearanceSettings>& aCtx );

    HANDLER_RESULT<Empty> handleSetBoardEditorAppearanceSettings(
            const HANDLER_CONTEXT<SetBoardEditorAppearanceSettings>& aCtx );

    HANDLER_RESULT<BoardPlotSettingsResponse>
    handleGetBoardPlotSettings( const HANDLER_CONTEXT<GetBoardPlotSettings>& aCtx );

    HANDLER_RESULT<Empty> handleSetBoardPlotSettings( const HANDLER_CONTEXT<SetBoardPlotSettings>& aCtx );

    HANDLER_RESULT<InjectDrcErrorResponse> handleInjectDrcError(
            const HANDLER_CONTEXT<InjectDrcError>& aCtx );

    HANDLER_RESULT<DrcResultsResponse> handleRunBoardJobDrc( const HANDLER_CONTEXT<RunBoardJobDrc>& aCtx );

    HANDLER_RESULT<DrcResultsResponse> handleGetDrcMarkers( const HANDLER_CONTEXT<GetDrcMarkers>& aCtx );

    HANDLER_RESULT<Empty> handleSetDrcMarkerExcluded( const HANDLER_CONTEXT<SetDrcMarkerExcluded>& aCtx );

    HANDLER_RESULT<DrcSeveritiesResponse> handleGetDrcSeverities( const HANDLER_CONTEXT<GetDrcSeverities>& aCtx );

    HANDLER_RESULT<DrcSeveritiesResponse> handleSetDrcSeverities( const HANDLER_CONTEXT<SetDrcSeverities>& aCtx );

    /// Fill a DrcResultsResponse from the markers currently on the board
    void collectDrcMarkers( DrcResultsResponse& aResponse ) const;

    DrcSeveritiesResponse drcSeverities() const;

    /**
     * Run an export job through the API job registry: synchronously, or (headless, when
     * aSettings.async is set) on the job worker thread.  Since 11.0.
     */
    HANDLER_RESULT<types::RunJobResponse> runBoardJob( const types::RunJobSettings& aSettings,
                                                       std::unique_ptr<JOB> aJob );

    HANDLER_RESULT<types::RunJobResponse> handleRunBoardJobExport3D(
            const HANDLER_CONTEXT<RunBoardJobExport3D>& aCtx );

    HANDLER_RESULT<commands::VariantsResponse> handleGetVariants( const HANDLER_CONTEXT<commands::GetVariants>& aCtx );
    HANDLER_RESULT<Empty> handleAddVariant( const HANDLER_CONTEXT<commands::AddVariant>& aCtx );
    HANDLER_RESULT<Empty> handleDeleteVariant( const HANDLER_CONTEXT<commands::DeleteVariant>& aCtx );
    HANDLER_RESULT<Empty> handleRenameVariant( const HANDLER_CONTEXT<commands::RenameVariant>& aCtx );
    HANDLER_RESULT<Empty> handleCopyVariant( const HANDLER_CONTEXT<commands::CopyVariant>& aCtx );
    HANDLER_RESULT<Empty> handleSetVariantDescription( const HANDLER_CONTEXT<commands::SetVariantDescription>& aCtx );
    HANDLER_RESULT<Empty> handleSetCurrentVariant( const HANDLER_CONTEXT<commands::SetCurrentVariant>& aCtx );
    HANDLER_RESULT<commands::CurrentVariantResponse>
    handleGetCurrentVariant( const HANDLER_CONTEXT<commands::GetCurrentVariant>& aCtx );

    HANDLER_RESULT<types::RunJobResponse> handleRunBoardJobExportRender(
            const HANDLER_CONTEXT<RunBoardJobExportRender>& aCtx );

    HANDLER_RESULT<types::RunJobResponse> handleRunBoardJobExportSvg(
            const HANDLER_CONTEXT<RunBoardJobExportSvg>& aCtx );

    HANDLER_RESULT<types::RunJobResponse> handleRunBoardJobExportDxf(
            const HANDLER_CONTEXT<RunBoardJobExportDxf>& aCtx );

    HANDLER_RESULT<types::RunJobResponse> handleRunBoardJobExportPdf(
            const HANDLER_CONTEXT<RunBoardJobExportPdf>& aCtx );

    HANDLER_RESULT<types::RunJobResponse> handleRunBoardJobExportPs(
            const HANDLER_CONTEXT<RunBoardJobExportPs>& aCtx );

    HANDLER_RESULT<types::RunJobResponse> handleRunBoardJobExportGerbers(
            const HANDLER_CONTEXT<RunBoardJobExportGerbers>& aCtx );

    HANDLER_RESULT<types::RunJobResponse> handleRunBoardJobExportDrill(
            const HANDLER_CONTEXT<RunBoardJobExportDrill>& aCtx );

    HANDLER_RESULT<types::RunJobResponse> handleRunBoardJobExportPosition(
            const HANDLER_CONTEXT<RunBoardJobExportPosition>& aCtx );

    HANDLER_RESULT<types::RunJobResponse> handleRunBoardJobExportGencad(
            const HANDLER_CONTEXT<RunBoardJobExportGencad>& aCtx );

    HANDLER_RESULT<types::RunJobResponse> handleRunBoardJobExportIpc2581(
            const HANDLER_CONTEXT<RunBoardJobExportIpc2581>& aCtx );

    HANDLER_RESULT<types::RunJobResponse> handleRunBoardJobExportIpcD356(
            const HANDLER_CONTEXT<RunBoardJobExportIpcD356>& aCtx );

    HANDLER_RESULT<types::RunJobResponse> handleRunBoardJobExportODB(
            const HANDLER_CONTEXT<RunBoardJobExportODB>& aCtx );

    HANDLER_RESULT<types::RunJobResponse> handleRunBoardJobExportStats(
            const HANDLER_CONTEXT<RunBoardJobExportStats>& aCtx );

    HANDLER_RESULT<commands::CrossProbeAnnounceResponse> handleCrossProbeAnnounce(
            const HANDLER_CONTEXT<commands::CrossProbeAnnounce>& aCtx );

    HANDLER_RESULT<commands::SyncSelectionResponse> handleSyncSelection(
            const HANDLER_CONTEXT<commands::SyncSelection>& aCtx );

    HANDLER_RESULT<commands::HighlightNetsResponse> handleHighlightNets(
            const HANDLER_CONTEXT<commands::HighlightNets>& aCtx );

    // Since 11.0
    HANDLER_RESULT<RatsnestResponse> handleGetRatsnest( const HANDLER_CONTEXT<GetRatsnest>& aCtx );

    HANDLER_RESULT<UnroutedCountResponse> handleGetUnroutedCount( const HANDLER_CONTEXT<GetUnroutedCount>& aCtx );

    HANDLER_RESULT<NetLengthsResponse> handleGetNetLengths( const HANDLER_CONTEXT<GetNetLengths>& aCtx );

    HANDLER_RESULT<UpdateFootprintsFromLibraryResponse>
    handleUpdateFootprintsFromLibrary( const HANDLER_CONTEXT<UpdateFootprintsFromLibrary>& aCtx );

    HANDLER_RESULT<SetTeardropsResponse> handleSetTeardrops( const HANDLER_CONTEXT<SetTeardrops>& aCtx );

    HANDLER_RESULT<SetTeardropsResponse> handleRemoveTeardrops( const HANDLER_CONTEXT<RemoveTeardrops>& aCtx );

    HANDLER_RESULT<AutoplaceFootprintsResponse>
    handleAutoplaceFootprints( const HANDLER_CONTEXT<AutoplaceFootprints>& aCtx );

    HANDLER_RESULT<GlobalDeletionResponse> handleGlobalDeletion( const HANDLER_CONTEXT<GlobalDeletion>& aCtx );

    HANDLER_RESULT<GraphicsDefaultsResponse>
    handleGetGraphicsDefaults( const HANDLER_CONTEXT<GetGraphicsDefaults>& aCtx );

    HANDLER_RESULT<GraphicsDefaultsResponse>
    handleSetGraphicsDefaults( const HANDLER_CONTEXT<SetGraphicsDefaults>& aCtx );

    void packGraphicsDefaults( board::GraphicsDefaults& aOut ) const;

    /**
     * Resolve the nets named in a request to net codes.
     * @return an error status if a net is unknown; an empty set if the request named none
     */
    HANDLER_RESULT<std::set<int>> resolveNets( const google::protobuf::RepeatedPtrField<board::types::Net>& aNets );

    /// Record a headless origin change on the undo stack, as the origin tools do in the editor
    void recordOriginUndo( const std::string& aClientName, UNDO_REDO aType, const VECTOR2I& aFrom,
                           const VECTOR2I& aTo );

protected:
    kiapi::common::types::DocumentType thisDocumentType() const override
    {
        return kiapi::common::types::DOCTYPE_PCB;
    }

    tl::expected<bool, ApiResponseStatus> validateDocumentInternal( const DocumentSpecifier& aDocument ) const override;

    std::optional<TITLE_BLOCK*> getTitleBlock( const DocumentSpecifier& aDocument ) override;

    std::optional<PAGE_INFO> getPageSettings( const DocumentSpecifier& aDocument ) override;

    bool setPageSettings( const DocumentSpecifier& aDocument, const PAGE_INFO& aPageInfo ) override;

    wxString getDrawingSheetFileName() override;

    void setDrawingSheetFileName( const wxString& aFileName ) override;

    void onModified() override;

private:
    PCB_CONTEXT* pcbContext() const { return static_cast<PCB_CONTEXT*>( context() ); }

    PCB_EDIT_FRAME* frame() const;
};

#endif //KICAD_API_HANDLER_PCB_H
