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

#include <magic_enum.hpp>
#include <memory>
#include <properties/property.h>

#include <common.h>
#include <fmt.h>
#include <api/api_handler_pcb.h>
#include <api/api_job_registry.h>
#include <api/api_jobs.h>
#include <api/api_pcb_utils.h>
#include <api/api_enums.h>
#include <api/api_utils.h>
#include <api/cross_probe_client.h>
#include <wx/log.h>
#include <base_screen.h>
#include <board_commit.h>
#include <board_connected_item.h>
#include <board_design_settings.h>
#include <board_stackup_manager/board_stackup.h>
#include <core/kicad_algo.h>
#include <footprint.h>
#include <kicad_clipboard.h>
#include <netinfo.h>
#include <pad.h>
#include <pcb_draw_panel_gal.h>
#include <pcb_edit_frame.h>
#include <pcb_group.h>
#include <pcb_reference_image.h>
#include <pcb_shape.h>
#include <pcb_text.h>
#include <pcb_textbox.h>
#include <pcb_track.h>
#include <pcbnew_id.h>
#include <pcb_marker.h>
#include <pcb_point.h>
#include <kiway.h>
#include <drc/drc_engine.h>
#include <drc/drc_item.h>
#include <drawing_sheet/ds_proxy_view_item.h>
#include <footprint_library_adapter.h>
#include <kiface_ids.h>
#include <netlist_reader/netlist_reader.h>
#include <project_pcb.h>
#include <richio.h>
#include <jobs/job_export_pcb_3d.h>
#include <jobs/job_export_pcb_dxf.h>
#include <jobs/job_export_pcb_drill.h>
#include <jobs/job_export_pcb_gencad.h>
#include <jobs/job_export_pcb_gerber.h>
#include <jobs/job_export_pcb_gerbers.h>
#include <jobs/job_export_pcb_ipc2581.h>
#include <jobs/job_export_pcb_ipcd356.h>
#include <jobs/job_export_pcb_odb.h>
#include <jobs/job_export_pcb_pdf.h>
#include <jobs/job_export_pcb_pos.h>
#include <jobs/job_export_pcb_ps.h>
#include <jobs/job_export_pcb_stats.h>
#include <jobs/job_export_pcb_svg.h>
#include <pcb_plot_params.h>
#include <jobs/job_pcb_render.h>
#include <layer_ids.h>
#include <netlist_reader/board_netlist_updater.h>
#include <netlist_reader/pcb_netlist.h>
#include <project.h>
#include <tool/actions.h>
#include <string_utils.h>
#include <tool/tool_manager.h>
#include <tools/pcb_actions.h>
#include <tools/pcb_selection_tool.h>
#include <tools/zone_filler_tool.h>
#include <zone.h>
#include <zone_filler.h>

#include <api/common/types/base_types.pb.h>
#include <api/board/board_rules.pb.h>
#include <connectivity/connectivity_data.h>
#include <connectivity/connectivity_algo.h>
#include <autorouter/ar_autoplacer.h>
#include <length_delay_calculation/length_delay_calculation.h>
#include <netlist_reader/pcb_netlist_utils.h>
#include <pcb_generator.h>
#include <generators/pcb_tuning_pattern.h>
#include <ratsnest/ratsnest_data.h>
#include <api/api_undo_stack.h>
#include <origin_viewitem.h>
#include <undo_redo_container.h>
#include <teardrop/teardrop.h>
#include <teardrop/teardrop_parameters.h>
#include <google/protobuf/util/json_util.h>
#include <drc/drc_rule_condition.h>
#include <drc/drc_rule_parser.h>
#include <widgets/appearance_controls.h>
#include <widgets/report_severity.h>
#include <drc/rule_editor/drc_re_rule_loader.h>
#include <trace_helpers.h>
#include <wx/ffile.h>

using namespace kiapi::common::commands;
using types::CommandStatus;
using types::DocumentType;
using types::ItemRequestStatus;


API_HANDLER_PCB::API_HANDLER_PCB( PCB_EDIT_FRAME* aFrame ) :
        API_HANDLER_PCB( CreatePcbFrameContext( aFrame ), aFrame )
{
}


API_HANDLER_PCB::API_HANDLER_PCB( std::shared_ptr<PCB_CONTEXT> aContext, PCB_EDIT_FRAME* aFrame ) :
        API_HANDLER_BOARD( std::move( aContext ), aFrame )
{
    registerHandler<GetOpenDocuments, GetOpenDocumentsResponse>(
            &API_HANDLER_PCB::handleGetOpenDocuments );
    registerHandler<SaveDocument, Empty>( &API_HANDLER_PCB::handleSaveDocument );
    registerHandler<SaveCopyOfDocument, Empty>( &API_HANDLER_PCB::handleSaveCopyOfDocument );
    registerHandler<RevertDocument, Empty>( &API_HANDLER_PCB::handleRevertDocument, HANDLER_MODE::GUI_ONLY );

    registerHandler<GetItems, GetItemsResponse>( &API_HANDLER_PCB::handleGetItems );

    registerHandler<SetBoardEnabledLayers, BoardEnabledLayersResponse>( &API_HANDLER_PCB::handleSetBoardEnabledLayers );
    registerHandler<UpdateBoardStackup, BoardStackupResponse>( &API_HANDLER_PCB::handleUpdateBoardStackup );
    registerHandler<GetBoardDesignRules, BoardDesignRulesResponse>( &API_HANDLER_PCB::handleGetBoardDesignRules );
    registerHandler<SetBoardDesignRules, BoardDesignRulesResponse>( &API_HANDLER_PCB::handleSetBoardDesignRules );
    registerHandler<GetCustomDesignRules, CustomRulesResponse>( &API_HANDLER_PCB::handleGetCustomDesignRules );
    registerHandler<SetCustomDesignRules, CustomRulesResponse>( &API_HANDLER_PCB::handleSetCustomDesignRules );
    registerHandler<GetEmbeddedFiles, common::types::EmbeddedFiles>( &API_HANDLER_PCB::handleGetEmbeddedFiles );
    registerHandler<AddEmbeddedFiles, Empty>( &API_HANDLER_PCB::handleAddEmbeddedFiles );
    registerHandler<SetEmbeddedFiles, Empty>( &API_HANDLER_PCB::handleSetEmbeddedFiles );
    registerHandler<GetBoardOrigin, types::Vector2>( &API_HANDLER_PCB::handleGetBoardOrigin );
    registerHandler<SetBoardOrigin, Empty>( &API_HANDLER_PCB::handleSetBoardOrigin );
    registerHandler<GetBoardLayerName, BoardLayerNameResponse>( &API_HANDLER_PCB::handleGetBoardLayerName );
    registerHandler<GetBoardLayerByName, BoardLayerResponse>( &API_HANDLER_PCB::handleGetBoardLayerByName );

    registerHandler<GetNets, NetsResponse>( &API_HANDLER_PCB::handleGetNets );
    registerHandler<GetConnectedItems, GetItemsResponse>( &API_HANDLER_PCB::handleGetConnectedItems );
    registerHandler<GetItemsByNet, GetItemsResponse>( &API_HANDLER_PCB::handleGetItemsByNet );
    registerHandler<GetItemsByNetClass, GetItemsResponse>( &API_HANDLER_PCB::handleGetItemsByNetClass );
    registerHandler<GetNetClassForNets, NetClassForNetsResponse>(
            &API_HANDLER_PCB::handleGetNetClassForNets );
    registerHandler<RefillZones, Empty>( &API_HANDLER_PCB::handleRefillZones );
    registerHandler<ImportNetlist, ImportNetlistResponse>( &API_HANDLER_PCB::handleImportNetlist );

    registerHandler<GetBoardEditorAppearanceSettings, BoardEditorAppearanceSettings>(
            &API_HANDLER_PCB::handleGetBoardEditorAppearanceSettings, HANDLER_MODE::GUI_ONLY );
    registerHandler<SetBoardEditorAppearanceSettings, Empty>(
            &API_HANDLER_PCB::handleSetBoardEditorAppearanceSettings, HANDLER_MODE::GUI_ONLY );
    registerHandler<GetBoardPlotSettings, BoardPlotSettingsResponse>( &API_HANDLER_PCB::handleGetBoardPlotSettings );
    registerHandler<SetBoardPlotSettings, Empty>( &API_HANDLER_PCB::handleSetBoardPlotSettings );
    registerHandler<InjectDrcError, InjectDrcErrorResponse>(
            &API_HANDLER_PCB::handleInjectDrcError );
    registerHandler<RunBoardJobDrc, DrcResultsResponse>( &API_HANDLER_PCB::handleRunBoardJobDrc );
    registerHandler<GetDrcMarkers, DrcResultsResponse>( &API_HANDLER_PCB::handleGetDrcMarkers );
    registerHandler<SetDrcMarkerExcluded, Empty>( &API_HANDLER_PCB::handleSetDrcMarkerExcluded );
    registerHandler<GetDrcSeverities, DrcSeveritiesResponse>( &API_HANDLER_PCB::handleGetDrcSeverities );
    registerHandler<SetDrcSeverities, DrcSeveritiesResponse>( &API_HANDLER_PCB::handleSetDrcSeverities );

    registerHandler<GetVariants, VariantsResponse>( &API_HANDLER_PCB::handleGetVariants );
    registerHandler<AddVariant, Empty>( &API_HANDLER_PCB::handleAddVariant );
    registerHandler<DeleteVariant, Empty>( &API_HANDLER_PCB::handleDeleteVariant );
    registerHandler<RenameVariant, Empty>( &API_HANDLER_PCB::handleRenameVariant );
    registerHandler<CopyVariant, Empty>( &API_HANDLER_PCB::handleCopyVariant );
    registerHandler<SetVariantDescription, Empty>( &API_HANDLER_PCB::handleSetVariantDescription );
    registerHandler<SetCurrentVariant, Empty>( &API_HANDLER_PCB::handleSetCurrentVariant );
    registerHandler<GetCurrentVariant, CurrentVariantResponse>(
            &API_HANDLER_PCB::handleGetCurrentVariant );

    registerHandler<RunBoardJobExport3D, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExport3D );
    registerHandler<RunBoardJobExportRender, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExportRender );
    registerHandler<RunBoardJobExportSvg, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExportSvg );
    registerHandler<RunBoardJobExportDxf, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExportDxf );
    registerHandler<RunBoardJobExportPdf, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExportPdf );
    registerHandler<RunBoardJobExportPs, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExportPs );
    registerHandler<RunBoardJobExportGerbers, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExportGerbers );
    registerHandler<RunBoardJobExportDrill, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExportDrill );
    registerHandler<RunBoardJobExportPosition, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExportPosition );
    registerHandler<RunBoardJobExportGencad, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExportGencad );
    registerHandler<RunBoardJobExportIpc2581, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExportIpc2581 );
    registerHandler<RunBoardJobExportIpcD356, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExportIpcD356 );
    registerHandler<RunBoardJobExportODB, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExportODB );
    registerHandler<RunBoardJobExportStats, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExportStats );

    registerHandler<GetPageSettings, types::PageSettings>( &API_HANDLER_PCB::handleGetPageSettings );
    registerHandler<SetPageSettings, types::PageSettings>( &API_HANDLER_PCB::handleSetPageSettings );

    registerHandler<CrossProbeAnnounce, CrossProbeAnnounceResponse>( &API_HANDLER_PCB::handleCrossProbeAnnounce );
    registerHandler<SyncSelection, SyncSelectionResponse>(
            &API_HANDLER_PCB::handleSyncSelection, HANDLER_MODE::GUI_ONLY );
    // Since 11.0
    registerHandler<GetRatsnest, RatsnestResponse>( &API_HANDLER_PCB::handleGetRatsnest );
    registerHandler<GetUnroutedCount, UnroutedCountResponse>( &API_HANDLER_PCB::handleGetUnroutedCount );
    registerHandler<GetNetLengths, NetLengthsResponse>( &API_HANDLER_PCB::handleGetNetLengths );
    registerHandler<UpdateFootprintsFromLibrary, UpdateFootprintsFromLibraryResponse>(
            &API_HANDLER_PCB::handleUpdateFootprintsFromLibrary );
    registerHandler<SetTeardrops, SetTeardropsResponse>( &API_HANDLER_PCB::handleSetTeardrops );
    registerHandler<RemoveTeardrops, SetTeardropsResponse>( &API_HANDLER_PCB::handleRemoveTeardrops );
    registerHandler<AutoplaceFootprints, AutoplaceFootprintsResponse>( &API_HANDLER_PCB::handleAutoplaceFootprints );
    registerHandler<GlobalDeletion, GlobalDeletionResponse>( &API_HANDLER_PCB::handleGlobalDeletion );
    registerHandler<GetGraphicsDefaults, GraphicsDefaultsResponse>( &API_HANDLER_PCB::handleGetGraphicsDefaults );
    registerHandler<SetGraphicsDefaults, GraphicsDefaultsResponse>( &API_HANDLER_PCB::handleSetGraphicsDefaults );

    registerHandler<HighlightNets, HighlightNetsResponse>(
            &API_HANDLER_PCB::handleHighlightNets, HANDLER_MODE::GUI_ONLY );
}


PCB_EDIT_FRAME* API_HANDLER_PCB::frame() const
{
    return static_cast<PCB_EDIT_FRAME*>( m_frame );
}


HANDLER_RESULT<GetOpenDocumentsResponse> API_HANDLER_PCB::handleGetOpenDocuments(
        const HANDLER_CONTEXT<GetOpenDocuments>& aCtx )
{
    if( aCtx.Request.type() != DocumentType::DOCTYPE_PCB )
    {
        ApiResponseStatus e;
        // No message needed for AS_UNHANDLED; this is an internal flag for the API server
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    GetOpenDocumentsResponse response;
    response.mutable_documents()->Add( *Document() );
    return response;
}


std::optional<DocumentSpecifier> API_HANDLER_PCB::Document() const
{
    common::types::DocumentSpecifier doc;

    wxFileName fn( pcbContext()->GetCurrentFileName() );

    doc.set_type( DocumentType::DOCTYPE_PCB );
    doc.set_board_filename( fn.GetFullName() );

    doc.mutable_project()->set_name( project().GetProjectName().ToStdString() );
    doc.mutable_project()->set_path( project().GetProjectDirectory().ToStdString() );

    return doc;
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleSaveDocument(
        const HANDLER_CONTEXT<SaveDocument>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    pcbContext()->SaveBoard();
    notifyDocumentSaved( pcbContext()->GetCurrentFileName() );
    return Empty();
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleSaveCopyOfDocument(
        const HANDLER_CONTEXT<SaveCopyOfDocument>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    wxFileName boardPath( project().AbsolutePath( wxString::FromUTF8( aCtx.Request.path() ) ) );

    if( !boardPath.IsOk() || !boardPath.IsDirWritable() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "save path '{}' could not be opened",
                                          boardPath.GetFullPath().ToStdString() ) );
        return tl::unexpected( e );
    }

    if( boardPath.FileExists()
        && ( !boardPath.IsFileWritable() || !aCtx.Request.options().overwrite() ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "save path '{}' exists and cannot be overwritten",
                                          boardPath.GetFullPath().ToStdString() ) );
        return tl::unexpected( e );
    }

    if( boardPath.GetExt() != FILEEXT::KiCadPcbFileExtension )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "save path '{}' must have a kicad_pcb extension",
                                          boardPath.GetFullPath().ToStdString() ) );
        return tl::unexpected( e );
    }

    BOARD* board = this->board();

    if( board->GetFileName().Matches( boardPath.GetFullPath() ) )
    {
        pcbContext()->SaveBoard();
        notifyDocumentSaved( boardPath.GetFullPath() );
        return Empty();
    }

    bool includeProject = true;

    if( aCtx.Request.has_options() )
        includeProject = aCtx.Request.options().include_project();

    pcbContext()->SavePcbCopy( boardPath.GetFullPath(), includeProject, /* aHeadless = */ true );

    return Empty();
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleRevertDocument(
        const HANDLER_CONTEXT<RevertDocument>& aCtx )
{
    // Validate first so a request meant for another editor's document is not captured here
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    // Reloading frees every item, so refuse while any client transaction is open; the staged
    // commit would otherwise dangle
    if( !m_commits.empty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BUSY );
        e.set_error_message( "cannot revert while a commit is open" );
        return tl::unexpected( e );
    }

    if( std::optional<ApiResponseStatus> headless = checkForHeadless( "RevertDocument" ) )
        return tl::unexpected( *headless );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    wxFileName fn = project().AbsolutePath( board()->GetFileName() );

    frame()->GetScreen()->SetContentModified( false );
    frame()->ReleaseFile();
    frame()->OpenProjectFiles( std::vector<wxString>( 1, fn.GetFullPath() ), KICTL_REVERT );

    bumpRevision();
    return Empty();
}


tl::expected<bool, ApiResponseStatus> API_HANDLER_PCB::validateDocumentInternal( const DocumentSpecifier& aDocument ) const
{
    if( aDocument.type() != DocumentType::DOCTYPE_PCB )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "the requested document is not a board" );
        return tl::unexpected( e );
    }

    wxFileName fn( pcbContext()->GetCurrentFileName() );

    if( aDocument.board_filename().compare( fn.GetFullName() ) != 0 )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "the requested document {} is not open",
                                          aDocument.board_filename() ) );
        return tl::unexpected( e );
    }

    return true;
}


HANDLER_RESULT<GetItemsResponse> API_HANDLER_PCB::handleGetItems( const HANDLER_CONTEXT<GetItems>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    // validateItemHeaderDocument already answers AS_UNHANDLED for another editor's
    // document type; anything else it reports is a real error for this client
    if( HANDLER_RESULT<std::optional<KIID>> header = validateItemHeaderDocument( aCtx.Request.header() );
        !header )
    {
        return tl::unexpected( header.error() );
    }

    GetItemsResponse response;

    std::vector<BOARD_ITEM*> items;
    std::set<KICAD_T>        typesRequested;

    if( !collectItems( parseRequestedItemTypes( aCtx.Request.types() ), items, typesRequested ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "none of the requested types are valid for a Board object" );
        return tl::unexpected( e );
    }

    std::erase_if( items,
                   [&]( const BOARD_ITEM* aItem )
                   {
                       return !typesRequested.count( aItem->Type() );
                   } );

    windowItems( aCtx.Request, items, response,
                 []( const BOARD_ITEM* aItem ) -> const EDA_ITEM*
                 {
                     return aItem;
                 } );

    for( const BOARD_ITEM* item : items )
    {
        google::protobuf::Any itemBuf;
        item->Serialize( itemBuf );
        response.mutable_items()->Add( std::move( itemBuf ) );
    }

    response.set_status( ItemRequestStatus::IRS_OK );
    return response;
}


bool API_HANDLER_PCB::collectItems( const std::vector<KICAD_T>& aTypes, std::vector<BOARD_ITEM*>& items,
                       std::set<KICAD_T>& typesRequested ) const
{
    BOARD* board = this->board();
    std::set<KICAD_T> typesInserted;
    bool              handledAnything = false;

    for( KICAD_T type : aTypes )
    {
        typesRequested.emplace( type );

        if( typesInserted.count( type ) )
            continue;

        switch( type )
        {
        case PCB_TRACE_T:
        case PCB_ARC_T:
        case PCB_VIA_T:
            handledAnything = true;
            std::copy( board->Tracks().begin(), board->Tracks().end(),
                       std::back_inserter( items ) );
            typesInserted.insert( { PCB_TRACE_T, PCB_ARC_T, PCB_VIA_T } );
            break;

        case PCB_PAD_T:
        {
            handledAnything = true;

            for( FOOTPRINT* fp : board->Footprints() )
            {
                std::copy( fp->Pads().begin(), fp->Pads().end(),
                           std::back_inserter( items ) );
            }

            typesInserted.insert( PCB_PAD_T );
            break;
        }

        case PCB_FOOTPRINT_T:
        {
            handledAnything = true;

            std::copy( board->Footprints().begin(), board->Footprints().end(),
                       std::back_inserter( items ) );

            typesInserted.insert( PCB_FOOTPRINT_T );
            break;
        }

        case PCB_SHAPE_T:
        case PCB_TABLE_T:
        case PCB_TEXT_T:
        case PCB_TEXTBOX_T:
        case PCB_BARCODE_T:
        case PCB_REFERENCE_IMAGE_T:
        case PCB_GRIDITEM_T:
        {
            handledAnything = true;
            bool inserted = false;

            for( BOARD_ITEM* item : board->Drawings() )
            {
                if( item->Type() == type )
                {
                    items.emplace_back( item );
                    inserted = true;
                }
            }

            if( inserted )
                typesInserted.insert( type );

            break;
        }

        case PCB_DIMENSION_T:
        {
            handledAnything = true;
            bool inserted = false;

            for( BOARD_ITEM* item : board->Drawings() )
            {
                switch (item->Type()) {
                    case PCB_DIM_ALIGNED_T:
                    case PCB_DIM_CENTER_T:
                    case PCB_DIM_RADIAL_T:
                    case PCB_DIM_ORTHOGONAL_T:
                    case PCB_DIM_LEADER_T:
                        items.emplace_back( item );
                        inserted = true;
                        break;
                    default:
                        break;
                }
            }
            // we have to add the dimension subtypes to the requested to get them out
            typesRequested.insert( {PCB_DIM_ALIGNED_T, PCB_DIM_CENTER_T, PCB_DIM_RADIAL_T, PCB_DIM_ORTHOGONAL_T, PCB_DIM_LEADER_T } );

            if( inserted )
                typesInserted.insert( {PCB_DIM_ALIGNED_T, PCB_DIM_CENTER_T, PCB_DIM_RADIAL_T, PCB_DIM_ORTHOGONAL_T, PCB_DIM_LEADER_T } );

            break;
        }

        case PCB_ZONE_T:
        {
            handledAnything = true;

            std::copy( board->Zones().begin(), board->Zones().end(),
                       std::back_inserter( items ) );

            typesInserted.insert( PCB_ZONE_T );
            break;
        }

        case PCB_GROUP_T:
        {
            handledAnything = true;

            std::copy( board->Groups().begin(), board->Groups().end(),
                       std::back_inserter( items ) );

            typesInserted.insert( PCB_GROUP_T );
            break;
        }

        case PCB_POINT_T:
        {
            handledAnything = true;
            std::copy( board->Points().begin(), board->Points().end(), std::back_inserter( items ) );
            typesInserted.insert( PCB_POINT_T );
            break;
        }

        case PCB_CONSTRAINT_T:
        {
            handledAnything = true;

            std::copy( board->Constraints().begin(), board->Constraints().end(),
                       std::back_inserter( items ) );

            typesInserted.insert( PCB_CONSTRAINT_T );
            break;
        }

        default:
            break;
        }
    }
    return handledAnything;
}


std::map<KICAD_T, uint32_t> API_HANDLER_PCB::countItems( const DocumentSpecifier& aDocument )
{
    // Every type handleGetItems serves
    static const std::vector<KICAD_T> allTypes = { PCB_TRACE_T,     PCB_ARC_T,     PCB_VIA_T,
                                                   PCB_PAD_T,       PCB_FOOTPRINT_T, PCB_SHAPE_T, PCB_TABLE_T,
                                                   PCB_TEXT_T,      PCB_TEXTBOX_T, PCB_BARCODE_T, PCB_REFERENCE_IMAGE_T,
                                                   PCB_GRIDITEM_T,  PCB_DIMENSION_T, PCB_ZONE_T, PCB_GROUP_T, PCB_POINT_T,
                                                   PCB_CONSTRAINT_T };

    std::vector<BOARD_ITEM*> items;
    std::set<KICAD_T>        typesRequested;

    collectItems( allTypes, items, typesRequested );

    std::map<KICAD_T, uint32_t> counts;

    for( const BOARD_ITEM* item : items )
    {
        if( typesRequested.count( item->Type() ) )
            ++counts[item->Type()];
    }

    return counts;
}


HANDLER_RESULT<BoardEnabledLayersResponse> API_HANDLER_PCB::handleSetBoardEnabledLayers(
        const HANDLER_CONTEXT<SetBoardEnabledLayers>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( aCtx.Request.copper_layer_count() % 2 != 0 )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "copper_layer_count must be an even number" );
        return tl::unexpected( e );
    }

    if( aCtx.Request.copper_layer_count() > MAX_CU_LAYERS )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "copper_layer_count must be below %d", MAX_CU_LAYERS ) );
        return tl::unexpected( e );
    }

    int copperLayerCount = static_cast<int>( aCtx.Request.copper_layer_count() );
    LSET enabled = board::UnpackLayerSet( aCtx.Request.layers() );

    // Sanitize the input
    enabled |= LSET( { Edge_Cuts, Margin, F_CrtYd, B_CrtYd } );
    enabled &= ~LSET::AllCuMask();
    enabled |= LSET::AllCuMask( copperLayerCount );

    BOARD* board = this->board();

    LSET previousEnabled = board->GetEnabledLayers();
    LSET changedLayers = enabled ^ previousEnabled;

    board->SetEnabledLayers( enabled );
    board->SetVisibleLayers( board->GetVisibleLayers() | changedLayers );

    LSEQ removedLayers;

    for( PCB_LAYER_ID layer_id : previousEnabled )
    {
        if( !enabled[layer_id] && board->HasItemsOnLayer( layer_id ) )
            removedLayers.push_back( layer_id );
    }

    bool modified = false;

    if( !removedLayers.empty() )
    {
        toolManager()->RunAction( PCB_ACTIONS::selectionClear );

        for( PCB_LAYER_ID layer_id : removedLayers )
            modified |= board->RemoveAllItemsOnLayer( layer_id );
    }

    if( frame() )
    {
        if( enabled != previousEnabled )
            frame()->UpdateUserInterface();

        if( modified )
            frame()->OnModify();
    }

    bumpRevision();

    BoardEnabledLayersResponse response;

    response.set_copper_layer_count( copperLayerCount );
    board::PackLayerSet( *response.mutable_layers(), enabled );

    return response;
}


HANDLER_RESULT<BoardStackupResponse> API_HANDLER_PCB::handleUpdateBoardStackup(
        const HANDLER_CONTEXT<UpdateBoardStackup>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    BOARD*                 board = this->board();
    BOARD_DESIGN_SETTINGS& bds = board->GetDesignSettings();

    // Start from the current stackup so that finish, impedance and edge settings omitted from
    // the request keep their values
    BOARD_STACKUP stackup = board->GetStackupOrDefault();

    if( !stackup.Deserialize( aCtx.Request.stackup() ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "stackup could not be parsed: every layer needs a known type and, "
                             "except for dielectric layers, a board layer of that type" );
        return tl::unexpected( e );
    }

    // The enabled copper layers define the copper layer count and must form the complete stack
    // from F.Cu to B.Cu; the other enabled entries define which physical layers the board has.
    LSEQ copperLayers;
    LSET stackupLayers;

    for( const BOARD_STACKUP_ITEM* item : stackup.GetList() )
    {
        if( !item->IsEnabled() || item->GetBrdLayerId() == UNDEFINED_LAYER )
            continue;

        stackupLayers.set( item->GetBrdLayerId() );

        if( item->GetType() == BS_ITEM_TYPE_COPPER )
            copperLayers.push_back( item->GetBrdLayerId() );
    }

    int copperLayerCount = static_cast<int>( copperLayers.size() );

    if( copperLayerCount < 2 || copperLayerCount % 2 != 0 || copperLayerCount > MAX_CU_LAYERS )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "stackup must contain an even number of enabled copper "
                                          "layers between 2 and {}", MAX_CU_LAYERS ) );
        return tl::unexpected( e );
    }

    if( copperLayers != LSET::AllCuMask( copperLayerCount ).CuStack() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "copper layers must be listed top to bottom as F.Cu, In1.Cu, ..., B.Cu" );
        return tl::unexpected( e );
    }

    // Only the layers that describe the physical board are affected; Edge.Cuts, courtyards, user
    // layers and the like are left as they are.
    LSET previousEnabled = board->GetEnabledLayers();
    LSET enabled = ( previousEnabled & ~BOARD_STACKUP::StackupAllowedBrdLayers() ) | stackupLayers;

    if( enabled != previousEnabled )
    {
        LSEQ removedLayers;

        for( PCB_LAYER_ID layer : previousEnabled )
        {
            if( !enabled[layer] && board->HasItemsOnLayer( layer ) )
                removedLayers.push_back( layer );
        }

        board->SetEnabledLayers( enabled );
        board->SetCopperLayerCount( copperLayerCount );
        board->SetVisibleLayers( board->GetVisibleLayers() | ( enabled ^ previousEnabled ) );

        if( !removedLayers.empty() )
        {
            toolManager()->RunAction( PCB_ACTIONS::selectionClear );

            for( PCB_LAYER_ID layer : removedLayers )
                board->RemoveAllItemsOnLayer( layer );

            // Undo state may hold pointers to the items deleted above
            if( frame() )
                frame()->ClearUndoRedoList();
        }
    }

    // Store the stackup; disabled entries are dropped, as the board setup dialog does
    BOARD_STACKUP& brdStackup = bds.GetStackupDescriptor();

    brdStackup.RemoveAll();

    for( const BOARD_STACKUP_ITEM* item : stackup.GetList() )
    {
        if( !item->IsEnabled() )
            continue;

        BOARD_STACKUP_ITEM* copy = new BOARD_STACKUP_ITEM( *item );

        if( copy->GetBrdLayerId() != UNDEFINED_LAYER )
            copy->SetLayerName( board->GetLayerName( copy->GetBrdLayerId() ) );

        brdStackup.Add( copy );
    }

    brdStackup.m_FinishType = stackup.m_FinishType;
    brdStackup.m_HasDielectricConstrains = stackup.m_HasDielectricConstrains;
    brdStackup.m_EdgeConnectorConstraints = stackup.m_EdgeConnectorConstraints;
    brdStackup.m_EdgePlating = stackup.m_EdgePlating;

    bds.m_HasStackup = true;
    bds.SetBoardThickness( brdStackup.BuildBoardThicknessFromStackup() );

    onModified();

    BoardStackupResponse response;
    board::PackBoardStackup( *board, *response.mutable_stackup() );
    return response;
}


HANDLER_RESULT<common::types::EmbeddedFiles>
API_HANDLER_PCB::handleGetEmbeddedFiles( const HANDLER_CONTEXT<GetEmbeddedFiles>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    common::types::EmbeddedFiles response;
    board::PackEmbeddedFiles( response, *board()->GetEmbeddedFiles() );
    return response;
}


HANDLER_RESULT<Empty> unpackEmbeddedFiles( EMBEDDED_FILES& aOutput, const common::types::EmbeddedFiles& aProto )
{
    if( !board::UnpackEmbeddedFiles( aOutput, aProto ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "embedded file validation failed" );
        return tl::unexpected( e );
    }

    return Empty();
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleAddEmbeddedFiles( const HANDLER_CONTEXT<AddEmbeddedFiles>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    EMBEDDED_FILES files;
    HANDLER_RESULT<Empty> result = unpackEmbeddedFiles( files, aCtx.Request.files() );

    if( !result.has_value() )
        return result;

    EMBEDDED_FILES* boardFiles = board()->GetEmbeddedFiles();

    for( const std::shared_ptr<EMBEDDED_FILES::EMBEDDED_FILE>& file : files.EmbeddedFileMap() | std::views::values )
    {
        auto copy = std::make_shared<EMBEDDED_FILES::EMBEDDED_FILE>( *file );
        boardFiles->AddFile( copy );
    }

    onModified();
    return result;
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleSetEmbeddedFiles( const HANDLER_CONTEXT<SetEmbeddedFiles>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    HANDLER_RESULT<Empty> result = unpackEmbeddedFiles( *board()->GetEmbeddedFiles(),
                                                        aCtx.Request.files() );

    if( !result.has_value() )
        return result;

    onModified();
    return result;
}


HANDLER_RESULT<BoardDesignRulesResponse> API_HANDLER_PCB::handleGetBoardDesignRules(
        const HANDLER_CONTEXT<GetBoardDesignRules>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    const BOARD_DESIGN_SETTINGS& bds = board()->GetDesignSettings();
    BoardDesignRulesResponse     response;
    kiapi::board::BoardDesignRules* rules = response.mutable_rules();

    kiapi::board::MinimumConstraints* constraints = rules->mutable_constraints();

    constraints->mutable_min_clearance()->set_value_nm( bds.m_MinClearance );
    constraints->mutable_min_groove_width()->set_value_nm( bds.m_MinGrooveWidth );
    constraints->mutable_min_connection_width()->set_value_nm( bds.m_MinConn );
    constraints->mutable_min_track_width()->set_value_nm( bds.m_TrackMinWidth );
    constraints->mutable_min_via_annular_width()->set_value_nm( bds.m_ViasMinAnnularWidth );
    constraints->mutable_min_via_size()->set_value_nm( bds.m_ViasMinSize );
    constraints->mutable_min_through_drill()->set_value_nm( bds.m_MinThroughDrill );
    constraints->mutable_min_microvia_size()->set_value_nm( bds.m_MicroViasMinSize );
    constraints->mutable_min_microvia_drill()->set_value_nm( bds.m_MicroViasMinDrill );
    constraints->mutable_copper_edge_clearance()->set_value_nm( bds.m_CopperEdgeClearance );
    constraints->mutable_hole_clearance()->set_value_nm( bds.m_HoleClearance );
    constraints->mutable_hole_to_hole_min()->set_value_nm( bds.m_HoleToHoleMin );
    constraints->mutable_silk_clearance()->set_value_nm( bds.m_SilkClearance );
    constraints->set_min_resolved_spokes( bds.m_MinResolvedSpokes );
    constraints->mutable_min_silk_text_height()->set_value_nm( bds.m_MinSilkTextHeight );
    constraints->mutable_min_silk_text_thickness()->set_value_nm( bds.m_MinSilkTextThickness );

    kiapi::board::PredefinedSizes* sizes = rules->mutable_predefined_sizes();

    for( size_t ii = 1; ii < bds.m_TrackWidthList.size(); ++ii )
        sizes->add_tracks()->mutable_width()->set_value_nm( bds.m_TrackWidthList[ii] );

    for( size_t ii = 1; ii < bds.m_ViasDimensionsList.size(); ++ii )
    {
        kiapi::board::PresetViaDimension* via = sizes->add_vias();
        via->mutable_diameter()->set_value_nm( bds.m_ViasDimensionsList[ii].m_Diameter );
        via->mutable_drill()->set_value_nm( bds.m_ViasDimensionsList[ii].m_Drill );
    }

    for( size_t ii = 1; ii < bds.m_DiffPairDimensionsList.size(); ++ii )
    {
        kiapi::board::PresetDiffPairDimension* pair = sizes->add_diff_pairs();
        pair->mutable_width()->set_value_nm( bds.m_DiffPairDimensionsList[ii].m_Width );
        pair->mutable_gap()->set_value_nm( bds.m_DiffPairDimensionsList[ii].m_Gap );
        pair->mutable_via_gap()->set_value_nm( bds.m_DiffPairDimensionsList[ii].m_ViaGap );
    }

    kiapi::board::SolderMaskPasteDefaults* maskPaste = rules->mutable_solder_mask_paste();

    maskPaste->mutable_mask_expansion()->set_value_nm( bds.m_SolderMaskExpansion );
    maskPaste->mutable_mask_min_width()->set_value_nm( bds.m_SolderMaskMinWidth );
    maskPaste->mutable_mask_to_copper_clearance()->set_value_nm( bds.m_SolderMaskToCopperClearance );
    maskPaste->mutable_paste_margin()->set_value_nm( bds.m_SolderPasteMargin );
    maskPaste->set_paste_margin_ratio( bds.m_SolderPasteMarginRatio );
    maskPaste->set_allow_soldermask_bridges_in_footprints( bds.m_AllowSoldermaskBridgesInFPs );

    kiapi::board::TeardropDefaults* teardrops = rules->mutable_teardrops();

    teardrops->set_target_vias( bds.m_TeardropParamsList.m_TargetVias );
    teardrops->set_target_pth_pads( bds.m_TeardropParamsList.m_TargetPTHPads );
    teardrops->set_target_smd_pads( bds.m_TeardropParamsList.m_TargetSMDPads );
    teardrops->set_target_track_to_track( bds.m_TeardropParamsList.m_TargetTrack2Track );
    teardrops->set_use_round_shapes_only( bds.m_TeardropParamsList.m_UseRoundShapesOnly );

    TEARDROP_PARAMETERS_LIST& tdList = const_cast<TEARDROP_PARAMETERS_LIST&>( bds.m_TeardropParamsList );

    for( int target = TARGET_ROUND; target <= TARGET_TRACK; ++target )
    {
        const TEARDROP_PARAMETERS* params = tdList.GetParameters( static_cast<TARGET_TD>( target ) );
        kiapi::board::TeardropTargetEntry* entry = teardrops->add_target_params();

        entry->set_target( ToProtoEnum<TARGET_TD, kiapi::board::TeardropTarget>(
            static_cast<TARGET_TD>( target ) ) );
        entry->mutable_params()->set_enabled( params->m_Enabled );
        entry->mutable_params()->mutable_max_length()->set_value_nm( params->m_TdMaxLen );
        entry->mutable_params()->mutable_max_width()->set_value_nm( params->m_TdMaxWidth );
        entry->mutable_params()->set_best_length_ratio( params->m_BestLengthRatio );
        entry->mutable_params()->set_best_width_ratio( params->m_BestWidthRatio );
        entry->mutable_params()->set_width_to_size_filter_ratio( params->m_WidthtoSizeFilterRatio );
        entry->mutable_params()->set_curved_edges( params->m_CurvedEdges );
        entry->mutable_params()->set_allow_two_tracks( params->m_AllowUseTwoTracks );
        entry->mutable_params()->set_on_pads_in_zones( params->m_TdOnPadsInZones );
    }

    kiapi::board::ViaProtectionDefaults* viaProtection = rules->mutable_via_protection();

    viaProtection->set_tent_front( bds.m_TentViasFront );
    viaProtection->set_tent_back( bds.m_TentViasBack );
    viaProtection->set_cover_front( bds.m_CoverViasFront );
    viaProtection->set_cover_back( bds.m_CoverViasBack );
    viaProtection->set_plug_front( bds.m_PlugViasFront );
    viaProtection->set_plug_back( bds.m_PlugViasBack );
    viaProtection->set_cap( bds.m_CapVias );
    viaProtection->set_fill( bds.m_FillVias );

    for( const auto& [errorCode, severity] : bds.m_DRCSeverities )
    {
        board::DrcSeveritySetting* setting = rules->add_severities();
        setting->set_rule_type(
                ToProtoEnum<PCB_DRC_CODE, board::DrcErrorType>( static_cast<PCB_DRC_CODE>( errorCode ) ) );
        setting->set_severity( ToProtoEnum<SEVERITY, types::RuleSeverity>( severity ) );
    }

    for( const DRC_EXCLUSION& exclusion : bds.m_DrcExclusions )
        rules->add_exclusions()->CopyFrom( exclusion.ToProto() );

    response.set_custom_rules_status( CRS_NONE );

    wxString rulesPath = board()->GetDesignRulesPath();

    if( !rulesPath.IsEmpty() && wxFileName::IsFileReadable( rulesPath ) )
    {
        wxFFile file( rulesPath, "r" );
        wxString content;
        std::vector<std::shared_ptr<DRC_RULE>> parsedRules;

        if( !file.IsOpened() )
        {
            response.set_custom_rules_status( CRS_INVALID );
            return response;
        }

        file.ReadAll( &content );
        file.Close();

        try
        {
            DRC_RULES_PARSER parser( content, "File" );
            parser.Parse( parsedRules, nullptr );
            response.set_custom_rules_status( CRS_VALID );
        }
        catch( const IO_ERROR& )
        {
            response.set_custom_rules_status( CRS_INVALID );
        }
    }

    return response;
}


HANDLER_RESULT<BoardDesignRulesResponse> API_HANDLER_PCB::handleSetBoardDesignRules(
        const HANDLER_CONTEXT<SetBoardDesignRules>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    BOARD_DESIGN_SETTINGS newSettings( board()->GetDesignSettings() );
    const kiapi::board::BoardDesignRules& rules = aCtx.Request.rules();

    if( rules.has_constraints() )
    {
        const kiapi::board::MinimumConstraints& constraints = rules.constraints();

        newSettings.m_MinClearance = constraints.min_clearance().value_nm();
        newSettings.m_MinGrooveWidth = constraints.min_groove_width().value_nm();
        newSettings.m_MinConn = constraints.min_connection_width().value_nm();
        newSettings.m_TrackMinWidth = constraints.min_track_width().value_nm();
        newSettings.m_ViasMinAnnularWidth = constraints.min_via_annular_width().value_nm();
        newSettings.m_ViasMinSize = constraints.min_via_size().value_nm();
        newSettings.m_MinThroughDrill = constraints.min_through_drill().value_nm();
        newSettings.m_MicroViasMinSize = constraints.min_microvia_size().value_nm();
        newSettings.m_MicroViasMinDrill = constraints.min_microvia_drill().value_nm();
        newSettings.m_CopperEdgeClearance = constraints.copper_edge_clearance().value_nm();
        newSettings.m_HoleClearance = constraints.hole_clearance().value_nm();
        newSettings.m_HoleToHoleMin = constraints.hole_to_hole_min().value_nm();
        newSettings.m_SilkClearance = constraints.silk_clearance().value_nm();
        newSettings.m_MinResolvedSpokes = constraints.min_resolved_spokes();
        newSettings.m_MinSilkTextHeight = constraints.min_silk_text_height().value_nm();
        newSettings.m_MinSilkTextThickness = constraints.min_silk_text_thickness().value_nm();
    }

    if( rules.has_predefined_sizes() )
    {
        newSettings.m_TrackWidthList.clear();
        newSettings.m_TrackWidthList.emplace_back( 0 );

        for( const kiapi::board::PresetTrackWidth& track : rules.predefined_sizes().tracks() )
            newSettings.m_TrackWidthList.emplace_back( track.width().value_nm() );

        newSettings.m_ViasDimensionsList.clear();
        newSettings.m_ViasDimensionsList.emplace_back( 0, 0 );

        for( const kiapi::board::PresetViaDimension& via : rules.predefined_sizes().vias() )
        {
            newSettings.m_ViasDimensionsList.emplace_back( static_cast<int>( via.diameter().value_nm() ),
                                                           static_cast<int>( via.drill().value_nm() ) );
        }

        newSettings.m_DiffPairDimensionsList.clear();
        newSettings.m_DiffPairDimensionsList.emplace_back( 0, 0, 0 );

        for( const kiapi::board::PresetDiffPairDimension& pair : rules.predefined_sizes().diff_pairs() )
        {
            newSettings.m_DiffPairDimensionsList.emplace_back(
                    static_cast<int>( pair.width().value_nm() ),
                    static_cast<int>( pair.gap().value_nm() ),
                    static_cast<int>( pair.via_gap().value_nm() ) );
        }
    }

    if( rules.has_solder_mask_paste() )
    {
        const kiapi::board::SolderMaskPasteDefaults& maskPaste = rules.solder_mask_paste();

        newSettings.m_SolderMaskExpansion = maskPaste.mask_expansion().value_nm();
        newSettings.m_SolderMaskMinWidth = maskPaste.mask_min_width().value_nm();
        newSettings.m_SolderMaskToCopperClearance = maskPaste.mask_to_copper_clearance().value_nm();
        newSettings.m_SolderPasteMargin = maskPaste.paste_margin().value_nm();
        newSettings.m_SolderPasteMarginRatio = maskPaste.paste_margin_ratio();
        newSettings.m_AllowSoldermaskBridgesInFPs =
                maskPaste.allow_soldermask_bridges_in_footprints();
    }

    if( rules.has_teardrops() )
    {
        const kiapi::board::TeardropDefaults& teardrops = rules.teardrops();

        newSettings.m_TeardropParamsList.m_TargetVias = teardrops.target_vias();
        newSettings.m_TeardropParamsList.m_TargetPTHPads = teardrops.target_pth_pads();
        newSettings.m_TeardropParamsList.m_TargetSMDPads = teardrops.target_smd_pads();
        newSettings.m_TeardropParamsList.m_TargetTrack2Track = teardrops.target_track_to_track();
        newSettings.m_TeardropParamsList.m_UseRoundShapesOnly = teardrops.use_round_shapes_only();

        for( const kiapi::board::TeardropTargetEntry& entry : teardrops.target_params() )
        {
            if( entry.target() == kiapi::board::TeardropTarget::TDT_UNKNOWN )
                continue;

            TARGET_TD target = FromProtoEnum<TARGET_TD, kiapi::board::TeardropTarget>(
                    entry.target() );

            TEARDROP_PARAMETERS* params = newSettings.m_TeardropParamsList.GetParameters( target );

            params->m_Enabled = entry.params().enabled();
            params->m_TdMaxLen = entry.params().max_length().value_nm();
            params->m_TdMaxWidth = entry.params().max_width().value_nm();
            params->m_BestLengthRatio = entry.params().best_length_ratio();
            params->m_BestWidthRatio = entry.params().best_width_ratio();
            params->m_WidthtoSizeFilterRatio = entry.params().width_to_size_filter_ratio();
            params->m_CurvedEdges = entry.params().curved_edges();
            params->m_AllowUseTwoTracks = entry.params().allow_two_tracks();
            params->m_TdOnPadsInZones = entry.params().on_pads_in_zones();
        }
    }

    if( rules.has_via_protection() )
    {
        const kiapi::board::ViaProtectionDefaults& viaProtection = rules.via_protection();

        newSettings.m_TentViasFront = viaProtection.tent_front();
        newSettings.m_TentViasBack = viaProtection.tent_back();
        newSettings.m_CoverViasFront = viaProtection.cover_front();
        newSettings.m_CoverViasBack = viaProtection.cover_back();
        newSettings.m_PlugViasFront = viaProtection.plug_front();
        newSettings.m_PlugViasBack = viaProtection.plug_back();
        newSettings.m_CapVias = viaProtection.cap();
        newSettings.m_FillVias = viaProtection.fill();
    }

    if( rules.severities_size() > 0 )
    {
        newSettings.m_DRCSeverities.clear();

        for( const kiapi::board::DrcSeveritySetting& severitySetting : rules.severities() )
        {
            PCB_DRC_CODE ruleType =
                    FromProtoEnum<PCB_DRC_CODE, kiapi::board::DrcErrorType>( severitySetting.rule_type() );

            const std::unordered_set<SEVERITY> permitted( { RPT_SEVERITY_ERROR, RPT_SEVERITY_WARNING, RPT_SEVERITY_IGNORE } );
            SEVERITY setting = FromProtoEnum<SEVERITY, kiapi::common::types::RuleSeverity>( severitySetting.severity() );

            if( !permitted.contains( setting ) )
            {
                ApiResponseStatus e;
                e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                e.set_error_message( fmt::format( "DRC severity must be error, warning, or ignore" ) );
                return tl::unexpected( e );
            }

            newSettings.m_DRCSeverities[ruleType] = setting;
        }
    }

    if( rules.exclusions_size() > 0 )
    {
        newSettings.m_DrcExclusions.clear();

        for( const kiapi::board::DrcExclusion& exclusion : rules.exclusions() )
            newSettings.m_DrcExclusions.insert( DRC_EXCLUSION::FromProto( exclusion ) );
    }

    std::vector<BOARD_DESIGN_SETTINGS::VALIDATION_ERROR> errors = newSettings.ValidateDesignRules();

    if( !errors.empty() )
    {
        const BOARD_DESIGN_SETTINGS::VALIDATION_ERROR& error = errors.front();

        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "Invalid board design rules: {}: {}",
                                          error.setting_name.ToStdString(),
                                          error.error_message.ToStdString() ) );
        return tl::unexpected( e );
    }

    board()->SetDesignSettings( newSettings );

    if( frame() )
    {
        frame()->OnModify();
        frame()->UpdateUserInterface();
    }

    bumpRevision();

    HANDLER_CONTEXT<GetBoardDesignRules> getCtx = { aCtx.ClientName, GetBoardDesignRules() };
    *getCtx.Request.mutable_board() = aCtx.Request.board();

    publishProjectChanged( kiapi::common::events::PCK_SETTINGS, aCtx.ClientName );

    return handleGetBoardDesignRules( getCtx );
}


HANDLER_RESULT<CustomRulesResponse> API_HANDLER_PCB::handleGetCustomDesignRules(
        const HANDLER_CONTEXT<GetCustomDesignRules>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    CustomRulesResponse response;
    response.set_status( CRS_NONE );

    wxString rulesPath = board()->GetDesignRulesPath();

    if( rulesPath.IsEmpty() || !wxFileName::IsFileReadable( rulesPath ) )
        return response;

    wxFFile file( rulesPath, "r" );

    if( !file.IsOpened() )
    {
        response.set_status( CRS_INVALID );
        response.set_error_text( "Failed to open custom rules file" );
        return response;
    }

    wxString content;
    file.ReadAll( &content );
    file.Close();

    std::vector<std::shared_ptr<DRC_RULE>> parsedRules;

    try
    {
        DRC_RULES_PARSER parser( content, "File" );
        parser.Parse( parsedRules, nullptr );
    }
    catch( const IO_ERROR& ioe )
    {
        response.set_status( CRS_INVALID );
        response.set_error_text( ioe.What().ToStdString() );
        return response;
    }

    for( const std::shared_ptr<DRC_RULE>& rule : parsedRules )
    {
        // TODO(JE) since we now need this for both here and the rules editor, maybe it's time
        // to just make comment parsing part of the parser?
        wxString text = DRC_RULE_LOADER::ExtractRuleText( content, rule->m_Name );
        wxString comment = DRC_RULE_LOADER::ExtractRuleComment( text );

        kiapi::board::CustomRule* customRule = response.add_rules();

        if( rule->m_Condition )
            customRule->set_condition( rule->m_Condition->GetExpression().ToUTF8() );

        for( const DRC_CONSTRAINT& constraint : rule->m_Constraints )
        {
            board::CustomRuleConstraint* constraintProto = customRule->add_constraints();
            constraint.ToProto( *constraintProto );
        }

        customRule->set_severity( ToProtoEnum<SEVERITY, types::RuleSeverity>( rule->m_Severity ) );
        customRule->set_name( rule->m_Name.ToUTF8() );

        if( rule->m_LayerSource.CmpNoCase( wxS( "outer" ) ) == 0 )
        {
            customRule->set_layer_mode( kiapi::board::CRLM_OUTER );
        }
        else if( rule->m_LayerSource.CmpNoCase( wxS( "inner" ) ) == 0 )
        {
            customRule->set_layer_mode( kiapi::board::CRLM_INNER );
        }
        else if( !rule->m_LayerSource.IsEmpty() )
        {
            int layer = LSET::NameToLayer( rule->m_LayerSource );

            if( layer != UNDEFINED_LAYER && layer != UNSELECTED_LAYER && layer < PCB_LAYER_ID_COUNT )
            {
                customRule->set_single_layer(
                        ToProtoEnum<PCB_LAYER_ID, board::types::BoardLayer>( static_cast<PCB_LAYER_ID>( layer ) ) );
            }
        }

        if( !comment.IsEmpty() )
            customRule->set_comments( comment );
    }

    response.set_status( CRS_VALID );
    return response;
}


HANDLER_RESULT<CustomRulesResponse> API_HANDLER_PCB::handleSetCustomDesignRules(
        const HANDLER_CONTEXT<SetCustomDesignRules>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    wxString rulesPath = board()->GetDesignRulesPath();

    if( aCtx.Request.rules_size() == 0 )
    {
        if( wxFileName::FileExists( rulesPath ) )
        {
            if( !wxRemoveFile( rulesPath ) )
            {
                CustomRulesResponse response;
                response.set_status( CRS_INVALID );
                response.set_error_text( "Failed to remove custom rules file" );
                return response;
            }
        }

        CustomRulesResponse response;
        response.set_status( CRS_NONE );
        return response;
    }

    wxString rulesText;
    rulesText << "(version 2)\n";

    for( const board::CustomRule& rule : aCtx.Request.rules() )
    {
        wxString serializationError;
        wxString serializedRule = DRC_RULE::FormatRuleFromProto( rule, &serializationError );

        if( serializedRule.IsEmpty() )
        {
            CustomRulesResponse response;
            response.set_status( CRS_INVALID );

            if( serializationError.IsEmpty() )
                response.set_error_text( "Failed to serialize custom rule" );
            else
                response.set_error_text( serializationError.ToUTF8() );

            return response;
        }

        rulesText << "\n" << serializedRule;
    }

    // Validate generated file text before writing so callers get parser errors in response.
    try
    {
        std::vector<std::shared_ptr<DRC_RULE>> parsedRules;
        DRC_RULES_PARSER parser( rulesText, "SetCustomDesignRules" );
        parser.Parse( parsedRules, nullptr );
    }
    catch( const IO_ERROR& ioe )
    {
        CustomRulesResponse response;
        response.set_status( CRS_INVALID );
        response.set_error_text( ioe.What().ToStdString() );
        return response;
    }

    wxFFile file( rulesPath, "w" );

    if( !file.IsOpened() )
    {
        CustomRulesResponse response;
        response.set_status( CRS_INVALID );
        response.set_error_text( "Failed to open custom rules file for writing" );
        return response;
    }

    if( !file.Write( rulesText ) )
    {
        file.Close();

        CustomRulesResponse response;
        response.set_status( CRS_INVALID );
        response.set_error_text( "Failed to write custom rules file" );
        return response;
    }

    file.Close();
    bumpRevision();

    HANDLER_CONTEXT<GetCustomDesignRules> getCtx = { aCtx.ClientName, GetCustomDesignRules() };
    *getCtx.Request.mutable_board() = aCtx.Request.board();
    publishProjectChanged( kiapi::common::events::PCK_SETTINGS, aCtx.ClientName );

    return handleGetCustomDesignRules( getCtx );
}


HANDLER_RESULT<types::Vector2> API_HANDLER_PCB::handleGetBoardOrigin(
        const HANDLER_CONTEXT<GetBoardOrigin>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );
        !documentValidation )
    {
        return tl::unexpected( documentValidation.error() );
    }

    VECTOR2I origin;
    const BOARD_DESIGN_SETTINGS& settings = board()->GetDesignSettings();

    switch( aCtx.Request.type() )
    {
    case BOT_GRID:
        origin = settings.GetGridOrigin();
        break;

    case BOT_DRILL:
        origin = settings.GetAuxOrigin();
        break;

    default:
    case BOT_UNKNOWN:
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "Unexpected origin type" );
        return tl::unexpected( e );
    }
    }

    types::Vector2 reply;
    PackVector2( reply, origin );
    return reply;
}

HANDLER_RESULT<Empty> API_HANDLER_PCB::handleSetBoardOrigin(
        const HANDLER_CONTEXT<SetBoardOrigin>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );
        !documentValidation )
    {
        return tl::unexpected( documentValidation.error() );
    }

    VECTOR2I origin = UnpackVector2( aCtx.Request.origin() );

    switch( aCtx.Request.type() )
    {
    case BOT_GRID:
    {
        PCB_EDIT_FRAME* f = frame();

        if( f )
        {
            frame()->CallAfter(
                    [f, origin]()
                    {
                        // gridSetOrigin takes ownership and frees this
                        VECTOR2D*     dorigin = new VECTOR2D( origin );
                        TOOL_MANAGER* mgr = f->GetToolManager();
                        mgr->RunAction( PCB_ACTIONS::gridSetOrigin, dorigin );
                        f->Refresh();
                    } );
        }
        else
        {
            recordOriginUndo( aCtx.ClientName, UNDO_REDO::GRIDORIGIN, board()->GetDesignSettings().GetGridOrigin(),
                              origin );
            board()->GetDesignSettings().SetGridOrigin( origin );
        }

        break;
    }

    case BOT_DRILL:
    {
        PCB_EDIT_FRAME* f = frame();

        if( f )
        {
            frame()->CallAfter(
                    [f, origin]()
                    {
                        TOOL_MANAGER* mgr = f->GetToolManager();
                        mgr->RunAction( PCB_ACTIONS::drillSetOrigin, origin );
                        f->Refresh();
                    } );
        }
        else
        {
            recordOriginUndo( aCtx.ClientName, UNDO_REDO::DRILLORIGIN, board()->GetDesignSettings().GetAuxOrigin(),
                              origin );
            board()->GetDesignSettings().SetAuxOrigin( origin );
        }

        break;
    }

    default:
    case BOT_UNKNOWN:
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "Unexpected origin type" );
        return tl::unexpected( e );
    }
    }

    bumpRevision();
    return Empty();
}


HANDLER_RESULT<BoardLayerNameResponse> API_HANDLER_PCB::handleGetBoardLayerName(
            const HANDLER_CONTEXT<GetBoardLayerName>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );
        !documentValidation )
    {
        return tl::unexpected( documentValidation.error() );
    }

    BoardLayerNameResponse response;

    PCB_LAYER_ID id = FromProtoEnum<PCB_LAYER_ID>( aCtx.Request.layer() );

    response.set_name( board()->GetLayerName( id ) );

    return response;
}


HANDLER_RESULT<BoardLayerResponse> API_HANDLER_PCB::handleGetBoardLayerByName(
            const HANDLER_CONTEXT<GetBoardLayerByName>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );
        !documentValidation )
    {
        return tl::unexpected( documentValidation.error() );
    }

    BoardLayerResponse response;

    PCB_LAYER_ID id = board()->GetLayerID( wxString::FromUTF8( aCtx.Request.name() ) );
    response.set_layer( ToProtoEnum<PCB_LAYER_ID, board::types::BoardLayer>( id ) );

    return response;
}


std::optional<TITLE_BLOCK*> API_HANDLER_PCB::getTitleBlock( const DocumentSpecifier& aDocument )
{
    return &context()->GetBoard()->GetTitleBlock();
}


std::optional<PAGE_INFO> API_HANDLER_PCB::getPageSettings( const DocumentSpecifier& aDocument )
{
    return context()->GetBoard()->GetPageSettings();
}


bool API_HANDLER_PCB::setPageSettings( const DocumentSpecifier& aDocument, const PAGE_INFO& aPageInfo )
{
    context()->GetBoard()->SetPageSettings( aPageInfo );
    return true;
}


wxString API_HANDLER_PCB::getDrawingSheetFileName()
{
    return BASE_SCREEN::m_DrawingSheetFileName;
}


void API_HANDLER_PCB::setDrawingSheetFileName( const wxString& aFileName )
{
    BASE_SCREEN::m_DrawingSheetFileName = aFileName;

    if( frame() )
        frame()->LoadDrawingSheet();
}


void API_HANDLER_PCB::onModified()
{
    API_HANDLER_EDITOR::onModified();

    if( frame() )
    {
        frame()->Refresh();
        frame()->OnModify();
        frame()->UpdateUserInterface();
    }
}


HANDLER_RESULT<NetsResponse> API_HANDLER_PCB::handleGetNets( const HANDLER_CONTEXT<GetNets>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    NetsResponse response;
    BOARD* board = this->board();

    std::set<wxString> netclassFilter;

    for( const std::string& nc : aCtx.Request.netclass_filter() )
        netclassFilter.insert( wxString( nc.c_str(), wxConvUTF8 ) );

    for( NETINFO_ITEM* net : board->GetNetInfo() )
    {
        NETCLASS* nc = net->GetNetClass();

        if( !netclassFilter.empty() && nc )
        {
            bool inClass = false;

            for( const wxString& filter : netclassFilter )
            {
                if( nc->ContainsNetclassWithName( filter ) )
                {
                    inClass = true;
                    break;
                }
            }

            if( !inClass )
                continue;
        }

        board::types::Net* netProto = response.add_nets();
        netProto->set_name( net->GetNetname() );
        netProto->mutable_code()->set_value( net->GetNetCode() );
    }

    return response;
}


HANDLER_RESULT<GetItemsResponse> API_HANDLER_PCB::handleGetConnectedItems(
        const HANDLER_CONTEXT<GetConnectedItems>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    // validateItemHeaderDocument already answers AS_UNHANDLED for another editor's
    // document type; anything else it reports is a real error for this client
    if( HANDLER_RESULT<std::optional<KIID>> header = validateItemHeaderDocument( aCtx.Request.header() );
        !header )
    {
        return tl::unexpected( header.error() );
    }

    std::vector<KICAD_T> types = parseRequestedItemTypes( aCtx.Request.types() );
    const bool filterByType = aCtx.Request.types_size() > 0;

    if( filterByType && types.empty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "none of the requested types are valid for a Board object" );
        return tl::unexpected( e );
    }

    std::set<KICAD_T> typeFilter( types.begin(), types.end() );
    std::vector<BOARD_CONNECTED_ITEM*> sourceItems;

    for( const types::KIID& id : aCtx.Request.items() )
    {
        if( std::optional<BOARD_ITEM*> item = getItemById( KIID( id.value() ) ) )
        {
            if( BOARD_CONNECTED_ITEM* connected = dynamic_cast<BOARD_CONNECTED_ITEM*>( *item ) )
                sourceItems.emplace_back( connected );
        }
    }

    if( sourceItems.empty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "none of the requested IDs were found or valid connected items" );
        return tl::unexpected( e );
    }

    GetItemsResponse response;
    std::shared_ptr<CONNECTIVITY_DATA> conn = board()->GetConnectivity();
    std::set<KIID> insertedItems;

    for( BOARD_CONNECTED_ITEM* source : sourceItems )
    {
        for( BOARD_CONNECTED_ITEM* connected : conn->GetConnectedItems( source ) )
        {
            if( filterByType && !typeFilter.contains( connected->Type() ) )
                continue;

            if( !insertedItems.insert( connected->m_Uuid ).second )
                continue;

            connected->Serialize( *response.add_items() );
        }
    }

    response.set_status( ItemRequestStatus::IRS_OK );
    return response;
}


HANDLER_RESULT<GetItemsResponse> API_HANDLER_PCB::handleGetItemsByNet(
        const HANDLER_CONTEXT<GetItemsByNet>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    // validateItemHeaderDocument already answers AS_UNHANDLED for another editor's
    // document type; anything else it reports is a real error for this client
    if( HANDLER_RESULT<std::optional<KIID>> header = validateItemHeaderDocument( aCtx.Request.header() );
        !header )
    {
        return tl::unexpected( header.error() );
    }

    std::vector<KICAD_T> types = parseRequestedItemTypes( aCtx.Request.types() );
    const bool filterByType = aCtx.Request.types_size() > 0;

    if( filterByType && types.empty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "none of the requested types are valid for a Board object" );
        return tl::unexpected( e );
    }

    if( !filterByType )
        types.assign( { PCB_PAD_T, PCB_VIA_T, PCB_TRACE_T, PCB_ARC_T, PCB_SHAPE_T, PCB_ZONE_T } );

    GetItemsResponse response;
    BOARD* board = this->board();
    std::shared_ptr<CONNECTIVITY_DATA> conn = board->GetConnectivity();
    std::set<KIID> insertedItems;

    const NETINFO_LIST& nets = board->GetNetInfo();

    for( const board::types::Net& net : aCtx.Request.nets() )
    {
        NETINFO_ITEM* netInfo = nets.GetNetItem( wxString::FromUTF8( net.name() ) );

        if( !netInfo )
            continue;

        for( BOARD_CONNECTED_ITEM* item : conn->GetNetItems( netInfo->GetNetCode(), types ) )
        {
            if( !insertedItems.insert( item->m_Uuid ).second )
                continue;

            item->Serialize( *response.add_items() );
        }
    }

    response.set_status( ItemRequestStatus::IRS_OK );
    return response;
}


HANDLER_RESULT<GetItemsResponse> API_HANDLER_PCB::handleGetItemsByNetClass(
        const HANDLER_CONTEXT<GetItemsByNetClass>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    // validateItemHeaderDocument already answers AS_UNHANDLED for another editor's
    // document type; anything else it reports is a real error for this client
    if( HANDLER_RESULT<std::optional<KIID>> header = validateItemHeaderDocument( aCtx.Request.header() );
        !header )
    {
        return tl::unexpected( header.error() );
    }

    std::vector<KICAD_T> types = parseRequestedItemTypes( aCtx.Request.types() );
    const bool filterByType = aCtx.Request.types_size() > 0;

    if( filterByType && types.empty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "none of the requested types are valid for a Board object" );
        return tl::unexpected( e );
    }

    if( !filterByType )
        types.assign( { PCB_PAD_T, PCB_VIA_T, PCB_TRACE_T, PCB_ARC_T, PCB_SHAPE_T, PCB_ZONE_T } );

    std::set<wxString> requestedClasses;

    for( const std::string& netClass : aCtx.Request.net_classes() )
        requestedClasses.insert( wxString( netClass.c_str(), wxConvUTF8 ) );

    GetItemsResponse response;
    BOARD* board = this->board();
    std::shared_ptr<CONNECTIVITY_DATA> conn = board->GetConnectivity();
    std::set<KIID> insertedItems;

    for( NETINFO_ITEM* net : board->GetNetInfo() )
    {
        if( !net )
            continue;

        NETCLASS* nc = net->GetNetClass();

        if( !requestedClasses.empty() )
        {
            if( !nc )
                continue;

            bool inClass = false;

            for( const wxString& filter : requestedClasses )
            {
                if( nc->ContainsNetclassWithName( filter ) )
                {
                    inClass = true;
                    break;
                }
            }

            if( !inClass )
                continue;
        }

        for( BOARD_CONNECTED_ITEM* item : conn->GetNetItems( net->GetNetCode(), types ) )
        {
            if( !insertedItems.insert( item->m_Uuid ).second )
                continue;

            item->Serialize( *response.add_items() );
        }
    }

    response.set_status( ItemRequestStatus::IRS_OK );
    return response;
}


HANDLER_RESULT<NetClassForNetsResponse> API_HANDLER_PCB::handleGetNetClassForNets(
            const HANDLER_CONTEXT<GetNetClassForNets>& aCtx )
{
    NetClassForNetsResponse response;

    BOARD* board = this->board();
    const NETINFO_LIST& nets = board->GetNetInfo();
    for( const board::types::Net& net : aCtx.Request.net() )
    {
        NETINFO_ITEM* netInfo = nets.GetNetItem( wxString::FromUTF8( net.name() ) );

        if( !netInfo )
            continue;

        auto [pair, rc] = response.mutable_classes()->insert( { net.name(), {} } );
        netInfo->GetNetClass()->Serialize( pair->second );
    }

    return response;
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleRefillZones( const HANDLER_CONTEXT<RefillZones>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    TOOL_MANAGER* mgr = toolManager();

    // A frame's tool manager always carries the zone filler tool; headless sessions start with a
    // bare tool manager and register it on first use, like the CLI jobs do.
    if( !mgr->FindTool( ZONE_FILLER_TOOL_NAME ) )
        mgr->RegisterTool( new ZONE_FILLER_TOOL );

    if( aCtx.Request.zones().empty() )
    {
        if( frame() )
        {
            frame()->CallAfter( [mgr]()
                                {
                                    mgr->RunAction( PCB_ACTIONS::zoneFillAll );
                                } );
        }
        else
        {
            // Headless sessions have no event loop to defer to; fill synchronously through the
            // same tool the CLI jobs use.
            mgr->GetTool<ZONE_FILLER_TOOL>()->FillAllZones( nullptr, nullptr, true );
        }
    }
    else
    {
        std::vector<ZONE*> toFill;

        for( const types::KIID& id : aCtx.Request.zones() )
        {
            std::optional<BOARD_ITEM*> item = getItemById( KIID( id.value() ) );

            if( !item || ( *item )->Type() != PCB_ZONE_T )
            {
                ApiResponseStatus e;
                e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                e.set_error_message( fmt::format( "zone with ID {} not found on the board", id.value() ) );
                return tl::unexpected( e );
            }

            ZONE* zone = static_cast<ZONE*>( *item );

            // The filler silently skips rule areas, which would turn this into a false success
            if( zone->GetIsRuleArea() )
            {
                ApiResponseStatus e;
                e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                e.set_error_message( fmt::format( "zone with ID {} is a rule area and cannot be filled",
                                                  id.value() ) );
                return tl::unexpected( e );
            }

            // A repeated id would enqueue concurrent fill tasks for the same zone
            if( !alg::contains( toFill, zone ) )
                toFill.push_back( zone );
        }

        std::unique_ptr<COMMIT> commit = createCommit();
        ZONE_FILLER             filler( board(), commit.get() );

        if( !filler.Fill( toFill ) )
        {
            commit->Revert();

            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_UNKNOWN );
            e.set_error_message( "zone fill failed" );
            return tl::unexpected( e );
        }

        commit->Push( _( "Fill Zone(s)" ), SKIP_CONNECTIVITY | ZONE_FILL_OP );

        // Push skipped connectivity, so run the same post-fill refresh as the interactive fill
        mgr->GetTool<ZONE_FILLER_TOOL>()->PostFillRefresh( frame() == nullptr );
    }

    bumpRevision();
    return Empty();
}


HANDLER_RESULT<ImportNetlistResponse> API_HANDLER_PCB::handleImportNetlist( const HANDLER_CONTEXT<ImportNetlist>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    wxFileName netlistPath( project().AbsolutePath( wxString::FromUTF8( aCtx.Request.netlist_path() ) ) );

    if( !netlistPath.IsOk() || !netlistPath.FileExists() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message(
                fmt::format( "netlist file '{}' could not be opened", netlistPath.GetFullPath().ToStdString() ) );
        return tl::unexpected( e );
    }

    PCB_CONTEXT*       ctx = pcbContext();
    WX_STRING_REPORTER reporter;

    const bool lookupByTimestamp = aCtx.Request.match_mode() != NetlistMatchMode::NMM_REFERENCE;

    NETLIST netlist;
    netlist.SetFindByTimeStamp( lookupByTimestamp );
    netlist.SetReplaceFootprints( aCtx.Request.update_footprints() );

    if( !ctx->ReadNetlistFromFile( netlistPath.GetFullPath(), netlist, reporter ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "unable to handle netlist file '{}': {}",
                                          netlistPath.GetFullPath().ToStdString(),
                                          reporter.GetMessages().ToStdString() ) );
        return tl::unexpected( e );
    }

    std::unique_ptr<BOARD_NETLIST_UPDATER> updater = ctx->MakeNetlistUpdater();

    updater->SetReporter( &reporter );
    updater->SetIsDryRun( aCtx.Request.dry_run() );
    updater->SetLookupByTimestamp( lookupByTimestamp );
    updater->SetDeleteUnusedFootprints( aCtx.Request.delete_extra_footprints() );
    updater->SetReplaceFootprints( aCtx.Request.update_footprints() );
    updater->SetTransferGroups( aCtx.Request.transfer_groups() );
    updater->SetOverrideLocks( aCtx.Request.override_locks() );
    updater->SetUpdateFields( !aCtx.Request.has_update_fields() || aCtx.Request.update_fields() );
    updater->SetRemoveExtraFields( aCtx.Request.remove_extra_fields() );

    const bool success = updater->UpdateNetlist( netlist );

    if( !aCtx.Request.dry_run() && success )
    {
        ctx->OnNetlistChanged( *updater );
        bumpRevision();
    }

    ImportNetlistResponse response;
    response.set_report( reporter.GetMessages().ToUTF8() );
    response.set_error_count( updater->GetErrorCount() );
    response.set_warning_count( updater->GetWarningCount() );
    response.set_new_footprint_count( updater->GetNewFootprintCount() );
    return response;
}


HANDLER_RESULT<BoardEditorAppearanceSettings> API_HANDLER_PCB::handleGetBoardEditorAppearanceSettings(
        const HANDLER_CONTEXT<GetBoardEditorAppearanceSettings>& aCtx )
{
    if( std::optional<ApiResponseStatus> headless = checkForHeadless( "GetBoardEditorAppearanceSettings" ) )
        return tl::unexpected( *headless );

    BoardEditorAppearanceSettings reply;

    // TODO: might be nice to put all these things in one place and have it derive SERIALIZABLE

    const PCB_DISPLAY_OPTIONS& displayOptions = frame()->GetDisplayOptions();

    reply.set_inactive_layer_display( ToProtoEnum<HIGH_CONTRAST_MODE, InactiveLayerDisplayMode>(
            displayOptions.m_ContrastModeDisplay ) );
    reply.set_net_color_display(
            ToProtoEnum<NET_COLOR_MODE, NetColorDisplayMode>( displayOptions.m_NetColorMode ) );

    reply.set_board_flip( frame()->GetCanvas()->GetView()->IsMirroredX()
                                  ? BoardFlipMode::BFM_FLIPPED_X
                                  : BoardFlipMode::BFM_NORMAL );

    PCBNEW_SETTINGS* editorSettings = frame()->GetPcbNewSettings();

    reply.set_ratsnest_display( ToProtoEnum<RATSNEST_MODE, RatsnestDisplayMode>(
            editorSettings->m_Display.m_RatsnestMode ) );

    return reply;
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleSetBoardEditorAppearanceSettings(
        const HANDLER_CONTEXT<SetBoardEditorAppearanceSettings>& aCtx )
{
    if( std::optional<ApiResponseStatus> headless = checkForHeadless( "SetBoardEditorAppearanceSettings" ) )
        return tl::unexpected( *headless );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    PCB_DISPLAY_OPTIONS options = frame()->GetDisplayOptions();
    PCBNEW_SETTINGS* editorSettings = frame()->GetPcbNewSettings();
    const BoardEditorAppearanceSettings& newSettings = aCtx.Request.settings();

    options.m_ContrastModeDisplay =
            FromProtoEnum<HIGH_CONTRAST_MODE>( newSettings.inactive_layer_display() );
    options.m_NetColorMode =
            FromProtoEnum<NET_COLOR_MODE>( newSettings.net_color_display() );
    options.m_FlipBoardView = newSettings.board_flip() == BoardFlipMode::BFM_FLIPPED_X;

    editorSettings->m_Display.m_RatsnestMode =
            FromProtoEnum<RATSNEST_MODE>( newSettings.ratsnest_display() );

    frame()->SetDisplayOptions( options );
    frame()->GetCanvas()->GetView()->UpdateAllLayersColor();
    frame()->GetCanvas()->Refresh();

    return Empty();
}


HANDLER_RESULT<BoardPlotSettingsResponse>
API_HANDLER_PCB::handleGetBoardPlotSettings( const HANDLER_CONTEXT<GetBoardPlotSettings>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    const PCB_PLOT_PARAMS& plotOpts = board()->GetPlotOptions();

    BoardPlotSettingsResponse response;
    BoardPlotSettings*        settings = response.mutable_plot_settings();

    board::PackLayerSet( *settings->mutable_layers(), plotOpts.GetLayerSelection() );

    for( PCB_LAYER_ID layer : plotOpts.GetPlotOnAllLayersSequence() )
        settings->add_common_layers( ToProtoEnum<PCB_LAYER_ID, board::types::BoardLayer>( layer ) );

    settings->set_mirror( plotOpts.GetMirror() );
    settings->set_black_and_white( plotOpts.GetBlackAndWhite() );
    settings->set_negative( plotOpts.GetNegative() );
    settings->set_scale( plotOpts.GetScale() );

    settings->set_sketch_pads_on_fab_layers( plotOpts.GetSketchPadsOnFabLayers() );
    settings->set_hide_dnp_footprints_on_fab_layers( plotOpts.GetHideDNPFPsOnFabLayers() );
    settings->set_sketch_dnp_footprints_on_fab_layers( plotOpts.GetSketchDNPFPsOnFabLayers() );
    settings->set_crossout_dnp_footprints_on_fab_layers( plotOpts.GetCrossoutDNPFPsOnFabLayers() );

    settings->set_plot_footprint_values( plotOpts.GetPlotValue() );
    settings->set_plot_reference_designators( plotOpts.GetPlotReference() );
    settings->set_plot_drawing_sheet( plotOpts.GetPlotFrameRef() );
    settings->set_subtract_solder_mask_from_silk( plotOpts.GetSubtractMaskFromSilk() );
    settings->set_plot_pad_numbers( plotOpts.GetPlotPadNumbers() );

    settings->set_drill_marks( ToProtoEnum<DRILL_MARKS, PlotDrillMarks>( plotOpts.GetDrillMarksType() ) );
    settings->set_use_drill_origin( plotOpts.GetUseAuxOrigin() );

    return response;
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleSetBoardPlotSettings( const HANDLER_CONTEXT<SetBoardPlotSettings>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    const BoardPlotSettings& settings = aCtx.Request.plot_settings();
    PCB_PLOT_PARAMS          plotOpts = board()->GetPlotOptions();

    plotOpts.SetLayerSelection( board::UnpackLayerSet( settings.layers() ) );

    LSEQ commonLayers;

    for( int layer : settings.common_layers() )
    {
        PCB_LAYER_ID layerId =
                FromProtoEnum<PCB_LAYER_ID, board::types::BoardLayer>( static_cast<board::types::BoardLayer>( layer ) );

        if( !IsPcbLayer( layerId ) )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( fmt::format( "SetBoardPlotSettings contains an invalid layer {}",
                                              magic_enum::enum_name( layerId ) ) );
            return tl::unexpected( e );
        }

        commonLayers.push_back( layerId );
    }

    plotOpts.SetPlotOnAllLayersSequence( commonLayers );

    plotOpts.SetMirror( settings.mirror() );
    plotOpts.SetBlackAndWhite( settings.black_and_white() );
    plotOpts.SetNegative( settings.negative() );
    plotOpts.SetScale( settings.scale() );

    plotOpts.SetSketchPadsOnFabLayers( settings.sketch_pads_on_fab_layers() );
    plotOpts.SetHideDNPFPsOnFabLayers( settings.hide_dnp_footprints_on_fab_layers() );
    plotOpts.SetSketchDNPFPsOnFabLayers( settings.sketch_dnp_footprints_on_fab_layers() );
    plotOpts.SetCrossoutDNPFPsOnFabLayers( settings.crossout_dnp_footprints_on_fab_layers() );

    plotOpts.SetPlotValue( settings.plot_footprint_values() );
    plotOpts.SetPlotReference( settings.plot_reference_designators() );
    plotOpts.SetPlotFrameRef( settings.plot_drawing_sheet() );
    plotOpts.SetSubtractMaskFromSilk( settings.subtract_solder_mask_from_silk() );
    plotOpts.SetPlotPadNumbers( settings.plot_pad_numbers() );

    plotOpts.SetDrillMarksType( FromProtoEnum<DRILL_MARKS>( settings.drill_marks() ) );
    plotOpts.SetUseAuxOrigin( settings.use_drill_origin() );

    board()->SetPlotOptions( plotOpts );

    if( frame() )
        frame()->OnModify();

    bumpRevision();
    return Empty();
}


void API_HANDLER_PCB::collectDrcMarkers( DrcResultsResponse& aResponse ) const
{
    uint32_t errors = 0, warnings = 0, exclusions = 0, unconnected = 0, parity = 0;

    for( const PCB_MARKER* marker : board()->Markers() )
    {
        if( !marker->GetRCItem() )
            continue;

        google::protobuf::Any any;
        marker->Serialize( any );

        board::DrcMarker* msg = aResponse.add_markers();
        any.UnpackTo( msg );

        SEVERITY severity = marker->GetSeverity();

        msg->mutable_id()->set_value( marker->m_Uuid.AsStdString() );
        msg->set_severity( ToProtoEnum<SEVERITY, types::RuleSeverity>( severity ) );
        msg->set_excluded( marker->IsExcluded() );
        msg->set_exclusion_comment( marker->GetComment().ToUTF8() );
        msg->set_description( marker->GetRCItem()->GetErrorMessage( true ).ToUTF8() );

        switch( severity )
        {
        case RPT_SEVERITY_ERROR:     ++errors;     break;
        case RPT_SEVERITY_WARNING:   ++warnings;   break;
        case RPT_SEVERITY_EXCLUSION: ++exclusions; break;
        default:                                   break;
        }

        if( marker->GetMarkerType() == MARKER_BASE::MARKER_RATSNEST )
            ++unconnected;
        else if( marker->GetMarkerType() == MARKER_BASE::MARKER_PARITY )
            ++parity;
    }

    aResponse.set_error_count( errors );
    aResponse.set_warning_count( warnings );
    aResponse.set_exclusion_count( exclusions );
    aResponse.set_unconnected_count( unconnected );
    aResponse.set_parity_count( parity );
}


HANDLER_RESULT<DrcResultsResponse> API_HANDLER_PCB::handleGetDrcMarkers( const HANDLER_CONTEXT<GetDrcMarkers>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    DrcResultsResponse response;
    collectDrcMarkers( response );
    return response;
}


HANDLER_RESULT<DrcResultsResponse> API_HANDLER_PCB::handleRunBoardJobDrc( const HANDLER_CONTEXT<RunBoardJobDrc>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    // Markers are rebuilt from scratch, which would leave staged changes pointing at freed items
    for( const auto& [client, commit] : m_commits )
    {
        if( commit.second && !commit.second->Empty() )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BUSY );
            e.set_error_message( fmt::format( "cannot run DRC while client '{}' has uncommitted changes", client ) );
            return tl::unexpected( e );
        }
    }

    // An asynchronous job runs on the registry's worker thread and shares this document's
    // PROJECT, its footprint library adapter and the KiCad thread pool with the checker, which
    // then rebuilds the board's markers underneath it.  Let the queue drain first, the same way
    // a synchronous job and IFACE::closeCurrentDocument do.
    API_JOB_REGISTRY::Instance().WaitForIdle();

    BOARD*                      brd = board();
    std::shared_ptr<DRC_ENGINE> drcEngine = brd->GetDesignSettings().m_DRCEngine;

    if( !drcEngine )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNKNOWN );
        e.set_error_message( "the board has no DRC engine" );
        return tl::unexpected( e );
    }

    // Running DRC requires libraries be loaded, so make sure they have been
    FOOTPRINT_LIBRARY_ADAPTER* adapter = PROJECT_PCB::FootprintLibAdapter( brd->GetProject() );
    adapter->AsyncLoad();
    adapter->BlockUntilLoaded();

    // Schematic parity: from an explicit netlist file, or by netlisting the project's schematic
    std::unique_ptr<NETLIST> netlist;
    bool                     checkParity = aCtx.Request.test_footprints_against_schematic();

    if( checkParity )
    {
        std::string netlistStr;
        wxString    netlistPath = wxString::FromUTF8( aCtx.Request.schematic_netlist_path() );

        if( !netlistPath.IsEmpty() )
        {
            wxFileName fn( project().AbsolutePath( netlistPath ) );

            if( !fn.FileExists() )
            {
                ApiResponseStatus e;
                e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                e.set_error_message( fmt::format( "netlist file '{}' does not exist",
                                                  fn.GetFullPath().ToStdString() ) );
                return tl::unexpected( e );
            }

            wxFFile file( fn.GetFullPath(), wxS( "rb" ) );
            wxString contents;

            if( !file.IsOpened() || !file.ReadAll( &contents ) )
            {
                ApiResponseStatus e;
                e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                e.set_error_message( fmt::format( "could not read netlist file '{}'",
                                                  fn.GetFullPath().ToStdString() ) );
                return tl::unexpected( e );
            }

            netlistStr = contents.ToStdString();
        }
        else
        {
            KIWAY* kiway = pcbContext()->GetKiway();

            if( kiway && kiway->Player( FRAME_SCH, false ) )
            {
                kiway->ExpressMail( FRAME_SCH, MAIL_SCH_GET_NETLIST, netlistStr );
            }
            else if( kiway )
            {
                wxFileName schematicPath( brd->GetFileName() );
                schematicPath.MakeAbsolute();
                schematicPath.SetExt( FILEEXT::KiCadSchematicFileExtension );

                if( !schematicPath.Exists() )
                {
                    ApiResponseStatus e;
                    e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                    e.set_error_message( "schematic parity requires a schematic next to the board or "
                                         "schematic_netlist_path" );
                    return tl::unexpected( e );
                }

                typedef bool ( *NETLIST_FN_PTR )( const wxString&, std::string& );
                KIFACE*        eeschema = kiway->KiFACE( KIWAY::FACE_SCH );
                NETLIST_FN_PTR netlister = (NETLIST_FN_PTR) eeschema->IfaceOrAddress( KIFACE_NETLIST_SCHEMATIC );
                ( *netlister )( schematicPath.GetFullPath(), netlistStr );
            }
            else
            {
                ApiResponseStatus e;
                e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                e.set_error_message( "schematic parity requires schematic_netlist_path in this context" );
                return tl::unexpected( e );
            }
        }

        if( netlistStr.empty() || netlistStr == MAIL_SCH_GET_NETLIST_CANCELLED )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( "could not obtain a schematic netlist for the parity checks "
                                 "(the schematic must be fully annotated)" );
            return tl::unexpected( e );
        }

        try
        {
            netlist = std::make_unique<NETLIST>();
            STRING_LINE_READER*  lineReader = new STRING_LINE_READER( netlistStr, _( "Eeschema netlist" ) );
            KICAD_NETLIST_READER netlistReader( lineReader, netlist.get() );

            netlistReader.LoadNetlist();
        }
        catch( const IO_ERROR& ioe )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( fmt::format( "could not parse the schematic netlist: {}",
                                              ioe.What().ToStdString() ) );
            return tl::unexpected( e );
        }

        drcEngine->SetSchematicNetlist( netlist.get() );
    }

    if( aCtx.Request.refill_zones() )
    {
        TOOL_MANAGER* mgr = toolManager();

        if( !mgr->FindTool( ZONE_FILLER_TOOL_NAME ) )
            mgr->RegisterTool( new ZONE_FILLER_TOOL );

        mgr->GetTool<ZONE_FILLER_TOOL>()->FillAllZones( nullptr, nullptr, true );
    }

    // The drawing sheet proxy lets the text-variable checks see the sheet, as the CLI job does
    std::unique_ptr<DS_PROXY_VIEW_ITEM> drawingSheet = std::make_unique<DS_PROXY_VIEW_ITEM>(
            pcbIUScale, &brd->GetPageSettings(), brd->GetProject(), &brd->GetTitleBlock(), &brd->GetProperties() );

    drawingSheet->SetSheetName( std::string() );
    drawingSheet->SetSheetPath( std::string() );
    drawingSheet->SetIsFirstPage( true );
    drawingSheet->SetFileName( TO_UTF8( brd->GetFileName() ) );

    wxString currentVariant = brd->GetCurrentVariant();
    drawingSheet->SetVariantName( TO_UTF8( currentVariant ) );
    drawingSheet->SetVariantDesc( TO_UTF8( brd->GetVariantDescription( currentVariant ) ) );

    drcEngine->SetDrawingSheet( drawingSheet.get() );
    drcEngine->SetProgressReporter( nullptr );

    BOARD_COMMIT commit( toolManager() );

    drcEngine->SetViolationHandler(
            [&]( const std::shared_ptr<DRC_ITEM>& aItem, const VECTOR2I& aPos, int aLayer,
                 const std::function<void( PCB_MARKER* )>& aPathGenerator )
            {
                PCB_MARKER* marker = new PCB_MARKER( aItem, aPos, aLayer );
                aPathGenerator( marker );
                commit.Add( marker );
            } );

    brd->RecordDRCExclusions();
    brd->DeleteMARKERs( true, true );
    drcEngine->RunTests( EDA_UNITS::MM, aCtx.Request.report_all_track_errors(), checkParity );
    drcEngine->ClearViolationHandler();
    drcEngine->SetDrawingSheet( nullptr );
    drcEngine->SetSchematicNetlist( nullptr );

    commit.Push( _( "DRC" ), SKIP_UNDO | SKIP_SET_DIRTY );

    // Update the exclusion status on any excluded markers that still exist.
    brd->ResolveDRCExclusions( false );

    bumpRevision();

    DrcResultsResponse response;
    collectDrcMarkers( response );
    return response;
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleSetDrcMarkerExcluded( const HANDLER_CONTEXT<SetDrcMarkerExcluded>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    std::vector<PCB_MARKER*> markers;

    for( const types::KIID& id : aCtx.Request.markers() )
    {
        KIID        kiid( id.value() );
        PCB_MARKER* found = nullptr;

        for( PCB_MARKER* marker : board()->Markers() )
        {
            if( marker->m_Uuid == kiid )
            {
                found = marker;
                break;
            }
        }

        if( !found )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( fmt::format( "marker {} not found on the board", id.value() ) );
            return tl::unexpected( e );
        }

        markers.push_back( found );
    }

    wxString comment = wxString::FromUTF8( aCtx.Request.comment() );

    for( PCB_MARKER* marker : markers )
    {
        marker->SetExcluded( aCtx.Request.excluded(), aCtx.Request.excluded() ? comment : wxString() );

        if( frame() && frame()->GetCanvas() )
            frame()->GetCanvas()->GetView()->Update( marker );
    }

    board()->RecordDRCExclusions();
    bumpRevision();

    return Empty();
}


DrcSeveritiesResponse API_HANDLER_PCB::drcSeverities() const
{
    DrcSeveritiesResponse  response;
    BOARD_DESIGN_SETTINGS& bds = board()->GetDesignSettings();

    for( const RC_ITEM& item : DRC_ITEM::GetItemsWithSeverities() )
    {
        PCB_DRC_CODE code = static_cast<PCB_DRC_CODE>( item.GetErrorCode() );

        // That list is what the Board Setup severities panel draws, so it carries the section
        // headings too.  They have no error code and no severity to report.
        if( code == 0 )
            continue;

        board::DrcSeveritySetting* setting = response.add_severities();
        setting->set_rule_type( ToProtoEnum<PCB_DRC_CODE, board::DrcErrorType>( code ) );
        setting->set_severity( ToProtoEnum<SEVERITY, types::RuleSeverity>( bds.GetSeverity( code ) ) );
    }

    return response;
}


HANDLER_RESULT<DrcSeveritiesResponse> API_HANDLER_PCB::handleGetDrcSeverities(
        const HANDLER_CONTEXT<GetDrcSeverities>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    return drcSeverities();
}


HANDLER_RESULT<DrcSeveritiesResponse> API_HANDLER_PCB::handleSetDrcSeverities(
        const HANDLER_CONTEXT<SetDrcSeverities>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    const std::unordered_set<SEVERITY> permitted( { RPT_SEVERITY_ERROR, RPT_SEVERITY_WARNING, RPT_SEVERITY_IGNORE } );
    std::map<int, SEVERITY>            changes;

    for( const board::DrcSeveritySetting& setting : aCtx.Request.severities() )
    {
        PCB_DRC_CODE code = FromProtoEnum<PCB_DRC_CODE, board::DrcErrorType>( setting.rule_type() );
        SEVERITY     severity = FromProtoEnum<SEVERITY, types::RuleSeverity>( setting.severity() );

        if( setting.rule_type() == board::DRCET_UNKNOWN || !permitted.contains( severity ) )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( "DRC severities need a valid rule type and a severity of error, warning, "
                                 "or ignore" );
            return tl::unexpected( e );
        }

        changes[code] = severity;
    }

    BOARD_DESIGN_SETTINGS& bds = board()->GetDesignSettings();

    for( const auto& [code, severity] : changes )
        bds.m_DRCSeverities[code] = severity;

    if( !changes.empty() )
        bumpRevision();

    publishProjectChanged( kiapi::common::events::PCK_SETTINGS, aCtx.ClientName );

    return drcSeverities();
}


HANDLER_RESULT<InjectDrcErrorResponse> API_HANDLER_PCB::handleInjectDrcError(
        const HANDLER_CONTEXT<InjectDrcError>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    SEVERITY severity = FromProtoEnum<SEVERITY>( aCtx.Request.severity() );
    int      layer = severity == RPT_SEVERITY_WARNING ? LAYER_DRC_WARNING : LAYER_DRC_ERROR;
    int      code = severity == RPT_SEVERITY_WARNING ? DRCE_GENERIC_WARNING : DRCE_GENERIC_ERROR;

    std::shared_ptr<DRC_ITEM> drcItem = DRC_ITEM::Create( code );

    drcItem->SetErrorMessage( wxString::FromUTF8( aCtx.Request.message() ) );

    RC_ITEM::KIIDS ids;

    for( const auto& id : aCtx.Request.items() )
        ids.emplace_back( KIID( id.value() ) );

    if( !ids.empty() )
        drcItem->SetItems( ids );

    const auto& pos = aCtx.Request.position();
    VECTOR2I    position( static_cast<int>( pos.x_nm() ), static_cast<int>( pos.y_nm() ) );

    PCB_MARKER* marker = new PCB_MARKER( drcItem, position, layer );

    getCurrentCommit( aCtx.ClientName )->Add( marker );
    pushCurrentCommit( aCtx.ClientName, wxS( "API injected DRC marker" ) );

    InjectDrcErrorResponse response;
    response.mutable_marker()->set_value( marker->GetUUID().AsStdString() );

    return response;
}


std::optional<ApiResponseStatus> ValidateUnitsInchMm( types::Units aUnits,
                                                      const std::string& aCommandName )
{
    if( aUnits == types::Units::U_INCH || aUnits == types::Units::U_MM
        || aUnits == types::Units::U_UNKNOWN )
    {
        return std::nullopt;
    }

    ApiResponseStatus e;
    e.set_status( ApiStatusCode::AS_BAD_REQUEST );
    e.set_error_message( fmt::format( "{} supports only inch and mm units", aCommandName ) );
    return e;
}


std::optional<ApiResponseStatus>
ValidatePaginationModeForSingleOrPerFile( kiapi::board::jobs::BoardJobPaginationMode aMode,
                                          const std::string& aCommandName )
{
    if( aMode == kiapi::board::jobs::BoardJobPaginationMode::BJPM_UNKNOWN
        || aMode == kiapi::board::jobs::BoardJobPaginationMode::BJPM_ALL_LAYERS_ONE_PAGE
        || aMode == kiapi::board::jobs::BoardJobPaginationMode::BJPM_EACH_LAYER_OWN_FILE )
    {
        return std::nullopt;
    }

    ApiResponseStatus e;
    e.set_status( ApiStatusCode::AS_BAD_REQUEST );
    e.set_error_message( fmt::format( "{} does not support EACH_LAYER_OWN_PAGE pagination mode",
                                      aCommandName ) );
    return e;
}


std::optional<ApiResponseStatus> ApplyBoardPlotSettings( const BoardPlotSettings& aSettings,
                                                         JOB_EXPORT_PCB_PLOT& aJob )
{
    for( int layer : aSettings.layers() )
    {
        PCB_LAYER_ID layerId = FromProtoEnum<PCB_LAYER_ID, board::types::BoardLayer>(
                static_cast<board::types::BoardLayer>( layer ) );

        if( layerId == PCB_LAYER_ID::UNDEFINED_LAYER )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( "Board plot settings contain an invalid layer" );
            return e;
        }

        aJob.m_plotLayerSequence.push_back( layerId );
    }

    for( int layer : aSettings.common_layers() )
    {
        PCB_LAYER_ID layerId = FromProtoEnum<PCB_LAYER_ID, board::types::BoardLayer>(
                static_cast<board::types::BoardLayer>( layer ) );

        if( layerId == PCB_LAYER_ID::UNDEFINED_LAYER )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( "Board plot settings contain an invalid common layer" );
            return e;
        }

        aJob.m_plotOnAllLayersSequence.push_back( layerId );
    }

    aJob.m_colorTheme = wxString::FromUTF8( aSettings.color_theme() );
    aJob.m_drawingSheet = wxString::FromUTF8( aSettings.drawing_sheet() );
    aJob.m_variant = wxString::FromUTF8( aSettings.variant() );

    aJob.m_mirror = aSettings.mirror();
    aJob.m_blackAndWhite = aSettings.black_and_white();
    aJob.m_negative = aSettings.negative();
    aJob.m_scale = aSettings.scale();

    aJob.m_sketchPadsOnFabLayers = aSettings.sketch_pads_on_fab_layers();
    aJob.m_hideDNPFPsOnFabLayers = aSettings.hide_dnp_footprints_on_fab_layers();
    aJob.m_sketchDNPFPsOnFabLayers = aSettings.sketch_dnp_footprints_on_fab_layers();
    aJob.m_crossoutDNPFPsOnFabLayers = aSettings.crossout_dnp_footprints_on_fab_layers();

    aJob.m_plotFootprintValues = aSettings.plot_footprint_values();
    aJob.m_plotRefDes = aSettings.plot_reference_designators();
    aJob.m_plotDrawingSheet = aSettings.plot_drawing_sheet();
    aJob.m_subtractSolderMaskFromSilk = aSettings.subtract_solder_mask_from_silk();
    aJob.m_plotPadNumbers = aSettings.plot_pad_numbers();

    aJob.m_drillShapeOption = FromProtoEnum<DRILL_MARKS>( aSettings.drill_marks() );

    aJob.m_useDrillOrigin = aSettings.use_drill_origin();
    aJob.m_checkZonesBeforePlot = aSettings.check_zones_before_plot();

    return std::nullopt;
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::runBoardJob( const types::RunJobSettings& aSettings,
                                                                    std::unique_ptr<JOB> aJob )
{
    if( !pcbContext()->GetKiway() )
    {
        types::RunJobResponse response;
        response.set_status( types::JobStatus::JS_ERROR );
        response.set_message( "Internal error" );
        wxCHECK_MSG( false, response, "context missing valid kiway in runBoardJob?" );
        return response;
    }

    // The jobs handler reads an editor window's board directly, so only headless jobs may leave
    // the main thread
    bool async = aSettings.async() && !m_frame;

    return RunApiJob( Server(), pcbContext()->GetKiway(), KIWAY::FACE_PCB, std::move( aJob ), async,
                      aSettings.return_inline() );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExport3D(
        const HANDLER_CONTEXT<RunBoardJobExport3D>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.job_settings().document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    std::unique_ptr<JOB_EXPORT_PCB_3D> jobPtr = std::make_unique<JOB_EXPORT_PCB_3D>();
    JOB_EXPORT_PCB_3D&                job = *jobPtr;
    job.m_filename = pcbContext()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    job.m_format = FromProtoEnum<JOB_EXPORT_PCB_3D::FORMAT>( aCtx.Request.format() );

    job.m_variant = wxString::FromUTF8( aCtx.Request.variant() );
    job.m_3dparams.m_NetFilter = wxString::FromUTF8( aCtx.Request.net_filter() );
    job.m_3dparams.m_ComponentFilter = wxString::FromUTF8( aCtx.Request.component_filter() );

    job.m_hasUserOrigin = aCtx.Request.has_user_origin();
    job.m_3dparams.m_Origin = VECTOR2D( aCtx.Request.origin().x_nm(), aCtx.Request.origin().y_nm() );

    job.m_3dparams.m_Overwrite = aCtx.Request.overwrite();
    job.m_3dparams.m_UseGridOrigin = aCtx.Request.use_grid_origin();
    job.m_3dparams.m_UseDrillOrigin = aCtx.Request.use_drill_origin();
    job.m_3dparams.m_UseDefinedOrigin = aCtx.Request.use_defined_origin() || aCtx.Request.has_user_origin();
    job.m_3dparams.m_UsePcbCenterOrigin = aCtx.Request.use_pcb_center_origin();

    job.m_3dparams.m_IncludeUnspecified = aCtx.Request.include_unspecified();
    job.m_3dparams.m_IncludeDNP = aCtx.Request.include_dnp();
    job.m_3dparams.m_SubstModels = aCtx.Request.substitute_models();

    job.m_3dparams.m_BoardOutlinesChainingEpsilon = aCtx.Request.board_outlines_chaining_epsilon();
    job.m_3dparams.m_BoardOnly = aCtx.Request.board_only();
    job.m_3dparams.m_CutViasInBody = aCtx.Request.cut_vias_in_body();
    job.m_3dparams.m_ExportBoardBody = aCtx.Request.export_board_body();
    job.m_3dparams.m_ExportComponents = aCtx.Request.export_components();
    job.m_3dparams.m_ExportTracksVias = aCtx.Request.export_tracks_and_vias();
    job.m_3dparams.m_ExportPads = aCtx.Request.export_pads();
    job.m_3dparams.m_ExportZones = aCtx.Request.export_zones();
    job.m_3dparams.m_ExportInnerCopper = aCtx.Request.export_inner_copper();
    job.m_3dparams.m_ExportSilkscreen = aCtx.Request.export_silkscreen();
    job.m_3dparams.m_ExportSoldermask = aCtx.Request.export_soldermask();
    job.m_3dparams.m_FuseShapes = aCtx.Request.fuse_shapes();
    job.m_3dparams.m_FillAllVias = aCtx.Request.fill_all_vias();
    job.m_3dparams.m_OptimizeStep = aCtx.Request.optimize_step();
    job.m_3dparams.m_ExtraPadThickness = aCtx.Request.extra_pad_thickness();

    job.m_vrmlUnits = FromProtoEnum<JOB_EXPORT_PCB_3D::VRML_UNITS>( aCtx.Request.vrml_units() );

    job.m_vrmlModelDir = wxString::FromUTF8( aCtx.Request.vrml_model_dir() );
    job.m_vrmlRelativePaths = aCtx.Request.vrml_relative_paths();

    return runBoardJob( aCtx.Request.job_settings(), std::move( jobPtr ) );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExportRender(
        const HANDLER_CONTEXT<RunBoardJobExportRender>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.job_settings().document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    std::unique_ptr<JOB_PCB_RENDER> jobPtr = std::make_unique<JOB_PCB_RENDER>();
    JOB_PCB_RENDER&                job = *jobPtr;
    job.m_filename = pcbContext()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    job.m_format = FromProtoEnum<JOB_PCB_RENDER::FORMAT>( aCtx.Request.format() );
    job.m_quality = FromProtoEnum<JOB_PCB_RENDER::QUALITY>( aCtx.Request.quality() );
    job.m_bgStyle = FromProtoEnum<JOB_PCB_RENDER::BG_STYLE>( aCtx.Request.background_style() );

    job.m_width = aCtx.Request.width();
    job.m_height = aCtx.Request.height();
    job.m_appearancePreset = aCtx.Request.appearance_preset();
    job.m_useBoardStackupColors = aCtx.Request.use_board_stackup_colors();

    job.m_side = FromProtoEnum<JOB_PCB_RENDER::SIDE>( aCtx.Request.side() );

    job.m_zoom = aCtx.Request.zoom();
    job.m_perspective = aCtx.Request.perspective();

    job.m_rotation = UnpackVector3D( aCtx.Request.rotation() );
    job.m_pan = UnpackVector3D( aCtx.Request.pan() );
    job.m_pivot = UnpackVector3D( aCtx.Request.pivot() );

    job.m_proceduralTextures = aCtx.Request.procedural_textures();
    job.m_floor = aCtx.Request.floor();
    job.m_antiAlias = aCtx.Request.anti_alias();
    job.m_postProcess = aCtx.Request.post_process();

    job.m_lightTopIntensity = UnpackVector3D( aCtx.Request.light_top_intensity() );
    job.m_lightBottomIntensity = UnpackVector3D( aCtx.Request.light_bottom_intensity() );
    job.m_lightCameraIntensity = UnpackVector3D( aCtx.Request.light_camera_intensity() );
    job.m_lightSideIntensity = UnpackVector3D( aCtx.Request.light_side_intensity() );
    job.m_lightSideElevation = aCtx.Request.light_side_elevation();

    return runBoardJob( aCtx.Request.job_settings(), std::move( jobPtr ) );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExportSvg(
        const HANDLER_CONTEXT<RunBoardJobExportSvg>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.job_settings().document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    std::unique_ptr<JOB_EXPORT_PCB_SVG> jobPtr = std::make_unique<JOB_EXPORT_PCB_SVG>();
    JOB_EXPORT_PCB_SVG&                job = *jobPtr;
    job.m_filename = pcbContext()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    if( std::optional<ApiResponseStatus> err = ApplyBoardPlotSettings( aCtx.Request.plot_settings(), job ) )
        return tl::unexpected( *err );

    job.m_fitPageToBoard = aCtx.Request.fit_page_to_board();
    job.m_precision = aCtx.Request.precision();

        if( std::optional<ApiResponseStatus> paginationError =
            ValidatePaginationModeForSingleOrPerFile( aCtx.Request.page_mode(),
                                                      "RunBoardJobExportSvg" ) )
    {
        return tl::unexpected( *paginationError );
    }

    job.m_genMode = FromProtoEnum<JOB_EXPORT_PCB_SVG::GEN_MODE>( aCtx.Request.page_mode() );

    return runBoardJob( aCtx.Request.job_settings(), std::move( jobPtr ) );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExportDxf(
        const HANDLER_CONTEXT<RunBoardJobExportDxf>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.job_settings().document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    std::unique_ptr<JOB_EXPORT_PCB_DXF> jobPtr = std::make_unique<JOB_EXPORT_PCB_DXF>();
    JOB_EXPORT_PCB_DXF&                job = *jobPtr;
    job.m_filename = pcbContext()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    if( std::optional<ApiResponseStatus> err = ApplyBoardPlotSettings( aCtx.Request.plot_settings(), job ) )
        return tl::unexpected( *err );

    job.m_plotGraphicItemsUsingContours = aCtx.Request.plot_graphic_items_using_contours();
    job.m_polygonMode = aCtx.Request.polygon_mode();

    if( std::optional<ApiResponseStatus> unitError =
            ValidateUnitsInchMm( aCtx.Request.units(), "RunBoardJobExportDxf" ) )
    {
        return tl::unexpected( *unitError );
    }

    job.m_dxfUnits = FromProtoEnum<JOB_EXPORT_PCB_DXF::DXF_UNITS>( aCtx.Request.units() );

        if( std::optional<ApiResponseStatus> paginationError =
            ValidatePaginationModeForSingleOrPerFile( aCtx.Request.page_mode(),
                                                      "RunBoardJobExportDxf" ) )
    {
        return tl::unexpected( *paginationError );
    }

    job.m_genMode = FromProtoEnum<JOB_EXPORT_PCB_DXF::GEN_MODE>( aCtx.Request.page_mode() );

    return runBoardJob( aCtx.Request.job_settings(), std::move( jobPtr ) );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExportPdf(
        const HANDLER_CONTEXT<RunBoardJobExportPdf>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.job_settings().document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    std::unique_ptr<JOB_EXPORT_PCB_PDF> jobPtr = std::make_unique<JOB_EXPORT_PCB_PDF>();
    JOB_EXPORT_PCB_PDF&                job = *jobPtr;
    job.m_filename = pcbContext()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    if( std::optional<ApiResponseStatus> err = ApplyBoardPlotSettings( aCtx.Request.plot_settings(), job ) )
        return tl::unexpected( *err );

    job.m_pdfFrontFPPropertyPopups = aCtx.Request.front_footprint_property_popups();
    job.m_pdfBackFPPropertyPopups = aCtx.Request.back_footprint_property_popups();
    job.m_pdfMetadata = aCtx.Request.include_metadata();
    job.m_pdfSingle = aCtx.Request.single_document();
    job.m_pdfBackgroundColor = wxString::FromUTF8( aCtx.Request.background_color() );

    job.m_pdfGenMode = FromProtoEnum<JOB_EXPORT_PCB_PDF::GEN_MODE>( aCtx.Request.page_mode() );

    return runBoardJob( aCtx.Request.job_settings(), std::move( jobPtr ) );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExportPs(
        const HANDLER_CONTEXT<RunBoardJobExportPs>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.job_settings().document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    std::unique_ptr<JOB_EXPORT_PCB_PS> jobPtr = std::make_unique<JOB_EXPORT_PCB_PS>();
    JOB_EXPORT_PCB_PS&                job = *jobPtr;
    job.m_filename = pcbContext()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    if( std::optional<ApiResponseStatus> err = ApplyBoardPlotSettings( aCtx.Request.plot_settings(), job ) )
        return tl::unexpected( *err );

    if( std::optional<ApiResponseStatus> paginationError =
            ValidatePaginationModeForSingleOrPerFile( aCtx.Request.page_mode(),
                                                      "RunBoardJobExportPs" ) )
    {
        return tl::unexpected( *paginationError );
    }

    job.m_genMode = FromProtoEnum<JOB_EXPORT_PCB_PS::GEN_MODE>( aCtx.Request.page_mode() );

    job.m_trackWidthCorrection = aCtx.Request.track_width_correction();
    job.m_XScaleAdjust = aCtx.Request.x_scale_adjust();
    job.m_YScaleAdjust = aCtx.Request.y_scale_adjust();
    job.m_forceA4 = aCtx.Request.force_a4();
    job.m_useGlobalSettings = aCtx.Request.use_global_settings();

    return runBoardJob( aCtx.Request.job_settings(), std::move( jobPtr ) );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExportGerbers(
        const HANDLER_CONTEXT<RunBoardJobExportGerbers>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.job_settings().document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    std::unique_ptr<JOB_EXPORT_PCB_GERBERS> jobPtr = std::make_unique<JOB_EXPORT_PCB_GERBERS>();
    JOB_EXPORT_PCB_GERBERS&                job = *jobPtr;
    job.m_filename = pcbContext()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    job.m_useBoardPlotParams = aCtx.Request.use_board_plot_params();

    if( !job.m_useBoardPlotParams )
    {
        if( std::optional<ApiResponseStatus> err = ApplyBoardPlotSettings( aCtx.Request.plot_settings(), job ) )
            return tl::unexpected( *err );
    }

    job.m_createJobsFile = aCtx.Request.create_gerber_job_file();
    job.m_includeNetlistAttributes = aCtx.Request.include_netlist_attributes();
    job.m_useX2Format = aCtx.Request.use_x2_format();
    job.m_disableApertureMacros = aCtx.Request.disable_aperture_macros();
    job.m_useProtelFileExtension = aCtx.Request.use_protel_file_extensions();

    switch( aCtx.Request.precision() )
    {
    default:
    case GerberPrecision::GP_5: job.m_precision = 5; break;
    case GerberPrecision::GP_6: job.m_precision = 6; break;
    }

    return runBoardJob( aCtx.Request.job_settings(), std::move( jobPtr ) );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExportDrill(
        const HANDLER_CONTEXT<RunBoardJobExportDrill>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.job_settings().document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    std::unique_ptr<JOB_EXPORT_PCB_DRILL> jobPtr = std::make_unique<JOB_EXPORT_PCB_DRILL>();
    JOB_EXPORT_PCB_DRILL&                job = *jobPtr;
    job.m_filename = pcbContext()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    job.m_format = FromProtoEnum<JOB_EXPORT_PCB_DRILL::DRILL_FORMAT>( aCtx.Request.format() );

    if( std::optional<ApiResponseStatus> unitError =
            ValidateUnitsInchMm( aCtx.Request.units(), "RunBoardJobExportDrill" ) )
    {
        return tl::unexpected( *unitError );
    }

    job.m_drillUnits = FromProtoEnum<JOB_EXPORT_PCB_DRILL::DRILL_UNITS>( aCtx.Request.units() );
    job.m_drillOrigin = FromProtoEnum<JOB_EXPORT_PCB_DRILL::DRILL_ORIGIN>( aCtx.Request.origin() );
    job.m_zeroFormat = FromProtoEnum<JOB_EXPORT_PCB_DRILL::ZEROS_FORMAT>( aCtx.Request.zeros_format() );

    if( aCtx.Request.has_excellon() )
    {
        const ExcellonFormatOptions& excellonOptions = aCtx.Request.excellon();

        if( excellonOptions.has_mirror_y() )
            job.m_excellonMirrorY = excellonOptions.mirror_y();

        if( excellonOptions.has_minimal_header() )
            job.m_excellonMinimalHeader = excellonOptions.minimal_header();

        if( excellonOptions.has_combine_pth_npth() )
            job.m_excellonCombinePTHNPTH = excellonOptions.combine_pth_npth();

        if( excellonOptions.has_route_oval_holes() )
            job.m_excellonOvalDrillRoute = excellonOptions.route_oval_holes();
    }

    if( aCtx.Request.map_format() != DrillMapFormat::DMF_UNKNOWN )
    {
        job.m_generateMap = true;
        job.m_mapFormat = FromProtoEnum<JOB_EXPORT_PCB_DRILL::MAP_FORMAT>( aCtx.Request.map_format() );
    }

    job.m_gerberPrecision = aCtx.Request.gerber_precision() == DrillGerberPrecision::DGP_4_5 ? 5 : 6;

    if( aCtx.Request.has_gerber_generate_tenting() )
        job.m_generateTenting = aCtx.Request.gerber_generate_tenting();

    if( aCtx.Request.report_format() != DrillReportFormat::DRF_UNKNOWN )
    {
        job.m_generateReport = true;

        if( aCtx.Request.has_report_filename() )
            job.m_reportPath = wxString::FromUTF8( aCtx.Request.report_filename() );
    }

    return runBoardJob( aCtx.Request.job_settings(), std::move( jobPtr ) );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExportPosition(
        const HANDLER_CONTEXT<RunBoardJobExportPosition>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.job_settings().document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    std::unique_ptr<JOB_EXPORT_PCB_POS> jobPtr = std::make_unique<JOB_EXPORT_PCB_POS>();
    JOB_EXPORT_PCB_POS&                job = *jobPtr;
    job.m_filename = pcbContext()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    if( aCtx.Request.has_use_drill_place_file_origin() )
        job.m_useDrillPlaceFileOrigin = aCtx.Request.use_drill_place_file_origin();

    job.m_smdOnly = aCtx.Request.smd_only();
    job.m_excludeFootprintsWithTh = aCtx.Request.exclude_footprints_with_th();
    job.m_excludeDNP = aCtx.Request.exclude_dnp();
    job.m_excludeBOM = aCtx.Request.exclude_from_bom();
    job.m_negateBottomX = aCtx.Request.negate_bottom_x();
    job.m_singleFile = aCtx.Request.single_file();
    job.m_nakedFilename = aCtx.Request.naked_filename();
    if( aCtx.Request.has_include_board_edge_for_gerber() )
        job.m_gerberBoardEdge = aCtx.Request.include_board_edge_for_gerber();

    job.m_variant = wxString::FromUTF8( aCtx.Request.variant() );

    job.m_side = FromProtoEnum<JOB_EXPORT_PCB_POS::SIDE>( aCtx.Request.side() );

    if( std::optional<ApiResponseStatus> unitError =
            ValidateUnitsInchMm( aCtx.Request.units(), "RunBoardJobExportPosition" ) )
    {
        return tl::unexpected( *unitError );
    }

    job.m_units = FromProtoEnum<JOB_EXPORT_PCB_POS::UNITS>( aCtx.Request.units() );
    job.m_format = FromProtoEnum<JOB_EXPORT_PCB_POS::FORMAT>( aCtx.Request.format() );

    return runBoardJob( aCtx.Request.job_settings(), std::move( jobPtr ) );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExportGencad(
        const HANDLER_CONTEXT<RunBoardJobExportGencad>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.job_settings().document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    std::unique_ptr<JOB_EXPORT_PCB_GENCAD> jobPtr = std::make_unique<JOB_EXPORT_PCB_GENCAD>();
    JOB_EXPORT_PCB_GENCAD&                job = *jobPtr;
    job.m_filename = pcbContext()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    job.m_flipBottomPads = aCtx.Request.flip_bottom_pads();
    job.m_useIndividualShapes = aCtx.Request.use_individual_shapes();
    job.m_storeOriginCoords = aCtx.Request.store_origin_coords();
    job.m_useDrillOrigin = aCtx.Request.use_drill_origin();
    job.m_useUniquePins = aCtx.Request.use_unique_pins();

    return runBoardJob( aCtx.Request.job_settings(), std::move( jobPtr ) );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExportIpc2581(
        const HANDLER_CONTEXT<RunBoardJobExportIpc2581>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.job_settings().document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    std::unique_ptr<JOB_EXPORT_PCB_IPC2581> jobPtr = std::make_unique<JOB_EXPORT_PCB_IPC2581>();
    JOB_EXPORT_PCB_IPC2581&                job = *jobPtr;
    job.m_filename = pcbContext()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    job.m_drawingSheet = wxString::FromUTF8( aCtx.Request.drawing_sheet() );
    job.m_variant = wxString::FromUTF8( aCtx.Request.variant() );
    if( aCtx.Request.has_precision() )
        job.m_precision = aCtx.Request.precision();

    job.m_compress = aCtx.Request.compress();
    job.m_colInternalId = wxString::FromUTF8( aCtx.Request.internal_id_column() );
    job.m_colMfgPn = wxString::FromUTF8( aCtx.Request.manufacturer_part_number_column() );
    job.m_colMfg = wxString::FromUTF8( aCtx.Request.manufacturer_column() );
    job.m_colDistPn = wxString::FromUTF8( aCtx.Request.distributor_part_number_column() );
    job.m_colDist = wxString::FromUTF8( aCtx.Request.distributor_column() );
    job.m_bomRev = wxString::FromUTF8( aCtx.Request.bom_revision() );

    if( std::optional<ApiResponseStatus> unitError =
            ValidateUnitsInchMm( aCtx.Request.units(), "RunBoardJobExportIpc2581" ) )
    {
        return tl::unexpected( *unitError );
    }

    job.m_units = FromProtoEnum<JOB_EXPORT_PCB_IPC2581::IPC2581_UNITS>( aCtx.Request.units() );
    job.m_version = FromProtoEnum<JOB_EXPORT_PCB_IPC2581::IPC2581_VERSION>( aCtx.Request.version() );

    return runBoardJob( aCtx.Request.job_settings(), std::move( jobPtr ) );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExportIpcD356(
        const HANDLER_CONTEXT<RunBoardJobExportIpcD356>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.job_settings().document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    std::unique_ptr<JOB_EXPORT_PCB_IPCD356> jobPtr = std::make_unique<JOB_EXPORT_PCB_IPCD356>();
    JOB_EXPORT_PCB_IPCD356&                job = *jobPtr;
    job.m_filename = pcbContext()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    return runBoardJob( aCtx.Request.job_settings(), std::move( jobPtr ) );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExportODB(
        const HANDLER_CONTEXT<RunBoardJobExportODB>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.job_settings().document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    std::unique_ptr<JOB_EXPORT_PCB_ODB> jobPtr = std::make_unique<JOB_EXPORT_PCB_ODB>();
    JOB_EXPORT_PCB_ODB&                job = *jobPtr;
    job.m_filename = pcbContext()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    job.m_drawingSheet = wxString::FromUTF8( aCtx.Request.drawing_sheet() );
    job.m_variant = wxString::FromUTF8( aCtx.Request.variant() );
    if( aCtx.Request.has_precision() )
        job.m_precision = aCtx.Request.precision();

    if( std::optional<ApiResponseStatus> unitError =
            ValidateUnitsInchMm( aCtx.Request.units(), "RunBoardJobExportODB" ) )
    {
        return tl::unexpected( *unitError );
    }

    job.m_units = FromProtoEnum<JOB_EXPORT_PCB_ODB::ODB_UNITS>( aCtx.Request.units() );
    job.m_compressionMode = FromProtoEnum<JOB_EXPORT_PCB_ODB::ODB_COMPRESSION>( aCtx.Request.compression() );

    return runBoardJob( aCtx.Request.job_settings(), std::move( jobPtr ) );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExportStats(
        const HANDLER_CONTEXT<RunBoardJobExportStats>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.job_settings().document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    std::unique_ptr<JOB_EXPORT_PCB_STATS> jobPtr = std::make_unique<JOB_EXPORT_PCB_STATS>();
    JOB_EXPORT_PCB_STATS&                job = *jobPtr;
    job.m_filename = pcbContext()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    job.m_format = FromProtoEnum<JOB_EXPORT_PCB_STATS::OUTPUT_FORMAT>( aCtx.Request.format() );

    if( std::optional<ApiResponseStatus> unitError =
            ValidateUnitsInchMm( aCtx.Request.units(), "RunBoardJobExportStats" ) )
    {
        return tl::unexpected( *unitError );
    }

    job.m_units = FromProtoEnum<JOB_EXPORT_PCB_STATS::UNITS>( aCtx.Request.units() );

    job.m_excludeFootprintsWithoutPads = aCtx.Request.exclude_footprints_without_pads();
    job.m_subtractHolesFromBoardArea = aCtx.Request.subtract_holes_from_board_area();
    job.m_subtractHolesFromCopperAreas = aCtx.Request.subtract_holes_from_copper_areas();

    return runBoardJob( aCtx.Request.job_settings(), std::move( jobPtr ) );
}


HANDLER_RESULT<CrossProbeAnnounceResponse> API_HANDLER_PCB::handleCrossProbeAnnounce(
        const HANDLER_CONTEXT<CrossProbeAnnounce>& aCtx )
{
    wxLogTrace( traceApi, "Received announce from frame %d at %s",
                aCtx.Request.frame_type(), aCtx.Request.socket_path() );

    CROSS_PROBE_CLIENT::RegisterPeer( static_cast<FRAME_T>( aCtx.Request.frame_type() ),
                                      aCtx.Request.socket_path() );

    CrossProbeAnnounceResponse response;
    response.set_status( CPS_OK );
    return response;
}


HANDLER_RESULT<SyncSelectionResponse> API_HANDLER_PCB::handleSyncSelection( const HANDLER_CONTEXT<SyncSelection>& aCtx )
{
    if( std::optional<ApiResponseStatus> headless = checkForHeadless( "SyncSelection" ) )
        return tl::unexpected( *headless );

    SyncSelectionResponse response;

    const CROSS_PROBING_SETTINGS& settings = frame()->GetPcbNewSettings()->m_CrossProbing;

    if( !settings.on_selection && aCtx.Request.context() != SyncSelectionContext::SSC_EXPLICIT )
    {
        response.set_status( CPS_DISABLED );
        response.set_message( "implicit selection sync disabled by user" );
        return response;
    }

    std::vector<BOARD_ITEM*> items =
            kiapi::board::FindItemsFromSyncSelection( board(), aCtx.Request.items() );

    frame()->m_ProbingSchToPcb = true; // recursion guard

    if( aCtx.Request.mode() == SyncSelectionMode::SSM_ITEMS_AND_NETS )
        frame()->GetToolManager()->RunAction( PCB_ACTIONS::syncSelectionWithNets, &items );
    else
        frame()->GetToolManager()->RunAction( PCB_ACTIONS::syncSelection, &items );

    // Update 3D viewer highlighting
    frame()->Update3DView( false, frame()->GetPcbNewSettings()->m_Display.m_Live3DRefresh );

    frame()->m_ProbingSchToPcb = false;

    if( settings.flash_selection )
    {
        wxLogTrace( traceCrossProbeFlash, "MAIL_SELECTION(_FORCE) PCB: flash enabled, items=%zu", items.size() );
        if( items.empty() )
        {
            wxLogTrace( traceCrossProbeFlash, "MAIL_SELECTION(_FORCE) PCB: nothing to flash" );
        }
        else
        {
            std::vector<BOARD_ITEM*> boardItems;
            std::copy( items.begin(), items.end(), std::back_inserter( boardItems ) );
            frame()->StartCrossProbeFlash( boardItems );
        }
    }
    else
    {
        wxLogTrace( traceCrossProbeFlash, "MAIL_SELECTION(_FORCE) PCB: flash disabled" );
    }

    response.set_status( CPS_OK );
    return response;
}


HANDLER_RESULT<HighlightNetsResponse> API_HANDLER_PCB::handleHighlightNets(
        const HANDLER_CONTEXT<HighlightNets>& aCtx )
{
    if( std::optional<ApiResponseStatus> headless = checkForHeadless( "HighlightNets" ) )
        return tl::unexpected( *headless );

    HighlightNetsResponse response;
    CROSS_PROBING_SETTINGS& crossProbingSettings = frame()->GetPcbNewSettings()->m_CrossProbing;

    if( aCtx.ClientName == StandaloneCrossProbeClientName
        || aCtx.ClientName == KiwayClientName )
    {
        if( !crossProbingSettings.auto_highlight )
        {
            response.set_status( CPS_DISABLED );
            response.set_message( "net highlight cross-probing disabled by user" );
            return response;
        }
    }

    std::vector<wxString> nets;

    for( const std::string& name : aCtx.Request.net_name() )
        nets.emplace_back( wxString::FromUTF8( name ) );

    frame()->HandleRemoteNetHighlight( nets );

    response.set_status( CPS_OK );
    return response;
}


HANDLER_RESULT<VariantsResponse> API_HANDLER_PCB::handleGetVariants( const HANDLER_CONTEXT<GetVariants>& aCtx )
{
    if( aCtx.Request.document().type() != DocumentType::DOCTYPE_PCB )
        return tl::unexpected( MakeResponseStatus( AS_UNHANDLED ) );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    BOARD* board = pcbContext()->GetBoard();
    VariantsResponse response;

    response.mutable_document()->CopyFrom( aCtx.Request.document() );

    for( const wxString& name : board->GetVariantNames() )
    {
        types::DesignVariant* var = response.add_variants();
        var->set_name( name.ToUTF8() );
        var->set_description( board->GetVariantDescription( name ).ToUTF8() );
    }

    return response;
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleAddVariant( const HANDLER_CONTEXT<AddVariant>& aCtx )
{
    if( aCtx.Request.document().type() != DocumentType::DOCTYPE_PCB )
        return tl::unexpected( MakeResponseStatus( AS_UNHANDLED ) );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    BOARD* board = pcbContext()->GetBoard();

    wxString name = wxString::FromUTF8( aCtx.Request.name() );

    if( name.IsEmpty() || name.CmpNoCase( GetDefaultVariantName() ) == 0 )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "'{}' is not a valid variant name", aCtx.Request.name() ) );
        return tl::unexpected( e );
    }

    if( board->HasVariant( name ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "a variant named '{}' already exists", aCtx.Request.name() ) );
        return tl::unexpected( e );
    }

    board->AddVariant( name );

    if( aCtx.Request.has_description() )
        board->SetVariantDescription( name, wxString::FromUTF8( aCtx.Request.description() ) );

    if( frame() )
        frame()->UpdateVariantSelectionCtrl();

    bumpRevision();
    publishProjectChanged( kiapi::common::events::PCK_VARIANTS, aCtx.ClientName );

    return Empty();
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleDeleteVariant( const HANDLER_CONTEXT<DeleteVariant>& aCtx )
{
    if( aCtx.Request.document().type() != DocumentType::DOCTYPE_PCB )
        return tl::unexpected( MakeResponseStatus( AS_UNHANDLED ) );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    BOARD* board = pcbContext()->GetBoard();

    wxString name = wxString::FromUTF8( aCtx.Request.name() );

    if( name.IsEmpty() || name.CmpNoCase( GetDefaultVariantName() ) == 0 )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "'{}' is not a valid variant name", aCtx.Request.name() ) );
        return tl::unexpected( e );
    }

    if( !board->HasVariant( name ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "no variant named '{}' exists", aCtx.Request.name() ) );
        return tl::unexpected( e );
    }

    board->DeleteVariant( name );

    if( frame() )
        frame()->UpdateVariantSelectionCtrl();

    bumpRevision();
    publishProjectChanged( kiapi::common::events::PCK_VARIANTS, aCtx.ClientName );

    return Empty();
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleRenameVariant( const HANDLER_CONTEXT<RenameVariant>& aCtx )
{
    if( aCtx.Request.document().type() != DocumentType::DOCTYPE_PCB )
        return tl::unexpected( MakeResponseStatus( AS_UNHANDLED ) );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    BOARD* board = pcbContext()->GetBoard();

    wxString oldName = wxString::FromUTF8( aCtx.Request.old_name() );
    wxString newName = wxString::FromUTF8( aCtx.Request.new_name() );

    if( oldName.IsEmpty() || oldName.CmpNoCase( GetDefaultVariantName() ) == 0 )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "'{}' is not a valid variant name", aCtx.Request.old_name() ) );
        return tl::unexpected( e );
    }

    if( newName.IsEmpty() || newName.CmpNoCase( GetDefaultVariantName() ) == 0 )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "'{}' is not a valid variant name", aCtx.Request.new_name() ) );
        return tl::unexpected( e );
    }

    if( !board->HasVariant( oldName ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "no variant named '{}' exists", aCtx.Request.old_name() ) );
        return tl::unexpected( e );
    }

    if( board->HasVariant( newName ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "a variant named '{}' already exists", aCtx.Request.new_name() ) );
        return tl::unexpected( e );
    }

    board->RenameVariant( oldName, newName );

    if( frame() )
        frame()->UpdateVariantSelectionCtrl();

    bumpRevision();
    publishProjectChanged( kiapi::common::events::PCK_VARIANTS, aCtx.ClientName );

    return Empty();
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleCopyVariant( const HANDLER_CONTEXT<CopyVariant>& aCtx )
{
    if( aCtx.Request.document().type() != DocumentType::DOCTYPE_PCB )
        return tl::unexpected( MakeResponseStatus( AS_UNHANDLED ) );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    BOARD* board = pcbContext()->GetBoard();

    wxString oldName = wxString::FromUTF8( aCtx.Request.old_name() );
    wxString newName = wxString::FromUTF8( aCtx.Request.new_name() );

    if( oldName.IsEmpty() || oldName.CmpNoCase( GetDefaultVariantName() ) == 0 )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "'{}' is not a valid variant name", aCtx.Request.old_name() ) );
        return tl::unexpected( e );
    }

    if( newName.IsEmpty() || newName.CmpNoCase( GetDefaultVariantName() ) == 0 )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "'{}' is not a valid variant name", aCtx.Request.new_name() ) );
        return tl::unexpected( e );
    }

    if( !board->HasVariant( oldName ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "no variant named '{}' exists", aCtx.Request.old_name() ) );
        return tl::unexpected( e );
    }

    if( board->HasVariant( newName ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "a variant named '{}' already exists", aCtx.Request.new_name() ) );
        return tl::unexpected( e );
    }

    board->CopyVariant( oldName, newName,
                        aCtx.Request.has_new_description() ? wxString::FromUTF8( aCtx.Request.new_description() )
                                                           : wxString() );

    if( frame() )
        frame()->UpdateVariantSelectionCtrl();

    bumpRevision();
    publishProjectChanged( kiapi::common::events::PCK_VARIANTS, aCtx.ClientName );

    return Empty();
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleSetVariantDescription( const HANDLER_CONTEXT<SetVariantDescription>& aCtx )
{
    if( aCtx.Request.document().type() != DocumentType::DOCTYPE_PCB )
        return tl::unexpected( MakeResponseStatus( AS_UNHANDLED ) );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    BOARD* board = pcbContext()->GetBoard();

    wxString name = wxString::FromUTF8( aCtx.Request.name() );

    if( name.IsEmpty() || name.CmpNoCase( GetDefaultVariantName() ) == 0 || !board->HasVariant( name ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "no variant named '{}' exists", aCtx.Request.name() ) );
        return tl::unexpected( e );
    }

    board->SetVariantDescription( name, wxString::FromUTF8( aCtx.Request.description() ) );

    bumpRevision();
    publishProjectChanged( kiapi::common::events::PCK_VARIANTS, aCtx.ClientName );

    return Empty();
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleSetCurrentVariant( const HANDLER_CONTEXT<SetCurrentVariant>& aCtx )
{
    if( aCtx.Request.document().type() != DocumentType::DOCTYPE_PCB )
        return tl::unexpected( MakeResponseStatus( AS_UNHANDLED ) );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    BOARD* board = pcbContext()->GetBoard();

    if( aCtx.Request.has_name() && !aCtx.Request.name().empty() )
    {
        if( wxString name = wxString::FromUTF8( aCtx.Request.name() ); !board->HasVariant( name ) )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( fmt::format( "no variant named '{}' exists", aCtx.Request.name() ) );
            return tl::unexpected( e );
        }
    }

    wxString varName = aCtx.Request.has_name() ? wxString::FromUTF8( aCtx.Request.name() ) : wxString();

    if( frame() )
        frame()->SetCurrentVariant( varName );
    else
        board->SetCurrentVariant( varName );

    bumpRevision();
    publishProjectChanged( kiapi::common::events::PCK_VARIANTS, aCtx.ClientName );

    return Empty();
}


HANDLER_RESULT<CurrentVariantResponse>
API_HANDLER_PCB::handleGetCurrentVariant( const HANDLER_CONTEXT<GetCurrentVariant>& aCtx )
{
    if( aCtx.Request.document().type() != DocumentType::DOCTYPE_PCB )
        return tl::unexpected( MakeResponseStatus( AS_UNHANDLED ) );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    CurrentVariantResponse response;

    if( wxString current = pcbContext()->GetBoard()->GetCurrentVariant(); !current.IsEmpty() )
        response.set_name( current.ToUTF8() );

    return response;
}


void API_HANDLER_PCB::recordOriginUndo( const std::string& aClientName, UNDO_REDO aType, const VECTOR2I& aFrom,
                                        const VECTOR2I& aTo )
{
    API_UNDO_STACK* stack = apiUndoStack();

    if( !stack || aFrom == aTo )
        return;

    // The same marker/image pair the origin tools record: the picked item holds the new origin,
    // its link the old one, and an undo swaps them
    KIGFX::ORIGIN_VIEWITEM* marker = new KIGFX::ORIGIN_VIEWITEM( VECTOR2D( aTo ), 0 );
    KIGFX::ORIGIN_VIEWITEM* image = new KIGFX::ORIGIN_VIEWITEM( VECTOR2D( aFrom ), 0 );

    marker->SetFlags( UR_TRANSIENT );
    image->SetFlags( UR_TRANSIENT );

    ITEM_PICKER picker( nullptr, marker, aType );
    picker.SetLink( image );

    PICKED_ITEMS_LIST list;
    list.SetDescription( aType == UNDO_REDO::DRILLORIGIN ? _( "Set Drill Origin" ) : _( "Set Grid Origin" ) );
    list.PushItem( picker );

    stack->SetNextAttribution( aClientName, std::nullopt );
    stack->SaveCopyInUndoList( list, false );
}


//// Ratsnest and net lengths (Since 11.0) ////

HANDLER_RESULT<std::set<int>>
API_HANDLER_PCB::resolveNets( const google::protobuf::RepeatedPtrField<board::types::Net>& aNets )
{
    std::set<int> codes;

    for( const board::types::Net& net : aNets )
    {
        NETINFO_ITEM* item = board()->FindNet( wxString::FromUTF8( net.name() ) );

        if( !item )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( fmt::format( "net '{}' does not exist on the board", net.name() ) );
            return tl::unexpected( e );
        }

        codes.insert( item->GetNetCode() );
    }

    return codes;
}


HANDLER_RESULT<RatsnestResponse> API_HANDLER_PCB::handleGetRatsnest( const HANDLER_CONTEXT<GetRatsnest>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    HANDLER_RESULT<std::set<int>> filter = resolveNets( aCtx.Request.nets() );

    if( !filter )
        return tl::unexpected( filter.error() );

    BOARD*                             brd = board();
    std::shared_ptr<CONNECTIVITY_DATA> connectivity = brd->GetConnectivity();

    connectivity->RecalculateRatsnest();

    RatsnestResponse response;

    for( int netcode = 1; netcode < connectivity->GetNetCount(); netcode++ )
    {
        if( !filter->empty() && !filter->contains( netcode ) )
            continue;

        RN_NET*       rn = connectivity->GetRatsnestForNet( netcode );
        NETINFO_ITEM* net = brd->FindNet( netcode );

        if( !rn || !net )
            continue;

        for( const CN_EDGE& edge : rn->GetEdges() )
        {
            std::shared_ptr<const CN_ANCHOR> source = edge.GetSourceNode();
            std::shared_ptr<const CN_ANCHOR> target = edge.GetTargetNode();

            if( !source || !target || !source->Parent() || !target->Parent() )
                continue;

            RatsnestEdge* out = response.add_edges();
            out->mutable_net()->mutable_code()->set_value( net->GetNetCode() );
            out->mutable_net()->set_name( net->GetNetname().ToUTF8() );
            out->mutable_source()->set_value( source->Parent()->m_Uuid.AsStdString() );
            PackVector2( *out->mutable_source_position(), source->Pos() );
            out->mutable_target()->set_value( target->Parent()->m_Uuid.AsStdString() );
            PackVector2( *out->mutable_target_position(), target->Pos() );
            out->mutable_length()->set_value_nm( KiROUND( ( target->Pos() - source->Pos() ).EuclideanNorm() ) );
        }
    }

    response.set_unrouted_count( connectivity->GetUnconnectedCount( false ) );
    return response;
}


HANDLER_RESULT<UnroutedCountResponse>
API_HANDLER_PCB::handleGetUnroutedCount( const HANDLER_CONTEXT<GetUnroutedCount>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    std::shared_ptr<CONNECTIVITY_DATA> connectivity = board()->GetConnectivity();

    connectivity->RecalculateRatsnest();

    UnroutedCountResponse response;
    uint32_t              nets = 0;

    for( int netcode = 1; netcode < connectivity->GetNetCount(); netcode++ )
    {
        RN_NET* rn = connectivity->GetRatsnestForNet( netcode );

        if( rn && !rn->GetEdges().empty() )
            nets++;
    }

    response.set_unrouted_count( connectivity->GetUnconnectedCount( false ) );
    response.set_unrouted_net_count( nets );
    return response;
}


HANDLER_RESULT<NetLengthsResponse> API_HANDLER_PCB::handleGetNetLengths( const HANDLER_CONTEXT<GetNetLengths>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    HANDLER_RESULT<std::set<int>> filter = resolveNets( aCtx.Request.nets() );

    if( !filter )
        return tl::unexpected( filter.error() );

    BOARD*                             brd = board();
    std::shared_ptr<CONNECTIVITY_DATA> connectivity = brd->GetConnectivity();
    LENGTH_DELAY_CALCULATION*          calc = brd->GetLengthCalculation();

    if( !calc )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNKNOWN );
        e.set_error_message( "the board has no length calculation engine" );
        return tl::unexpected( e );
    }

    connectivity->RecalculateRatsnest();

    // The copper items per net, as the net inspector collects them
    std::map<int, std::vector<LENGTH_DELAY_CALCULATION_ITEM>> itemsByNet;

    for( int netcode : *filter )
        itemsByNet[netcode];

    for( CN_ITEM* cnItem : connectivity->GetConnectivityAlgo()->ItemList() )
    {
        if( !cnItem->Valid() || !cnItem->Parent() )
            continue;

        int netcode = cnItem->Net();

        if( netcode <= 0 || ( !filter->empty() && !filter->contains( netcode ) ) )
            continue;

        switch( cnItem->Parent()->Type() )
        {
        case PCB_TRACE_T:
        case PCB_ARC_T:
        case PCB_VIA_T:
        case PCB_PAD_T:
            itemsByNet[netcode].emplace_back( calc->GetLengthCalculationItem( cnItem->Parent() ) );
            break;

        default:
            break;
        }
    }

    constexpr PATH_OPTIMISATIONS opts = { .OptimiseVias = true,
                                          .MergeTracks = true,
                                          .OptimiseTracesInPads = true,
                                          .InferViaInPad = false };

    const LENGTH_DELAY_DOMAIN_OPT domain = aCtx.Request.with_delays() ? LENGTH_DELAY_DOMAIN_OPT::WITH_DELAY_DETAIL
                                                                      : LENGTH_DELAY_DOMAIN_OPT::NO_DELAY_DETAIL;

    NetLengthsResponse response;

    for( auto& [netcode, items] : itemsByNet )
    {
        NETINFO_ITEM* net = brd->FindNet( netcode );

        if( !net )
            continue;

        LENGTH_DELAY_STATS stats = calc->CalculateLengthDetails( items, opts, nullptr, nullptr,
                                                                 LENGTH_DELAY_LAYER_OPT::WITH_LAYER_DETAIL, domain );

        // Nets without pads are listed only when asked for by name, as the inspector does
        if( filter->empty() && stats.NumPads == 0 )
            continue;

        NetLength* out = response.add_lengths();
        out->mutable_net()->set_name( net->GetNetname().ToUTF8() );
        out->set_pad_count( stats.NumPads );
        out->set_via_count( stats.NumVias );
        out->mutable_track_length()->set_value_nm( stats.TrackLength );
        out->mutable_via_length()->set_value_nm( stats.ViaLength );
        out->mutable_pad_to_die_length()->set_value_nm( stats.PadToDieLength );
        out->mutable_total_length()->set_value_nm( stats.TotalLength() );

        if( RN_NET* rn = connectivity->GetRatsnestForNet( netcode ) )
            out->mutable_unrouted_length()->set_value_nm( rn->GetTotalAirlineLength() );

        if( stats.LayerLengths )
        {
            for( const auto& [layer, length] : *stats.LayerLengths )
            {
                NetLayerLength* layerLength = out->add_layer_lengths();
                layerLength->set_layer( ToProtoEnum<PCB_LAYER_ID, board::types::BoardLayer>( layer ) );
                layerLength->mutable_length()->set_value_nm( length );
            }
        }

        if( aCtx.Request.with_delays() )
            out->set_total_delay_ps( stats.TotalDelay() );
    }

    return response;
}


//// Footprint updates (Since 11.0) ////

HANDLER_RESULT<UpdateFootprintsFromLibraryResponse>
API_HANDLER_PCB::handleUpdateFootprintsFromLibrary( const HANDLER_CONTEXT<UpdateFootprintsFromLibrary>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    const UpdateFootprintsFromLibrary& req = aCtx.Request;
    BOARD*                             brd = board();
    std::vector<FOOTPRINT*>            targets;

    for( const types::KIID& id : req.footprints() )
    {
        std::optional<BOARD_ITEM*> item = getItemById( KIID( id.value() ) );

        if( !item || ( *item )->Type() != PCB_FOOTPRINT_T )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( fmt::format( "footprint with ID {} not found on the board", id.value() ) );
            return tl::unexpected( e );
        }

        targets.push_back( static_cast<FOOTPRINT*>( *item ) );
    }

    if( req.footprints().empty() )
        targets.assign( brd->Footprints().begin(), brd->Footprints().end() );

    std::optional<LIB_ID> newId;

    if( req.has_new_footprint() )
    {
        newId = LIB_ID( wxString::FromUTF8( req.new_footprint().library_nickname() ),
                        wxString::FromUTF8( req.new_footprint().entry_name() ) );

        if( !newId->IsValid() )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( "new_footprint needs a library nickname and an entry name" );
            return tl::unexpected( e );
        }
    }

    // Library footprints come from the project's tables, so make sure they are loaded
    FOOTPRINT_LIBRARY_ADAPTER* adapter = PROJECT_PCB::FootprintLibAdapter( &project() );
    adapter->AsyncLoad();
    adapter->BlockUntilLoaded();

    auto option =
            []( bool aHas, bool aValue, bool aDefault )
            {
                return aHas ? aValue : aDefault;
            };

    const bool deleteExtraTexts = option( req.has_delete_extra_texts(), req.delete_extra_texts(), true );
    const bool resetTextLayers = option( req.has_reset_text_layers(), req.reset_text_layers(), true );
    const bool resetTextEffects = option( req.has_reset_text_effects(), req.reset_text_effects(), true );
    const bool resetTextPositions = option( req.has_reset_text_positions(), req.reset_text_positions(), true );
    const bool resetTextContent = option( req.has_reset_text_content(), req.reset_text_content(), true );
    const bool resetFabAttrs = option( req.has_reset_fabrication_attributes(), req.reset_fabrication_attributes(),
                                       true );
    const bool resetClearances = option( req.has_reset_clearance_overrides(), req.reset_clearance_overrides(), true );
    const bool reset3DModels = option( req.has_reset_3d_models(), req.reset_3d_models(), true );

    BOARD_COMMIT*                       commit = static_cast<BOARD_COMMIT*>( getCurrentCommit( aCtx.ClientName ) );
    UpdateFootprintsFromLibraryResponse response;

    for( FOOTPRINT* footprint : targets )
    {
        LIB_ID     id = newId.value_or( footprint->GetFPID() );
        wxString   reference = footprint->GetReference();
        FOOTPRINT* libFootprint = nullptr;

        try
        {
            libFootprint = adapter->LoadFootprint( id, false );
        }
        catch( const IO_ERROR& )
        {
            libFootprint = nullptr;
        }

        if( !libFootprint )
        {
            response.add_missing( reference.ToUTF8() );
            response.add_messages( fmt::format( "{} ({}): library footprint not found", reference.ToUTF8().data(),
                                                id.Format().c_str() ) );
            continue;
        }

        bool updated = !req.only_changed() || footprint->FootprintNeedsUpdate( libFootprint );
        bool shifted = false;

        if( req.only_changed() && !updated && !req.match_pad_positions() )
        {
            delete libFootprint;
            response.set_unchanged_count( response.unchanged_count() + 1 );
            response.add_messages( fmt::format( "{} ({}): no changes", reference.ToUTF8().data(),
                                                id.Format().c_str() ) );
            continue;
        }

        brd->ExchangeFootprint( footprint, libFootprint, *commit, req.match_pad_positions(), deleteExtraTexts,
                                resetTextLayers, resetTextEffects, resetTextPositions, resetTextContent,
                                resetFabAttrs, resetClearances, reset3DModels, req.reset_transform(), &updated,
                                &shifted );

        if( req.only_changed() && !updated )
        {
            response.set_unchanged_count( response.unchanged_count() + 1 );
            response.add_messages( fmt::format( "{} ({}): {}", reference.ToUTF8().data(), id.Format().c_str(),
                                                shifted ? "shifted/rotated to match pad positions" : "no changes" ) );
        }
        else
        {
            response.set_updated_count( response.updated_count() + 1 );
            response.add_messages( fmt::format( "{} ({}): {}", reference.ToUTF8().data(), id.Format().c_str(),
                                                newId ? "changed" : "updated" ) );
        }
    }

    if( !m_activeClients.contains( aCtx.ClientName ) )
        pushCurrentCommit( aCtx.ClientName, newId ? _( "Change Footprint" ) : _( "Update Footprint" ) );

    return response;
}


//// Teardrops (Since 11.0) ////

HANDLER_RESULT<SetTeardropsResponse> API_HANDLER_PCB::handleSetTeardrops( const HANDLER_CONTEXT<SetTeardrops>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    const SetTeardrops& req = aCtx.Request;
    ApiResponseStatus   e;
    e.set_status( ApiStatusCode::AS_BAD_REQUEST );

    if( req.action() == TDA_UNKNOWN )
    {
        e.set_error_message( "action must be TDA_ADD, TDA_REMOVE or TDA_SET" );
        return tl::unexpected( e );
    }

    if( req.action() == TDA_SET && !req.has_settings() )
    {
        e.set_error_message( "TDA_SET needs settings" );
        return tl::unexpected( e );
    }

    HANDLER_RESULT<std::set<int>> filter = resolveNets( req.nets() );

    if( !filter )
        return tl::unexpected( filter.error() );

    bool vias = req.vias();
    bool pthPads = req.pth_pads();
    bool smdPads = req.smd_pads();

    if( req.items().empty() && !vias && !pthPads && !smdPads && !req.track_to_track() )
        vias = pthPads = smdPads = true;

    BOARD*                    brd = board();
    BOARD_DESIGN_SETTINGS&    bds = brd->GetDesignSettings();
    TEARDROP_PARAMETERS_LIST* paramsList = bds.GetTeadropParamsList();
    BOARD_COMMIT*             commit = static_cast<BOARD_COMMIT*>( getCurrentCommit( aCtx.ClientName ) );
    SetTeardropsResponse      response;

    brd->SetLegacyTeardrops( false );

    auto apply =
            [&]( TEARDROP_PARAMETERS& aParams, TARGET_TD aDefaultTarget )
            {
                switch( req.action() )
                {
                case TDA_REMOVE:
                    aParams.m_Enabled = false;
                    break;

                case TDA_ADD:
                    aParams = *paramsList->GetParameters( aDefaultTarget );
                    aParams.m_Enabled = true;
                    break;

                case TDA_SET:
                    kiapi::board::UnpackTeardropSettings( aParams, req.settings() );
                    break;

                default:
                    break;
                }
            };

    auto process =
            [&]( BOARD_CONNECTED_ITEM* aItem, bool aFiltered )
            {
                if( aFiltered )
                {
                    if( !filter->empty() && !filter->contains( aItem->GetNetCode() ) )
                        return;

                    if( req.round_shapes_only() && !TEARDROP_MANAGER::IsUniformlyRound( aItem ) )
                        return;
                }

                const TEARDROP_PARAMETERS before = aItem->GetTeardropParams();

                commit->Modify( aItem );
                apply( aItem->GetTeardropParams(),
                       TEARDROP_MANAGER::IsUniformlyRound( aItem ) ? TARGET_ROUND : TARGET_RECT );

                // Report what actually changed, so that removing teardrops twice reports zero
                if( aItem->GetTeardropParams() != before )
                    response.set_item_count( response.item_count() + 1 );
            };

    if( !req.items().empty() )
    {
        for( const types::KIID& id : req.items() )
        {
            std::optional<BOARD_ITEM*> item = getItemById( KIID( id.value() ) );

            if( !item || ( ( *item )->Type() != PCB_PAD_T && ( *item )->Type() != PCB_VIA_T ) )
            {
                e.set_error_message( fmt::format( "item {} is not a pad or via on the board", id.value() ) );
                return tl::unexpected( e );
            }

            process( static_cast<BOARD_CONNECTED_ITEM*>( *item ), true );
        }
    }
    else
    {
        if( vias )
        {
            for( PCB_TRACK* track : brd->Tracks() )
            {
                if( track->Type() == PCB_VIA_T )
                    process( track, true );
            }
        }

        for( FOOTPRINT* footprint : brd->Footprints() )
        {
            for( PAD* pad : footprint->Pads() )
            {
                if( pthPads && pad->GetAttribute() == PAD_ATTRIB::PTH )
                    process( pad, true );
                else if( smdPads && ( pad->GetAttribute() == PAD_ATTRIB::SMD || pad->GetAttribute() == PAD_ATTRIB::CONN ) )
                    process( pad, true );
            }
        }
    }

    if( req.track_to_track() )
    {
        TEARDROP_PARAMETERS* trackParams = paramsList->GetParameters( TARGET_TRACK );
        TEARDROP_MANAGER     manager( brd, toolManager() );

        manager.DeleteTrackToTrackTeardrops( *commit );
        apply( *trackParams, TARGET_TRACK );

        if( trackParams->m_Enabled )
        {
            manager.BuildTrackCaches();
            manager.AddTeardropsOnTracks( *commit, nullptr, true );
        }
    }

    // The commit regenerates the teardrops of the pads and vias it modifies
    if( !m_activeClients.contains( aCtx.ClientName ) )
        pushCurrentCommit( aCtx.ClientName, _( "Edit Teardrops" ) );

    return response;
}


HANDLER_RESULT<SetTeardropsResponse>
API_HANDLER_PCB::handleRemoveTeardrops( const HANDLER_CONTEXT<RemoveTeardrops>& aCtx )
{
    HANDLER_CONTEXT<SetTeardrops> ctx;
    ctx.ClientName = aCtx.ClientName;
    *ctx.Request.mutable_board() = aCtx.Request.board();
    ctx.Request.set_action( TDA_REMOVE );
    ctx.Request.set_vias( true );
    ctx.Request.set_pth_pads( true );
    ctx.Request.set_smd_pads( true );
    ctx.Request.set_track_to_track( true );

    return handleSetTeardrops( ctx );
}


//// Autoplacement (Since 11.0) ////

HANDLER_RESULT<AutoplaceFootprintsResponse>
API_HANDLER_PCB::handleAutoplaceFootprints( const HANDLER_CONTEXT<AutoplaceFootprints>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    // The placer reverts its commit on failure, so it gets one of its own rather than a client's
    if( m_activeClients.contains( aCtx.ClientName ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BUSY );
        e.set_error_message( "AutoplaceFootprints cannot run inside a client commit; call EndCommit first" );
        return tl::unexpected( e );
    }

    BOARD*                  brd = board();
    std::vector<FOOTPRINT*> footprints;

    for( const types::KIID& id : aCtx.Request.footprints() )
    {
        std::optional<BOARD_ITEM*> item = getItemById( KIID( id.value() ) );

        if( !item || ( *item )->Type() != PCB_FOOTPRINT_T )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( fmt::format( "footprint with ID {} not found on the board", id.value() ) );
            return tl::unexpected( e );
        }

        footprints.push_back( static_cast<FOOTPRINT*>( *item ) );
    }

    const bool placeOffboard = footprints.empty() || aCtx.Request.include_offboard();

    std::map<FOOTPRINT*, VECTOR2I> before;

    for( FOOTPRINT* footprint : brd->Footprints() )
        before[footprint] = footprint->GetPosition();

    std::unique_ptr<COMMIT> owned = createCommit();
    BOARD_COMMIT*           commit = static_cast<BOARD_COMMIT*>( owned.get() );
    AR_AUTOPLACER           autoplacer( brd );

    AR_RESULT result = autoplacer.AutoplaceFootprints( footprints, commit, placeOffboard );

    AutoplaceFootprintsResponse response;

    if( result != AR_COMPLETED )
    {
        commit->Revert();
        response.set_result( APR_NO_BOARD_OUTLINE );
        return response;
    }

    uint32_t placed = 0;

    for( FOOTPRINT* footprint : brd->Footprints() )
    {
        if( before.contains( footprint ) && before[footprint] != footprint->GetPosition() )
            placed++;
    }

    publishDocumentChanged( aCtx.ClientName, _( "Autoplace Footprints" ), nullptr, commit );
    commit->Push( _( "Autoplace Footprints" ) );

    if( frame() )
        frame()->Refresh();

    response.set_result( APR_COMPLETED );
    response.set_placed_count( placed );
    return response;
}


//// Global deletion (Since 11.0) ////

HANDLER_RESULT<GlobalDeletionResponse> API_HANDLER_PCB::handleGlobalDeletion( const HANDLER_CONTEXT<GlobalDeletion>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    const GlobalDeletion& req = aCtx.Request;
    ApiResponseStatus     e;
    e.set_status( ApiStatusCode::AS_BAD_REQUEST );

    std::vector<KICAD_T> typeList = parseRequestedItemTypes( req.types() );

    if( typeList.empty() )
    {
        e.set_error_message( "GlobalDeletion needs at least one valid item type" );
        return tl::unexpected( e );
    }

    std::set<KICAD_T> types( typeList.begin(), typeList.end() );
    LSET              layers;

    for( int layer : req.layers() )
    {
        PCB_LAYER_ID id = FromProtoEnum<PCB_LAYER_ID>( static_cast<board::types::BoardLayer>( layer ) );

        if( id == UNDEFINED_LAYER || id == UNSELECTED_LAYER )
        {
            e.set_error_message( fmt::format( "layer {} is not a board layer", layer ) );
            return tl::unexpected( e );
        }

        layers.set( id );
    }

    if( req.layers().empty() )
        layers = LSET().set();

    const LockFilter lock = req.locked() == LF_UNKNOWN ? LF_ALL : req.locked();

    auto matches =
            [&]( BOARD_ITEM* aItem )
            {
                if( lock == LF_LOCKED && !aItem->IsLocked() )
                    return false;

                if( lock == LF_UNLOCKED && aItem->IsLocked() )
                    return false;

                return ( aItem->GetLayerSet() & layers ).any();
            };

    BOARD*                 brd = board();
    BOARD_COMMIT*          commit = static_cast<BOARD_COMMIT*>( getCurrentCommit( aCtx.ClientName ) );
    GlobalDeletionResponse response;

    auto remove =
            [&]( BOARD_ITEM* aItem )
            {
                commit->Remove( aItem );
                response.set_deleted_count( response.deleted_count() + 1 );
            };

    if( types.contains( PCB_ZONE_T ) )
    {
        for( ZONE* zone : brd->Zones() )
        {
            if( zone->IsTeardropArea() && !req.teardrops() )
                continue;

            if( matches( zone ) )
                remove( zone );
        }
    }

    for( BOARD_ITEM* item : brd->Drawings() )
    {
        if( !types.contains( item->Type() ) )
            continue;

        if( item->Type() == PCB_SHAPE_T && item->GetLayer() == Edge_Cuts && !req.board_edges() )
            continue;

        if( matches( item ) )
            remove( item );
    }

    if( types.contains( PCB_FOOTPRINT_T ) )
    {
        for( FOOTPRINT* footprint : brd->Footprints() )
        {
            if( matches( footprint ) )
                remove( footprint );
        }
    }

    const bool anyTrackType = types.contains( PCB_TRACE_T ) || types.contains( PCB_ARC_T ) || types.contains( PCB_VIA_T );

    if( anyTrackType )
    {
        for( PCB_TRACK* track : brd->Tracks() )
        {
            if( types.contains( track->Type() ) && matches( track ) )
                remove( track );
        }

        // Tuning patterns that already lost their tracks go with them, as the dialog does
        for( PCB_GENERATOR* generator : brd->Generators() )
        {
            if( PCB_TUNING_PATTERN* pattern = dynamic_cast<PCB_TUNING_PATTERN*>( generator ) )
            {
                if( pattern->GetBoardItems().empty() )
                    remove( pattern );
            }
        }
    }

    if( types.contains( PCB_GROUP_T ) )
    {
        for( PCB_GROUP* group : brd->Groups() )
        {
            if( lock == LF_ALL || ( lock == LF_LOCKED ) == group->IsLocked() )
                remove( group );
        }
    }

    if( !m_activeClients.contains( aCtx.ClientName ) )
        pushCurrentCommit( aCtx.ClientName, _( "Global Delete" ) );

    if( types.contains( PCB_MARKER_T ) )
    {
        response.set_deleted_count( response.deleted_count() + static_cast<uint32_t>( brd->Markers().size() ) );
        brd->DeleteMARKERs();
        bumpRevision();
    }

    if( frame() )
        frame()->Refresh();

    return response;
}


//// Graphics defaults (Since 11.0) ////

namespace
{

int layerClassIndex( board::BoardLayerClass aClass )
{
    switch( aClass )
    {
    case board::BLC_SILKSCREEN:  return LAYER_CLASS_SILK;
    case board::BLC_COPPER:      return LAYER_CLASS_COPPER;
    case board::BLC_EDGES:       return LAYER_CLASS_EDGES;
    case board::BLC_COURTYARD:   return LAYER_CLASS_COURTYARD;
    case board::BLC_FABRICATION: return LAYER_CLASS_FAB;
    case board::BLC_OTHER:       return LAYER_CLASS_OTHERS;
    default:                     return -1;
    }
}

} // namespace


void API_HANDLER_PCB::packGraphicsDefaults( board::GraphicsDefaults& aOut ) const
{
    const BOARD_DESIGN_SETTINGS& bds = board()->GetDesignSettings();

    for( board::BoardLayerClass layerClass : { board::BLC_SILKSCREEN, board::BLC_COPPER, board::BLC_EDGES,
                                               board::BLC_COURTYARD, board::BLC_FABRICATION, board::BLC_OTHER } )
    {
        int                                index = layerClassIndex( layerClass );
        board::BoardLayerGraphicsDefaults* out = aOut.add_layers();

        out->set_layer( layerClass );
        out->mutable_line_thickness()->set_value_nm( bds.m_LineThickness[index] );

        types::TextAttributes* text = out->mutable_text();
        PackVector2( *text->mutable_size(), bds.m_TextSize[index] );
        text->mutable_stroke_width()->set_value_nm( bds.m_TextThickness[index] );
        text->set_italic( bds.m_TextItalic[index] );
        text->set_keep_upright( bds.m_TextUpright[index] );
        text->set_visible( true );
    }
}


HANDLER_RESULT<GraphicsDefaultsResponse>
API_HANDLER_PCB::handleGetGraphicsDefaults( const HANDLER_CONTEXT<GetGraphicsDefaults>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    GraphicsDefaultsResponse response;
    packGraphicsDefaults( *response.mutable_defaults() );
    return response;
}


HANDLER_RESULT<GraphicsDefaultsResponse>
API_HANDLER_PCB::handleSetGraphicsDefaults( const HANDLER_CONTEXT<SetGraphicsDefaults>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    ApiResponseStatus e;
    e.set_status( ApiStatusCode::AS_BAD_REQUEST );

    for( const board::BoardLayerGraphicsDefaults& in : aCtx.Request.defaults().layers() )
    {
        if( layerClassIndex( in.layer() ) < 0 )
        {
            e.set_error_message( "every entry needs a known layer class" );
            return tl::unexpected( e );
        }

        if( in.line_thickness().value_nm() < 0 || in.text().stroke_width().value_nm() < 0
            || in.text().size().x_nm() <= 0 || in.text().size().y_nm() <= 0 )
        {
            e.set_error_message( "text sizes must be positive and thicknesses non-negative" );
            return tl::unexpected( e );
        }
    }

    BOARD_DESIGN_SETTINGS& bds = board()->GetDesignSettings();

    for( const board::BoardLayerGraphicsDefaults& in : aCtx.Request.defaults().layers() )
    {
        int index = layerClassIndex( in.layer() );

        bds.m_LineThickness[index] = static_cast<int>( in.line_thickness().value_nm() );
        bds.m_TextSize[index] = UnpackVector2( in.text().size() );
        bds.m_TextThickness[index] = static_cast<int>( in.text().stroke_width().value_nm() );
        bds.m_TextItalic[index] = in.text().italic();
        bds.m_TextUpright[index] = in.text().keep_upright();
    }

    if( aCtx.Request.defaults().layers_size() > 0 )
        bumpRevision();

    GraphicsDefaultsResponse response;
    packGraphicsDefaults( *response.mutable_defaults() );
    publishProjectChanged( kiapi::common::events::PCK_SETTINGS, aCtx.ClientName );

    return response;
}
