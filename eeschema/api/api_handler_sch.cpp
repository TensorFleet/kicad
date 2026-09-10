/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2024 Jon Evans <jon@craftyjon.com>
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

#include <api/api_handler_sch.h>
#include <api/api_job_registry.h>
#include <api/api_jobs.h>
#include <api/api_enums.h>
#include <api/api_sch_utils.h>
#include <api/api_utils.h>
#include <api/cross_probe_client.h>
#include <api/sch_context.h>
#include <api/api_server.h>
#include <api/sch_api_save.h>
#include <api/board/board_commands.pb.h>
#include <fmt.h>
#include <fmt/ranges.h>
#include <wx/log.h>
#include <magic_enum.hpp>
#include <base_screen.h>
#include <drawing_sheet/ds_proxy_view_item.h>
#include <erc/erc.h>
#include <erc/erc_item.h>
#include <erc/erc_settings.h>
#include <libraries/symbol_library_adapter.h>
#include <project_sch.h>
#include <sch_marker.h>
#include <jobs/job_export_bom.h>
#include <jobs/job_export_sch_netlist.h>
#include <jobs/job_export_sch_plot.h>
#include <kiway.h>
#include <sch_field.h>
#include <sch_group.h>
#include <common.h>
#include <connection_graph.h>
#include <sch_commit.h>
#include <string_utils.h>
#include <sch_edit_frame.h>
#include <sch_label.h>
#include <sch_reference_list.h>
#include <sch_screen.h>
#include <sch_sheet.h>
#include <sch_sheet_path.h>
#include <sch_sheet_pin.h>
#include <sch_symbol.h>
#include <schematic.h>
#include <tool/actions.h>
#include <tool/tool_manager.h>
#include <tools/sch_actions.h>
#include <tools/sch_selection.h>
#include <tools/sch_selection_tool.h>
#include <io/kicad/kicad_io_utils.h>
#include <richio.h>
#include <sch_io/kicad_sexpr/sch_io_kicad_sexpr.h>
#include <sch_io/sch_io_mgr.h>
#include <schematic_settings.h>
#include <netlist_exporters/netlist_exporter_kicad.h>
#include <reporter.h>
#include <lib_id.h>
#include <wx/ffile.h>
#include <wx/tokenzr.h>
#include <project.h>
#include <wildcards_and_files_ext.h>
#include <wx/filename.h>

#include <api/common/types/base_types.pb.h>
#include <trace_helpers.h>

using namespace kiapi::common::commands;
using namespace kiapi::schematic::commands;
using kiapi::common::types::CommandStatus;
using kiapi::common::types::DocumentType;
using kiapi::common::types::ItemRequestStatus;


std::set<KICAD_T> API_HANDLER_SCH::s_allowedTypes = {
    SCH_JUNCTION_T,
    SCH_NO_CONNECT_T,
    SCH_BUS_WIRE_ENTRY_T,
    SCH_BUS_BUS_ENTRY_T,
    SCH_LINE_T,
    SCH_SHAPE_T,
    SCH_RULE_AREA_T,
    SCH_BITMAP_T,
    SCH_TEXTBOX_T,
    SCH_TEXT_T,
    SCH_TABLE_T,
    SCH_LABEL_T,
    SCH_GLOBAL_LABEL_T,
    SCH_GROUP_T,
    SCH_HIER_LABEL_T,
    SCH_DIRECTIVE_LABEL_T,
    SCH_SYMBOL_T,
    SCH_SHEET_T,
};


API_HANDLER_SCH::API_HANDLER_SCH( SCH_EDIT_FRAME* aFrame ) :
        API_HANDLER_SCH( CreateSchFrameContext( aFrame ), aFrame )
{
}


API_HANDLER_SCH::API_HANDLER_SCH( std::shared_ptr<SCH_CONTEXT> aContext,
                                  SCH_EDIT_FRAME* aFrame ) :
        API_HANDLER_EDITOR( aFrame ),
        m_context( std::move( aContext ) )
{
    using namespace kiapi::schematic::jobs;
    using namespace kiapi::schematic::types;
    using namespace kiapi::schematic::commands;

    registerHandler<GetOpenDocuments, GetOpenDocumentsResponse>(
            &API_HANDLER_SCH::handleGetOpenDocuments );
    registerHandler<SaveDocument, google::protobuf::Empty>(
            &API_HANDLER_SCH::handleSaveDocument );
    registerHandler<SaveCopyOfDocument, google::protobuf::Empty>(
            &API_HANDLER_SCH::handleSaveCopyOfDocument );
    registerHandler<RevertDocument, google::protobuf::Empty>(
            &API_HANDLER_SCH::handleRevertDocument, HANDLER_MODE::GUI_ONLY );

    registerHandler<GetItems, GetItemsResponse>( &API_HANDLER_SCH::handleGetItems );
    registerHandler<GetItemsById, GetItemsResponse>( &API_HANDLER_SCH::handleGetItemsById );
    registerHandler<SaveDocumentToString, SavedDocumentResponse>( &API_HANDLER_SCH::handleSaveDocumentToString );
    registerHandler<SaveItemsToString, SavedSelectionResponse>( &API_HANDLER_SCH::handleSaveItemsToString );
    registerHandler<ParseAndCreateItemsFromString, CreateItemsResponse>(
            &API_HANDLER_SCH::handleParseAndCreateItemsFromString );

    registerHandler<GetSelection, SelectionResponse>( &API_HANDLER_SCH::handleGetSelection, HANDLER_MODE::GUI_ONLY );
    registerHandler<ClearSelection, Empty>( &API_HANDLER_SCH::handleClearSelection, HANDLER_MODE::GUI_ONLY );
    registerHandler<AddToSelection, SelectionResponse>(
            &API_HANDLER_SCH::handleAddToSelection, HANDLER_MODE::GUI_ONLY );
    registerHandler<RemoveFromSelection, SelectionResponse>(
            &API_HANDLER_SCH::handleRemoveFromSelection, HANDLER_MODE::GUI_ONLY );

    registerHandler<RunSchematicJobExportSvg, types::RunJobResponse>(
            &API_HANDLER_SCH::handleRunSchematicJobExportSvg );
    registerHandler<RunSchematicJobExportDxf, types::RunJobResponse>(
            &API_HANDLER_SCH::handleRunSchematicJobExportDxf );
    registerHandler<RunSchematicJobExportPdf, types::RunJobResponse>(
            &API_HANDLER_SCH::handleRunSchematicJobExportPdf );
    registerHandler<RunSchematicJobExportPs, types::RunJobResponse>(
            &API_HANDLER_SCH::handleRunSchematicJobExportPs );
    registerHandler<RunSchematicJobExportNetlist, types::RunJobResponse>(
            &API_HANDLER_SCH::handleRunSchematicJobExportNetlist );
    registerHandler<RunSchematicJobExportBOM, types::RunJobResponse>(
            &API_HANDLER_SCH::handleRunSchematicJobExportBOM );
    registerHandler<GetSchematicHierarchy, SchematicHierarchyResponse>( &API_HANDLER_SCH::handleGetSchematicHierarchy );
    registerHandler<RunSchematicJobErc, ErcResultsResponse>( &API_HANDLER_SCH::handleRunSchematicJobErc );
    registerHandler<GetErcMarkers, ErcResultsResponse>( &API_HANDLER_SCH::handleGetErcMarkers );
    registerHandler<SetErcMarkerExcluded, Empty>( &API_HANDLER_SCH::handleSetErcMarkerExcluded );
    registerHandler<GetErcSeverities, ErcSeveritiesResponse>( &API_HANDLER_SCH::handleGetErcSeverities );
    registerHandler<SetErcSeverities, ErcSeveritiesResponse>( &API_HANDLER_SCH::handleSetErcSeverities );
    registerHandler<GetPageSettings, types::PageSettings>( &API_HANDLER_SCH::handleGetPageSettings );
    registerHandler<SetPageSettings, types::PageSettings>( &API_HANDLER_SCH::handleSetPageSettings );
    registerHandler<GetSchematicNetlist, SchematicNetlistResponse>( &API_HANDLER_SCH::handleGetSchematicNetlist );
    registerHandler<CrossProbeAnnounce, CrossProbeAnnounceResponse>( &API_HANDLER_SCH::handleCrossProbeAnnounce );
    registerHandler<SyncSelection, SyncSelectionResponse>(
            &API_HANDLER_SCH::handleSyncSelection, HANDLER_MODE::GUI_ONLY );
    registerHandler<HighlightNets, HighlightNetsResponse>(
            &API_HANDLER_SCH::handleHighlightNets, HANDLER_MODE::GUI_ONLY );
    registerHandler<GetVariants, VariantsResponse>( &API_HANDLER_SCH::handleGetVariants );
    registerHandler<AddVariant, Empty>( &API_HANDLER_SCH::handleAddVariant );
    registerHandler<DeleteVariant, Empty>( &API_HANDLER_SCH::handleDeleteVariant );
    registerHandler<RenameVariant, Empty>( &API_HANDLER_SCH::handleRenameVariant );
    registerHandler<CopyVariant, Empty>( &API_HANDLER_SCH::handleCopyVariant );
    registerHandler<SetVariantDescription, Empty>( &API_HANDLER_SCH::handleSetVariantDescription );
    registerHandler<SetCurrentVariant, Empty>( &API_HANDLER_SCH::handleSetCurrentVariant );
    registerHandler<GetCurrentVariant, CurrentVariantResponse>(
            &API_HANDLER_SCH::handleGetCurrentVariant );
    registerHandler<ExpandTextVariables, ExpandTextVariablesResponse>(
            &API_HANDLER_SCH::handleExpandTextVariables );

    // Since 11.0
    registerHandler<Annotate, AnnotateResponse>( &API_HANDLER_SCH::handleAnnotate );
    registerHandler<ClearAnnotation, AnnotateResponse>( &API_HANDLER_SCH::handleClearAnnotation );
    registerHandler<SyncSchematicToBoard, SyncSchematicToBoardResponse>(
            &API_HANDLER_SCH::handleSyncSchematicToBoard );
    registerHandler<GetSchematicSettings, SchematicSettings>( &API_HANDLER_SCH::handleGetSchematicSettings );
    registerHandler<SetSchematicSettings, SchematicSettings>( &API_HANDLER_SCH::handleSetSchematicSettings );
    registerHandler<GetSymbolFieldsTable, SymbolFieldsTableResponse>(
            &API_HANDLER_SCH::handleGetSymbolFieldsTable );
    registerHandler<SetSymbolFields, SetSymbolFieldsResponse>( &API_HANDLER_SCH::handleSetSymbolFields );
    registerHandler<AssignFootprints, AssignFootprintsResponse>( &API_HANDLER_SCH::handleAssignFootprints );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_SCH::runSchematicJob( const types::RunJobSettings& aSettings,
                                                                        std::unique_ptr<JOB> aJob )
{
    if( !m_context->GetKiway() )
    {
        types::RunJobResponse response;
        response.set_status( types::JobStatus::JS_ERROR );
        response.set_message( "Internal error" );
        wxCHECK_MSG( false, response, "context missing valid kiway in runSchematicJob?" );
        return response;
    }

    // The jobs handler reads an editor window's schematic directly, so only headless jobs may
    // leave the main thread
    bool async = aSettings.async() && !m_frame;

    return RunApiJob( Server(), m_context->GetKiway(), KIWAY::FACE_SCH, std::move( aJob ), async,
                      aSettings.return_inline() );
}


std::unique_ptr<COMMIT> API_HANDLER_SCH::createCommit()
{
    if( m_frame )
        return std::make_unique<SCH_COMMIT>( static_cast<SCH_EDIT_FRAME*>( m_frame ) );

    return std::make_unique<SCH_COMMIT>( toolManager() );
}


SCHEMATIC* API_HANDLER_SCH::schematic() const
{
    wxCHECK( m_context, nullptr );
    return m_context->GetSchematic();
}


SCH_EDIT_FRAME* API_HANDLER_SCH::frame() const
{
    return static_cast<SCH_EDIT_FRAME*>( m_frame );
}


std::optional<ApiResponseStatus> API_HANDLER_SCH::checkForHeadless( const std::string& aCommandName ) const
{
    if( m_frame )
        return std::nullopt;

    ApiResponseStatus e;
    e.set_status( ApiStatusCode::AS_UNIMPLEMENTED );
    e.set_error_message( fmt::format( "{} is not available in headless mode", aCommandName ) );
    return e;
}


bool API_HANDLER_SCH::packSchItem( google::protobuf::Any& aOut, SCH_ITEM* aItem,
                                   const SCH_SHEET_PATH& aPath )
{
    if( aItem->Type() == SCH_SYMBOL_T )
    {
        kiapi::schematic::types::SchematicSymbolInstance symbol;

        if( !PackSymbol( &symbol, static_cast<SCH_SYMBOL*>( aItem ), aPath ) )
            return false;

        aOut.PackFrom( symbol );
    }
    else if( aItem->Type() == SCH_SHEET_T )
    {
        kiapi::schematic::types::SheetSymbol sheet;

        if( !PackSheet( &sheet, static_cast<SCH_SHEET*>( aItem ), aPath ) )
            return false;

        aOut.PackFrom( sheet );
    }
    else
    {
        aItem->Serialize( aOut );
    }

    return true;
}


std::optional<SCH_ITEM*> API_HANDLER_SCH::getItemById( const KIID& aId, SCH_SHEET_PATH* aPathOut ) const
{
    if( !schematic()->HasHierarchy() )
        schematic()->RefreshHierarchy();

    SCH_ITEM* item = schematic()->ResolveItem( aId, aPathOut, true );

    if( !item )
        return std::nullopt;

    return item;
}


tl::expected<bool, ApiResponseStatus>
API_HANDLER_SCH::validateDocumentInternal( const DocumentSpecifier& aDocument ) const
{
    if( aDocument.type() != DocumentType::DOCTYPE_SCHEMATIC )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "the requested document is not a schematic" );
        return tl::unexpected( e );
    }

    const PROJECT& prj = m_context->Prj();

    if( aDocument.project().name().compare( prj.GetProjectName().ToUTF8() ) != 0 )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "the requested project {} is not open",
                                          aDocument.project().name() ) );
        return tl::unexpected( e );
    }

    if( aDocument.project().path().compare( prj.GetProjectPath().ToUTF8() ) != 0 )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "the requested project {} is not open at path {}",
                                          aDocument.project().name(),
                                          aDocument.project().path() ) );
        return tl::unexpected( e );
    }

    if( aDocument.has_sheet_path() )
    {
        KIID_PATH path = UnpackSheetPath( aDocument.sheet_path() );

        if( !schematic()->Hierarchy().HasPath( path ) )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( fmt::format( "the requested sheet path {} is not valid for this schematic",
                                              path.AsString().ToStdString() ) );
            return tl::unexpected( e );
        }
    }

    return true;
}


HANDLER_RESULT<google::protobuf::Empty> API_HANDLER_SCH::handleSaveDocument( const HANDLER_CONTEXT<SaveDocument>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( !context()->SaveSchematic() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "failed to save schematic" );
        return tl::unexpected( e );
    }

    notifyDocumentSaved( m_context->GetCurrentFileName() );
    return google::protobuf::Empty();
}


HANDLER_RESULT<google::protobuf::Empty>
API_HANDLER_SCH::handleSaveCopyOfDocument( const HANDLER_CONTEXT<SaveCopyOfDocument>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    wxFileName schematicPath( project().AbsolutePath( wxString::FromUTF8( aCtx.Request.path() ) ) );

    if( !schematicPath.IsOk() || !schematicPath.IsDirWritable() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message(
                fmt::format( "save path '{}' could not be opened", schematicPath.GetFullPath().ToStdString() ) );
        return tl::unexpected( e );
    }

    if( schematicPath.FileExists() && ( !schematicPath.IsFileWritable() || !aCtx.Request.options().overwrite() ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "save path '{}' exists and cannot be overwritten",
                                          schematicPath.GetFullPath().ToStdString() ) );
        return tl::unexpected( e );
    }

    if( schematicPath.GetExt() != FILEEXT::KiCadSchematicFileExtension )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "save path '{}' must have a kicad_sch extension",
                                          schematicPath.GetFullPath().ToStdString() ) );
        return tl::unexpected( e );
    }

    bool includeProject = true;

    if( aCtx.Request.has_options() )
        includeProject = aCtx.Request.options().include_project();

    if( !context()->SaveSchematicCopy( schematicPath.GetFullPath(), includeProject ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "failed to save schematic copy" );
        return tl::unexpected( e );
    }

    return google::protobuf::Empty();
}


HANDLER_RESULT<google::protobuf::Empty>
API_HANDLER_SCH::handleRevertDocument( const HANDLER_CONTEXT<RevertDocument>& aCtx )
{
    if( aCtx.Request.document().type() != DocumentType::DOCTYPE_SCHEMATIC )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

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

    wxFileName fn = project().AbsolutePath( schematic()->GetFileName() );

    if( frame()->GetCurrentSheet().Last() != &schematic()->Root() )
    {
        SCH_SHEET_PATH rootSheetPath = schematic()->Hierarchy().at( 0 );
        frame()->GetToolManager()->RunAction<SCH_SHEET_PATH*>( SCH_ACTIONS::changeSheet, &rootSheetPath );
    }

    SCH_SCREENS screenList( schematic()->Root() );

    for( SCH_SCREEN* screen = screenList.GetFirst(); screen; screen = screenList.GetNext() )
        screen->SetContentModified( false );

    frame()->ReleaseFile();
    frame()->OpenProjectFiles( std::vector<wxString>( 1, fn.GetFullPath() ), KICTL_REVERT );

    bumpRevision();
    return google::protobuf::Empty();
}


void API_HANDLER_SCH::collectErcMarkers( ErcResultsResponse& aResponse ) const
{
    uint32_t    errors = 0, warnings = 0, exclusions = 0;
    SCH_SCREENS screens( schematic()->Root() );

    for( SCH_SCREEN* screen = screens.GetFirst(); screen; screen = screens.GetNext() )
    {
        for( SCH_ITEM* item : screen->Items().OfType( SCH_MARKER_T ) )
        {
            const SCH_MARKER* marker = static_cast<const SCH_MARKER*>( item );

            if( !marker->GetRCItem() )
                continue;

            google::protobuf::Any any;
            marker->Serialize( any );

            kiapi::schematic::ErcMarker* msg = aResponse.add_markers();
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
        }
    }

    aResponse.set_error_count( errors );
    aResponse.set_warning_count( warnings );
    aResponse.set_exclusion_count( exclusions );
}


HANDLER_RESULT<ErcResultsResponse> API_HANDLER_SCH::handleGetErcMarkers( const HANDLER_CONTEXT<GetErcMarkers>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.schematic() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    ErcResultsResponse response;
    collectErcMarkers( response );
    return response;
}


HANDLER_RESULT<ErcResultsResponse> API_HANDLER_SCH::handleRunSchematicJobErc(
        const HANDLER_CONTEXT<RunSchematicJobErc>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.schematic() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    // Markers are rebuilt from scratch, which would leave staged changes pointing at freed items
    for( const auto& [client, commit] : m_commits )
    {
        if( commit.second && !commit.second->Empty() )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BUSY );
            e.set_error_message( fmt::format( "cannot run ERC while client '{}' has uncommitted changes", client ) );
            return tl::unexpected( e );
        }
    }

    // An asynchronous job runs on the registry's worker thread and shares this document's
    // PROJECT, its symbol library adapter and the KiCad thread pool with the checker, which then
    // rebuilds the schematic's markers underneath it.  Let the queue drain first, the same way a
    // synchronous job and IFACE::closeCurrentDocument do.
    API_JOB_REGISTRY::Instance().WaitForIdle();

    SCHEMATIC* sch = schematic();

    // Running ERC requires libraries be loaded, so make sure they have been
    SYMBOL_LIBRARY_ADAPTER* adapter = PROJECT_SCH::SymbolLibAdapter( &sch->Project() );
    adapter->AsyncLoad();
    adapter->BlockUntilLoaded();

    sch->RecordERCExclusions();

    SCH_SCREENS screens( sch->Root() );
    screens.DeleteAllMarkers( MARKER_BASE::MARKER_ERC, true );

    // The drawing sheet proxy lets the text-variable checks see the sheet, as the CLI job does
    SCH_SCREEN* root = sch->RootScreen();

    std::unique_ptr<DS_PROXY_VIEW_ITEM> drawingSheet = std::make_unique<DS_PROXY_VIEW_ITEM>(
            schIUScale, &root->GetPageSettings(), &sch->Project(), &root->GetTitleBlock(), sch->GetProperties() );

    drawingSheet->SetPageNumber( TO_UTF8( root->GetPageNumber() ) );
    drawingSheet->SetSheetCount( root->GetPageCount() );
    drawingSheet->SetFileName( TO_UTF8( root->GetFileName() ) );
    drawingSheet->SetColorLayer( LAYER_SCHEMATIC_DRAWINGSHEET );
    drawingSheet->SetPageBorderColorLayer( LAYER_SCHEMATIC_PAGE_LIMITS );
    drawingSheet->SetIsFirstPage( root->GetVirtualPageNumber() == 1 );

    wxString currentVariant = sch->GetCurrentVariant();
    drawingSheet->SetVariantName( TO_UTF8( currentVariant ) );
    drawingSheet->SetVariantDesc( TO_UTF8( sch->GetVariantDescription( currentVariant ) ) );
    drawingSheet->SetSheetName( "" );
    drawingSheet->SetSheetPath( "" );

    KIWAY*  kiway = m_context->GetKiway();
    KIFACE* cvpcb = kiway ? kiway->KiFACE( KIWAY::FACE_CVPCB ) : nullptr;

    // RunTests only rebuilds connectivity when it has a frame; do it here so that every headless
    // run checks the same graph (RunERC leaves state behind that changes a second run otherwise)
    if( !m_frame )
    {
        SCH_COMMIT dummyCommit( toolManager() );
        sch->RecalculateConnections( &dummyCommit, NO_CLEANUP, toolManager() );
    }

    ERC_TESTER tester( sch );

    // RunTests resolves the recorded exclusions against the new markers when it finishes
    tester.RunTests( drawingSheet.get(), frame(), cvpcb, &sch->Project(), nullptr );

    if( m_frame && frame()->GetCanvas() )
    {
        for( SCH_ITEM* marker : frame()->GetScreen()->Items().OfType( SCH_MARKER_T ) )
        {
            frame()->GetCanvas()->GetView()->Remove( marker );
            frame()->GetCanvas()->GetView()->Add( marker );
        }

        frame()->GetCanvas()->Refresh();
    }

    bumpRevision();

    ErcResultsResponse response;
    collectErcMarkers( response );
    return response;
}


HANDLER_RESULT<Empty> API_HANDLER_SCH::handleSetErcMarkerExcluded( const HANDLER_CONTEXT<SetErcMarkerExcluded>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.schematic() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    SCH_SCREENS              screens( schematic()->Root() );
    std::vector<SCH_MARKER*> markers;

    for( const types::KIID& id : aCtx.Request.markers() )
    {
        KIID        kiid( id.value() );
        SCH_MARKER* found = nullptr;

        for( SCH_SCREEN* screen = screens.GetFirst(); screen && !found; screen = screens.GetNext() )
        {
            for( SCH_ITEM* item : screen->Items().OfType( SCH_MARKER_T ) )
            {
                if( item->m_Uuid == kiid )
                {
                    found = static_cast<SCH_MARKER*>( item );
                    break;
                }
            }
        }

        if( !found )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( fmt::format( "marker {} not found in the schematic", id.value() ) );
            return tl::unexpected( e );
        }

        markers.push_back( found );
    }

    wxString comment = wxString::FromUTF8( aCtx.Request.comment() );

    for( SCH_MARKER* marker : markers )
    {
        marker->SetExcluded( aCtx.Request.excluded(), aCtx.Request.excluded() ? comment : wxString() );

        if( m_frame && frame()->GetCanvas() )
            frame()->GetCanvas()->GetView()->Update( marker );
    }

    schematic()->RecordERCExclusions();
    bumpRevision();

    return Empty();
}


ErcSeveritiesResponse API_HANDLER_SCH::ercSeverities() const
{
    ErcSeveritiesResponse response;
    ERC_SETTINGS&         settings = schematic()->ErcSettings();

    for( const RC_ITEM& item : ERC_ITEM::GetItemsWithSeverities() )
    {
        ERCE_T code = static_cast<ERCE_T>( item.GetErrorCode() );

        // That list is what the Schematic Setup severities panel draws, so it carries the
        // section headings too.  They have no error code and no severity to report.
        if( code == ERCE_UNSPECIFIED )
            continue;

        kiapi::schematic::ErcSeveritySetting* setting = response.add_severities();
        setting->set_rule_type( ToProtoEnum<ERCE_T, kiapi::schematic::ErcErrorType>( code ) );
        setting->set_severity( ToProtoEnum<SEVERITY, types::RuleSeverity>( settings.GetSeverity( code ) ) );
    }

    return response;
}


HANDLER_RESULT<ErcSeveritiesResponse> API_HANDLER_SCH::handleGetErcSeverities(
        const HANDLER_CONTEXT<GetErcSeverities>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.schematic() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    return ercSeverities();
}


HANDLER_RESULT<ErcSeveritiesResponse> API_HANDLER_SCH::handleSetErcSeverities(
        const HANDLER_CONTEXT<SetErcSeverities>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.schematic() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    const std::set<SEVERITY>  permitted( { RPT_SEVERITY_ERROR, RPT_SEVERITY_WARNING, RPT_SEVERITY_IGNORE } );
    std::map<int, SEVERITY>   changes;

    for( const kiapi::schematic::ErcSeveritySetting& setting : aCtx.Request.severities() )
    {
        ERCE_T   code = FromProtoEnum<ERCE_T, kiapi::schematic::ErcErrorType>( setting.rule_type() );
        SEVERITY severity = FromProtoEnum<SEVERITY, types::RuleSeverity>( setting.severity() );

        if( setting.rule_type() == kiapi::schematic::ERCET_UNKNOWN || !permitted.contains( severity ) )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( "ERC severities need a valid rule type and a severity of error, warning, "
                                 "or ignore" );
            return tl::unexpected( e );
        }

        changes[code] = severity;
    }

    ERC_SETTINGS& settings = schematic()->ErcSettings();

    for( const auto& [code, severity] : changes )
        settings.m_ERCSeverities[code] = severity;

    if( !changes.empty() )
        bumpRevision();

    publishProjectChanged( kiapi::common::events::PCK_SETTINGS, aCtx.ClientName );

    return ercSeverities();
}


HANDLER_RESULT<SavedDocumentResponse>
API_HANDLER_SCH::handleSaveDocumentToString( const HANDLER_CONTEXT<SaveDocumentToString>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    // One sheet file per request: the one the document names (root by default)
    std::optional<SCH_SHEET_PATH> sheet = resolveSheet( aCtx.Request.document() );

    if( !sheet || !sheet->Last() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "the schematic has no sheet to save" );
        return tl::unexpected( e );
    }

    STRING_FORMATTER   formatter;
    SCH_IO_KICAD_SEXPR plugin;

    try
    {
        plugin.FormatSchematicToFormatter( &formatter, sheet->Last(), schematic() );
    }
    catch( const IO_ERROR& ioe )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "failed to format schematic: {}", ioe.What().ToUTF8().data() ) );
        return tl::unexpected( e );
    }

    std::string prettyData = formatter.GetString();
    KICAD_FORMAT::Prettify( prettyData, KICAD_FORMAT::FORMAT_MODE::COMPACT_TEXT_PROPERTIES );

    SavedDocumentResponse response;
    response.mutable_document()->CopyFrom( aCtx.Request.document() );
    response.set_contents( prettyData );
    return response;
}


HANDLER_RESULT<SavedSelectionResponse>
API_HANDLER_SCH::handleSaveItemsToString( const HANDLER_CONTEXT<SaveItemsToString>& aCtx )
{
    HANDLER_RESULT<std::optional<KIID>> containerResult = validateItemHeaderDocument( aCtx.Request.header() );

    if( !containerResult )
        return tl::unexpected( containerResult.error() );

    std::optional<SCH_SHEET_PATH> sheet = resolveSheet( aCtx.Request.header().document() );

    if( !sheet || !sheet->LastScreen() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "the schematic has no sheet to save from" );
        return tl::unexpected( e );
    }

    SavedSelectionResponse response;
    SCH_SELECTION          selection( sheet->LastScreen() );

    for( const types::KIID& id : aCtx.Request.items() )
    {
        SCH_SHEET_PATH           itemPath;
        std::optional<SCH_ITEM*> item = getItemById( KIID( id.value() ), &itemPath );

        if( !item || itemPath.LastScreen() != sheet->LastScreen() )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( fmt::format( "item {} does not exist on the requested sheet", id.value() ) );
            return tl::unexpected( e );
        }

        selection.Add( *item );
        response.add_ids()->set_value( id.value() );
    }

    // The clipboard format: a kicad_sch container carrying the items' library symbols and
    // instance data for the given sheet path
    STRING_FORMATTER   formatter;
    SCH_IO_KICAD_SEXPR plugin;

    plugin.Format( &selection, &*sheet, *schematic(), &formatter, true );

    std::string prettyData = formatter.GetString();
    KICAD_FORMAT::Prettify( prettyData, KICAD_FORMAT::FORMAT_MODE::COMPACT_TEXT_PROPERTIES );

    response.set_contents( prettyData );
    return response;
}


HANDLER_RESULT<CreateItemsResponse>
API_HANDLER_SCH::handleParseAndCreateItemsFromString( const HANDLER_CONTEXT<ParseAndCreateItemsFromString>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( aCtx.Request.contents().empty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "ParseAndCreateItemsFromString requires contents" );
        return tl::unexpected( e );
    }

    std::optional<SCH_SHEET_PATH> targetPath = resolveSheet( aCtx.Request.document() );

    if( !targetPath || !targetPath->LastScreen() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "the schematic has no sheet to create items on" );
        return tl::unexpected( e );
    }

    SCH_SCREEN* targetScreen = targetPath->LastScreen();

    // Parse into a scratch sheet, as the Paste action does.  The screen is owned by the sheet.
    std::string contents = aCtx.Request.contents();

    // The clipboard format is the bare item list; a whole sheet as written by
    // SaveDocumentToString (or "(kicad_sch)" by hand) is unwrapped and its file-level tokens
    // dropped so that it pastes too
    {
        wxString text = wxString::FromUTF8( contents );
        text.Trim( false ).Trim( true );

        if( text.StartsWith( wxS( "(kicad_sch" ) ) && text.EndsWith( wxS( ")" ) ) )
        {
            text = text.Mid( wxString( wxS( "(kicad_sch" ) ).length() );
            text.RemoveLast();

            // The file-level tokens are single lines in pretty-printed text
            wxString      filtered;
            wxArrayString lines = wxSplit( text, '\n', '\0' );

            for( const wxString& line : lines )
            {
                wxString trimmed = line;
                trimmed.Trim( false ).Trim( true );

                if( trimmed.EndsWith( wxS( ")" ) )
                    && ( trimmed.StartsWith( wxS( "(version " ) ) || trimmed.StartsWith( wxS( "(generator " ) )
                         || trimmed.StartsWith( wxS( "(generator_version " ) ) || trimmed.StartsWith( wxS( "(paper " ) ) ) )
                {
                    continue;
                }

                filtered += line + wxS( "\n" );
            }

            contents = filtered.ToUTF8();
        }
    }

    STRING_LINE_READER reader( contents, "ParseAndCreateItemsFromString" );
    SCH_IO_KICAD_SEXPR plugin;
    SCH_SHEET          tempSheet;
    SCH_SCREEN*        tempScreen = new SCH_SCREEN( schematic() );
    tempSheet.SetScreen( tempScreen );

    try
    {
        plugin.LoadContent( reader, &tempSheet );
    }
    catch( const IO_ERROR& ioe )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "contents could not be parsed as a schematic: {}",
                                          ioe.What().ToUTF8().data() ) );
        return tl::unexpected( e );
    }

    std::vector<SCH_ITEM*> items;

    for( SCH_ITEM* item : tempScreen->Items() )
    {
        if( item->Type() != SCH_MARKER_T )
            items.push_back( item );
    }

    // Pasting a copy of items that are already in the schematic must not collide with them; like
    // the Paste action, everything gets new ids in that case
    bool conflict = std::ranges::any_of( items,
                                         [&]( SCH_ITEM* aItem )
                                         {
                                             return getItemById( aItem->m_Uuid ).has_value();
                                         } );

    SCH_SHEET_LIST hierarchy = schematic()->Hierarchy();
    wxString       destFile = targetScreen->GetFileName();
    CreateItemsResponse response;

    // The scratch screen must not free what is handed to the commit
    tempScreen->Clear( false );

    SCH_COMMIT* commit = static_cast<SCH_COMMIT*>( getCurrentCommit( aCtx.ClientName ) );
    bool        anyCreated = false;

    for( SCH_ITEM* item : items )
    {
        std::unique_ptr<SCH_ITEM> owned( item );
        ItemStatus                status;
        google::protobuf::Any     packedInput;

        if( conflict )
        {
            const_cast<KIID&>( item->m_Uuid ) = KIID();

            item->RunOnChildren(
                    []( SCH_ITEM* aChild )
                    {
                        const_cast<KIID&>( aChild->m_Uuid ) = KIID();
                    },
                    RECURSE_MODE::RECURSE );
        }

        if( item->Type() == SCH_SYMBOL_T )
        {
            SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );

            // The pasted text carries its own lib_symbols (an exact copy of what was copied, so
            // the sheet's cache is not rewritten); fall back to the target sheet's
            const LIB_SYMBOL* source = nullptr;
            wxString          libName = symbol->GetSchSymbolLibraryName();

            if( auto it = tempScreen->GetLibSymbols().find( libName ); it != tempScreen->GetLibSymbols().end() )
                source = it->second;
            else if( auto it = targetScreen->GetLibSymbols().find( libName ); it != targetScreen->GetLibSymbols().end() )
                source = it->second;

            if( source )
                symbol->SetLibSymbol( new LIB_SYMBOL( *source ) );

            // A placement on the target sheet: the pasted reference and unit
            SCH_SYMBOL_INSTANCE instance;

            if( !symbol->GetInstance( instance, targetPath->Path() ) )
            {
                instance.m_Path = targetPath->Path();
                instance.m_Reference = symbol->GetField( FIELD_T::REFERENCE )->GetText();
                instance.m_Unit = symbol->GetUnit();
                symbol->AddHierarchicalReference( instance );
            }
        }
        else if( item->Type() == SCH_SHEET_T )
        {
            SCH_SHEET* sheet = static_cast<SCH_SHEET*>( item );

            if( !sheet->GetScreen() )
                sheet->SetScreen( new SCH_SCREEN( schematic() ) );

            if( !destFile.IsEmpty() && hierarchy.TestForRecursion( SCH_SHEET_LIST( sheet ), destFile ) )
            {
                status.set_code( ItemStatusCode::ISC_INVALID_DATA );
                status.set_error_message( "sheet would create a recursive hierarchy" );
                packSchItem( packedInput, item, *targetPath );
                ItemCreationResult* itemResult = response.add_created_items();
                *itemResult->mutable_status() = status;
                *itemResult->mutable_item() = packedInput;
                continue;
            }
        }

        if( item->IsConnectable() )
            item->SetConnectivityDirty();

        // Like the Paste action, pasted items start unlocked
        item->SetLocked( false );

        commit->Add( owned.release(), targetScreen );
        anyCreated = true;

        status.set_code( ItemStatusCode::ISC_OK );

        ItemCreationResult* itemResult = response.add_created_items();
        *itemResult->mutable_status() = status;

        if( !packSchItem( *itemResult->mutable_item(), item, *targetPath ) )
            item->Serialize( *itemResult->mutable_item() );
    }

    if( anyCreated && !m_activeClients.contains( aCtx.ClientName ) )
        pushCurrentCommit( aCtx.ClientName, _( "Pasted items via API" ) );

    if( m_frame && anyCreated )
        frame()->RecalculateConnections( nullptr, LOCAL_CLEANUP );

    response.set_status( ItemRequestStatus::IRS_OK );
    return response;
}


HANDLER_RESULT<GetOpenDocumentsResponse> API_HANDLER_SCH::handleGetOpenDocuments(
        const HANDLER_CONTEXT<GetOpenDocuments>& aCtx )
{
    if( aCtx.Request.type() != DocumentType::DOCTYPE_SCHEMATIC )
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


std::optional<DocumentSpecifier> API_HANDLER_SCH::Document() const
{
    common::types::DocumentSpecifier doc;

    doc.set_type( DocumentType::DOCTYPE_SCHEMATIC );

    if( std::optional<SCH_SHEET_PATH> path = m_context->GetCurrentSheet() )
        PackSheetPath( *doc.mutable_sheet_path(), path->Path() );

    PackProject( *doc.mutable_project(), m_context->Prj() );

    return doc;
}


HANDLER_RESULT<GetDocumentModifiedStateResponse>
API_HANDLER_SCH::handleGetDocumentModifiedState( const HANDLER_CONTEXT<GetDocumentModifiedState>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    GetDocumentModifiedStateResponse response;

    if( aCtx.Request.document().has_sheet_path() )
    {
        KIID_PATH path = UnpackSheetPath( aCtx.Request.document().sheet_path() );

        std::optional<SCH_SHEET_PATH> sheetPath = schematic()->Hierarchy().GetSheetPathByKIIDPath( path );

        if( !sheetPath )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( "the requested sheet path is not valid for this schematic" );
            return tl::unexpected( e );
        }

        if( const SCH_SCREEN* screen = sheetPath->LastScreen() )
        {
            response.set_state( screen->IsContentModified() ? DocumentModifiedState::DMS_MODIFIED
                                                            : DocumentModifiedState::DMS_UNMODIFIED );
        }

        return response;
    }

    if( !schematic()->HasHierarchy() )
        schematic()->RefreshHierarchy();

    response.set_state( schematic()->Hierarchy().IsModified() ? DocumentModifiedState::DMS_MODIFIED
                                                              : DocumentModifiedState::DMS_UNMODIFIED );
    return response;
}


void API_HANDLER_SCH::filterValidSchTypes( std::set<KICAD_T>& aTypeList )
{
    std::erase_if( aTypeList,
                   []( KICAD_T aType )
                   {
                       return !s_allowedTypes.contains( aType );
                   } );
}


HANDLER_RESULT<GetItemsResponse> API_HANDLER_SCH::handleGetItems( const HANDLER_CONTEXT<GetItems>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<std::optional<KIID>> valid = validateItemHeaderDocument( aCtx.Request.header() );
        !valid.has_value() )
    {
        return tl::unexpected( valid.error() );
    }

    std::set<KICAD_T> typesRequested, typesInserted;

    for( KICAD_T type : parseRequestedItemTypes( aCtx.Request.types() ) )
        typesRequested.insert( type );

    filterValidSchTypes( typesRequested );

    if( typesRequested.empty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "none of the requested types are valid for a Schematic object" );
        return tl::unexpected( e );
    }

    SCH_SHEET_LIST hierarchy = schematic()->Hierarchy();
    std::optional<SCH_SHEET_PATH> pathFilter;

    if( aCtx.Request.header().document().has_sheet_path() )
    {
        KIID_PATH kp = UnpackSheetPath( aCtx.Request.header().document().sheet_path() );
        pathFilter = hierarchy.GetSheetPathByKIIDPath( kp );
    }

    std::map<KICAD_T, std::vector<std::pair<EDA_ITEM*, SCH_SHEET_PATH>>> itemMap;

    auto processScreen =
        [&]( const SCH_SHEET_PATH& aPath )
        {
            const SCH_SCREEN* aScreen = aPath.LastScreen();

            for( SCH_ITEM* aItem : aScreen->Items() )
            {
                itemMap[ aItem->Type() ].emplace_back( aItem, aPath );

                aItem->RunOnChildren(
                        [&]( SCH_ITEM* aChild )
                        {
                            itemMap[ aChild->Type() ].emplace_back( aChild, aPath );
                        },
                        RECURSE_MODE::NO_RECURSE );
            }
        };

    if( pathFilter )
    {
        processScreen( *pathFilter );
    }
    else
    {
        for( const SCH_SHEET_PATH& path : hierarchy )
            processScreen( path );
    }

    GetItemsResponse response;
    google::protobuf::Any any;

    std::vector<std::pair<EDA_ITEM*, SCH_SHEET_PATH>> ordered;

    for( KICAD_T type : parseRequestedItemTypes( aCtx.Request.types() ) )
    {
        if( !s_allowedTypes.contains( type ) )
            continue;

        if( !typesInserted.insert( type ).second )
            continue;

        ordered.insert( ordered.end(), itemMap[type].begin(), itemMap[type].end() );
    }

    windowItems( aCtx.Request, ordered, response,
                 []( const std::pair<EDA_ITEM*, SCH_SHEET_PATH>& aEntry ) -> const EDA_ITEM*
                 {
                     return aEntry.first;
                 } );

    for( const auto& [item, itemPath] : ordered )
    {
        if( packSchItem( any, static_cast<SCH_ITEM*>( item ), itemPath ) )
            response.mutable_items()->Add( std::move( any ) );
    }

    response.set_status( ItemRequestStatus::IRS_OK );
    return response;
}


std::map<KICAD_T, uint32_t> API_HANDLER_SCH::countItems( const DocumentSpecifier& aDocument )
{
    std::map<KICAD_T, uint32_t> counts;
    SCH_SHEET_LIST              hierarchy = schematic()->Hierarchy();

    auto countScreen =
            [&]( const SCH_SHEET_PATH& aPath )
            {
                for( SCH_ITEM* item : aPath.LastScreen()->Items() )
                {
                    if( s_allowedTypes.contains( item->Type() ) )
                        ++counts[item->Type()];

                    item->RunOnChildren(
                            [&]( SCH_ITEM* aChild )
                            {
                                if( s_allowedTypes.contains( aChild->Type() ) )
                                    ++counts[aChild->Type()];
                            },
                            RECURSE_MODE::NO_RECURSE );
                }
            };

    // The sheet the document names, or every sheet, as GetItems does
    if( aDocument.has_sheet_path() )
    {
        KIID_PATH kp = UnpackSheetPath( aDocument.sheet_path() );

        if( std::optional<SCH_SHEET_PATH> path = hierarchy.GetSheetPathByKIIDPath( kp ) )
            countScreen( *path );
    }
    else
    {
        for( const SCH_SHEET_PATH& path : hierarchy )
            countScreen( path );
    }

    return counts;
}


HANDLER_RESULT<GetItemsResponse> API_HANDLER_SCH::handleGetItemsById( const HANDLER_CONTEXT<GetItemsById>& aCtx )
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

    SCH_SHEET_LIST hierarchy = schematic()->Hierarchy();
    std::optional<SCH_SHEET_PATH> pathFilter;

    if( aCtx.Request.header().document().has_sheet_path() )
    {
        KIID_PATH kp = UnpackSheetPath( aCtx.Request.header().document().sheet_path() );
        pathFilter = hierarchy.GetSheetPathByKIIDPath( kp );
    }

    GetItemsResponse response;
    SCH_ITEM* item = nullptr;
    google::protobuf::Any any;

    for( const types::KIID& idProto : aCtx.Request.items() )
    {
        KIID id( idProto.value() );

        SCH_SHEET_PATH itemPath;

        if( pathFilter )
        {
            item = pathFilter->ResolveItem( id );
            itemPath = *pathFilter;
        }
        else
        {
            item = hierarchy.ResolveItem( id, &itemPath, true );
        }

        if( !item || !s_allowedTypes.contains( item->Type() ) )
            continue;

        if( item->Type() == SCH_SYMBOL_T )
        {
            kiapi::schematic::types::SchematicSymbolInstance symbol;

            if( !PackSymbol( &symbol, static_cast<SCH_SYMBOL*>( item ), itemPath ) )
                continue;

            any.PackFrom( symbol );
        }
        else if( item->Type() == SCH_SHEET_T )
        {
            kiapi::schematic::types::SheetSymbol sheet;

            if( !PackSheet( &sheet, static_cast<SCH_SHEET*>( item ), itemPath ) )
                continue;

            any.PackFrom( sheet );
        }
        else
        {
            item->Serialize( any );
        }

        response.mutable_items()->Add( std::move( any ) );
    }

    if( response.items().empty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "none of the requested IDs were found or valid" );
        return tl::unexpected( e );
    }

    response.set_status( ItemRequestStatus::IRS_OK );
    return response;
}


HANDLER_RESULT<SelectionResponse>
API_HANDLER_SCH::handleGetSelection( const HANDLER_CONTEXT<GetSelection>& aCtx )
{
    if( std::optional<ApiResponseStatus> headless = checkForHeadless( "GetSelection" ) )
        return tl::unexpected( *headless );

    // validateItemHeaderDocument already answers AS_UNHANDLED for another editor's
    // document type; anything else it reports is a real error for this client
    if( HANDLER_RESULT<std::optional<KIID>> header = validateItemHeaderDocument( aCtx.Request.header() );
        !header )
    {
        return tl::unexpected( header.error() );
    }

    std::set<KICAD_T> filter;

    for( KICAD_T type : parseRequestedItemTypes( aCtx.Request.types() ) )
        filter.insert( type );

    SCH_SELECTION_TOOL* tool = m_context->GetToolManager()->GetTool<SCH_SELECTION_TOOL>();
    SCH_SHEET_PATH path = m_context->GetCurrentSheet().value_or( SCH_SHEET_PATH() );

    SelectionResponse response;
    google::protobuf::Any any;

    for( EDA_ITEM* item : tool->GetSelection() )
    {
        if( filter.empty() || filter.contains( item->Type() ) )
        {
            if( packSchItem( any, static_cast<SCH_ITEM*>( item ), path ) )
                response.mutable_items()->Add( std::move( any ) );
        }
    }

    return response;
}


HANDLER_RESULT<Empty>
API_HANDLER_SCH::handleClearSelection( const HANDLER_CONTEXT<ClearSelection>& aCtx )
{
    if( std::optional<ApiResponseStatus> headless = checkForHeadless( "ClearSelection" ) )
        return tl::unexpected( *headless );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    // validateItemHeaderDocument already answers AS_UNHANDLED for another editor's
    // document type; anything else it reports is a real error for this client
    if( HANDLER_RESULT<std::optional<KIID>> header = validateItemHeaderDocument( aCtx.Request.header() );
        !header )
    {
        return tl::unexpected( header.error() );
    }

    m_context->GetToolManager()->RunAction( ACTIONS::selectionClear );
    frame()->Refresh();

    return Empty();
}


HANDLER_RESULT<SelectionResponse>
API_HANDLER_SCH::handleAddToSelection( const HANDLER_CONTEXT<AddToSelection>& aCtx )
{
    if( std::optional<ApiResponseStatus> headless = checkForHeadless( "AddToSelection" ) )
        return tl::unexpected( *headless );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    // validateItemHeaderDocument already answers AS_UNHANDLED for another editor's
    // document type; anything else it reports is a real error for this client
    if( HANDLER_RESULT<std::optional<KIID>> header = validateItemHeaderDocument( aCtx.Request.header() );
        !header )
    {
        return tl::unexpected( header.error() );
    }

    SCH_SELECTION_TOOL* tool = m_context->GetToolManager()->GetTool<SCH_SELECTION_TOOL>();
    SCH_SHEET_PATH current = m_context->GetCurrentSheet().value_or( SCH_SHEET_PATH() );

    EDA_ITEMS toAdd;

    for( const types::KIID& id : aCtx.Request.items() )
    {
        SCH_SHEET_PATH itemPath;

        // Selection only operates on the currently-displayed sheet; off-sheet items are skipped
        if( std::optional<SCH_ITEM*> item = getItemById( KIID( id.value() ), &itemPath );
            item && itemPath == current )
        {
            toAdd.push_back( *item );
        }
    }

    tool->AddItemsToSel( &toAdd );
    frame()->Refresh();

    SelectionResponse response;
    google::protobuf::Any any;

    for( EDA_ITEM* item : tool->GetSelection() )
    {
        if( packSchItem( any, static_cast<SCH_ITEM*>( item ), current ) )
            response.mutable_items()->Add( std::move( any ) );
    }

    return response;
}


HANDLER_RESULT<SelectionResponse>
API_HANDLER_SCH::handleRemoveFromSelection( const HANDLER_CONTEXT<RemoveFromSelection>& aCtx )
{
    if( std::optional<ApiResponseStatus> headless = checkForHeadless( "RemoveFromSelection" ) )
        return tl::unexpected( *headless );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    // validateItemHeaderDocument already answers AS_UNHANDLED for another editor's
    // document type; anything else it reports is a real error for this client
    if( HANDLER_RESULT<std::optional<KIID>> header = validateItemHeaderDocument( aCtx.Request.header() );
        !header )
    {
        return tl::unexpected( header.error() );
    }

    SCH_SELECTION_TOOL* tool = m_context->GetToolManager()->GetTool<SCH_SELECTION_TOOL>();
    SCH_SHEET_PATH current = m_context->GetCurrentSheet().value_or( SCH_SHEET_PATH() );

    EDA_ITEMS toRemove;

    for( const types::KIID& id : aCtx.Request.items() )
    {
        SCH_SHEET_PATH itemPath;

        if( std::optional<SCH_ITEM*> item = getItemById( KIID( id.value() ), &itemPath );
            item && itemPath == current )
        {
            toRemove.push_back( *item );
        }
    }

    tool->RemoveItemsFromSel( &toRemove );
    frame()->Refresh();

    SelectionResponse response;
    google::protobuf::Any any;

    for( EDA_ITEM* item : tool->GetSelection() )
    {
        if( packSchItem( any, static_cast<SCH_ITEM*>( item ), current ) )
            response.mutable_items()->Add( std::move( any ) );
    }

    return response;
}


HANDLER_RESULT<std::unique_ptr<EDA_ITEM>> API_HANDLER_SCH::createItemForType( KICAD_T aType, EDA_ITEM* aContainer )
{
    if( !aContainer )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "Tried to create an item in a null container" );
        return tl::unexpected( e );
    }

    if( !s_allowedTypes.contains( aType ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "type {} is not supported by the schematic API handler",
                                          magic_enum::enum_name( aType ) ) );
        return tl::unexpected( e );
    }

    if( aType == SCH_PIN_T && !dynamic_cast<SCH_SYMBOL*>( aContainer ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "Tried to create a pin in {}, which is not a symbol",
                                          aContainer->GetFriendlyName().ToStdString() ) );
        return tl::unexpected( e );
    }
    else if( aType == SCH_SHEET_T && !dynamic_cast<SCH_SCREEN*>( aContainer ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "Tried to create a sheet symbol in {}, which is not a "
                                          "schematic sheet",
                                          aContainer->GetFriendlyName().ToStdString() ) );
        return tl::unexpected( e );
    }
    else if( aType == SCH_SYMBOL_T && !dynamic_cast<SCH_SCREEN*>( aContainer ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "Tried to create a symbol in {}, which is not a "
                                          "schematic sheet",
                                          aContainer->GetFriendlyName().ToStdString() ) );
        return tl::unexpected( e );
    }

    std::unique_ptr<EDA_ITEM> created = CreateItemForType( aType, aContainer );

    if( created && !created->GetParent() )
        created->SetParent( aContainer );

    if( !created )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "Tried to create an item of type {}, which is unhandled",
                                          magic_enum::enum_name( aType ) ) );
        return tl::unexpected( e );
    }

    return created;
}


HANDLER_RESULT<ItemRequestStatus> API_HANDLER_SCH::handleCreateUpdateItemsInternal( bool aCreate,
        const std::string& aClientName,
        const types::ItemHeader &aHeader,
        const google::protobuf::RepeatedPtrField<google::protobuf::Any>& aItems,
        std::function<void( ItemStatus, google::protobuf::Any )> aItemHandler )
{
    ApiResponseStatus e;

    auto containerResult = validateItemHeaderDocument( aHeader );

    if( !containerResult && containerResult.error().status() == ApiStatusCode::AS_UNHANDLED )
    {
        // No message needed for AS_UNHANDLED; this is an internal flag for the API server
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }
    else if( !containerResult )
    {
        e.CopyFrom( containerResult.error() );
        return tl::unexpected( e );
    }

    SCH_SHEET_LIST hierarchy = schematic()->Hierarchy();
    SCH_SCREEN* targetScreen = schematic()->GetCurrentScreen();
    SCH_SHEET_PATH targetPath = m_context->GetCurrentSheet().value_or( *hierarchy.begin() );

    if( aHeader.document().has_sheet_path() )
    {
        KIID_PATH kp = UnpackSheetPath( aHeader.document().sheet_path() );
        if( std::optional<SCH_SHEET_PATH> path = hierarchy.GetSheetPathByKIIDPath( kp ) )
        {
            targetPath = *path;
            targetScreen = targetPath.LastScreen();
        }
    }

    SCH_COMMIT* commit = static_cast<SCH_COMMIT*>( getCurrentCommit( aClientName ) );
    bool connectivityChanged = false;   // an in-place symbol update invalidated the net graph

    for( const google::protobuf::Any& anyItem : aItems )
    {
        ItemStatus status;
        std::optional<KICAD_T> type = TypeNameFromAny( anyItem );

        if( !type )
        {
            status.set_code( ItemStatusCode::ISC_INVALID_TYPE );
            status.set_error_message( fmt::format( "Could not decode a valid type from {}",
                                                   anyItem.type_url() ) );
            aItemHandler( status, anyItem );
            continue;
        }

        EDA_ITEM* container = targetScreen;

        HANDLER_RESULT<std::unique_ptr<EDA_ITEM>> creationResult = createItemForType( *type, container );

        if( !creationResult )
        {
            status.set_code( ItemStatusCode::ISC_INVALID_TYPE );
            status.set_error_message( creationResult.error().error_message() );
            aItemHandler( status, anyItem );
            continue;
        }

        std::unique_ptr<EDA_ITEM> item( std::move( *creationResult ) );

        bool unpacked = false;

        // Retained past the unpack: the placement data they carry is applied once the item is
        // in the schematic.
        kiapi::schematic::types::SchematicSymbolInstance symbolProto;
        kiapi::schematic::types::SheetSymbol            sheetProto;

        if( *type == SCH_SYMBOL_T )
        {
            unpacked = anyItem.UnpackTo( &symbolProto )
                       && UnpackSymbol( static_cast<SCH_SYMBOL*>( item.get() ), symbolProto );
        }
        else if( *type == SCH_SHEET_T )
        {
            unpacked = anyItem.UnpackTo( &sheetProto );

            if( unpacked )
            {
                SCH_SHEET* sheet = static_cast<SCH_SHEET*>( item.get() );

                if( tl::expected<bool, ApiResponseStatus> result = UnpackSheet( sheet, sheetProto );
                    result.has_value() )
                {
                    unpacked = *result;
                }
                else
                {
                    return tl::unexpected( result.error() );
                }
            }
        }
        else if( SCH_GROUP* group = dynamic_cast<SCH_GROUP*>( item.get() ) )
        {
            unpacked = group->DeserializeGroup( anyItem, commit );
        }
        else
        {
            unpacked = item->Deserialize( anyItem );
        }

        if( !unpacked )
        {
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( fmt::format( "could not unpack {} from request",
                                              item->GetClass().ToStdString() ) );
            return tl::unexpected( e );
        }

        if( std::vector<wxString> removed = item->RemoveConflictingCustomProperties(); !removed.empty() )
        {
            auto as_str =
                []( const wxString& aIn )
                {
                    return std::string( aIn.ToUTF8() );
                };

            status.set_code( ItemStatusCode::ISC_INVALID_DATA );
            status.set_error_message( fmt::format(
                    "Invalid custom properties for item {}: property name(s) '{}' already in use",
                    item->m_Uuid.AsStdString(), fmt::join( std::views::transform( removed, as_str ), ", " ) ) );

            aItemHandler( status, anyItem );
            continue;
        }

        SCH_ITEM* existingItem = nullptr;
        SCH_SHEET_PATH existingPath;

        existingItem = targetPath.ResolveItem( item->m_Uuid );

        if( existingItem )
            existingPath = targetPath;

        if( aCreate && existingItem )
        {
            status.set_code( ItemStatusCode::ISC_EXISTING );
            status.set_error_message( fmt::format( "an item with UUID {} already exists",
                                                   item->m_Uuid.AsStdString() ) );
            aItemHandler( status, anyItem );
            continue;
        }
        else if( !aCreate && !existingItem )
        {
            status.set_code( ItemStatusCode::ISC_NONEXISTENT );
            status.set_error_message( fmt::format( "an item with UUID {} does not exist",
                                                   item->m_Uuid.AsStdString() ) );
            aItemHandler( status, anyItem );
            continue;
        }

        if( !aCreate )
        {
            SCH_SCREEN* itemScreen = existingPath.LastScreen();

            if( itemScreen != targetScreen )
            {
                status.set_code( ItemStatusCode::ISC_INVALID_DATA );
                status.set_error_message( fmt::format( "item {} exists on a different sheet than targeted",
                                                       item->m_Uuid.AsStdString() ) );
                aItemHandler( status, anyItem );
                continue;
            }
        }

        if( *type == SCH_SHEET_T )
        {
            SCH_SHEET* sheet = static_cast<SCH_SHEET*>( item.get() );

            // A new sheet gets the screen of the file it names: an existing file is loaded (or
            // shared, if the hierarchy already uses it) and a missing one is created on disk
            if( aCreate && !sheet->GetScreen() )
            {
                if( wxString error = attachSheetFile( sheet, targetPath ); !error.IsEmpty() )
                {
                    status.set_code( ItemStatusCode::ISC_INVALID_DATA );
                    status.set_error_message( error.ToStdString() );
                    aItemHandler( status, anyItem );
                    continue;
                }
            }

            SCH_SHEET_PATH parentPath;

            if( aCreate )
                parentPath = targetPath;
            else
                parentPath = existingPath;

            wxString destFilePath = parentPath.LastScreen()->GetFileName();

            if( !destFilePath.IsEmpty() )
            {
                SCH_SHEET_LIST schematicSheets = schematic()->Hierarchy();
                SCH_SHEET_LIST loadedSheets( sheet );

                if( schematicSheets.TestForRecursion( loadedSheets, destFilePath ) )
                {
                    status.set_code( ItemStatusCode::ISC_INVALID_DATA );
                    status.set_error_message( "sheet update would create recursive hierarchy" );
                    aItemHandler( status, anyItem );
                    continue;
                }
            }
        }

        status.set_code( ItemStatusCode::ISC_OK );
        google::protobuf::Any newItem;

        if( aCreate )
        {
            SCH_ITEM* createdItem = static_cast<SCH_ITEM*>( item.release() );
            commit->Add( createdItem, targetScreen );

            if( !createdItem )
            {
                e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                e.set_error_message( "could not add the requested item to its parent container" );
                return tl::unexpected( e );
            }

            if( createdItem->Type() == SCH_SYMBOL_T )
            {
                SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( createdItem );
                kiapi::schematic::types::SchematicSymbolInstance packed;

                ApplySymbolInstance( symbol, symbolProto, targetPath, schematic() );

                if( PackSymbol( &packed, symbol, targetPath ) )
                    newItem.PackFrom( packed );
            }
            else if( createdItem->Type() == SCH_SHEET_T )
            {
                SCH_SHEET* sheet = static_cast<SCH_SHEET*>( createdItem );
                kiapi::schematic::types::SheetSymbol packed;

                if( sheetProto.page_number().empty() )
                    sheetProto.set_page_number( hierarchy.GetNextPageNumber().ToUTF8() );

                ApplySheetInstance( sheet, sheetProto, targetPath, schematic() );

                if( PackSheet( &packed, sheet, targetPath ) )
                    newItem.PackFrom( packed );
            }
            else
            {
                createdItem->Serialize( newItem );
            }
        }
        else
        {
            // SwapItemData hands the item the temporary's (empty) instance list, so keep the
            // placements to restore afterwards.
            std::vector<SCH_SYMBOL_INSTANCE> symbolPlacements;
            std::vector<SCH_SHEET_INSTANCE>  sheetPlacements;

            if( existingItem->Type() == SCH_SYMBOL_T )
                symbolPlacements = static_cast<SCH_SYMBOL*>( existingItem )->GetInstances();
            else if( existingItem->Type() == SCH_SHEET_T )
                sheetPlacements = static_cast<SCH_SHEET*>( existingItem )->GetInstances();

            // An unchanged library definition keeps the library symbol the sheet already has, so
            // that the lib_symbols cache is not rewritten under a new name
            if( existingItem->Type() == SCH_SYMBOL_T )
            {
                ReuseUnchangedLibSymbol( static_cast<SCH_SYMBOL*>( item.get() ),
                                         static_cast<SCH_SYMBOL*>( existingItem ), symbolProto, existingPath );
            }

            commit->Modify( existingItem, targetScreen );
            existingItem->SwapItemData( static_cast<SCH_ITEM*>( item.get() ) );

            if( existingItem->IsConnectable() )
            {
                existingItem->SetConnectivityDirty();
                connectivityChanged = true;
            }

            if( existingItem->Type() == SCH_SYMBOL_T )
            {
                SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( existingItem );
                kiapi::schematic::types::SchematicSymbolInstance packed;

                for( const SCH_SYMBOL_INSTANCE& placement : symbolPlacements )
                    symbol->AddHierarchicalReference( placement );

                ApplySymbolInstance( symbol, symbolProto, existingPath, schematic() );

                if( PackSymbol( &packed, symbol, existingPath ) )
                    newItem.PackFrom( packed );
            }
            else if( existingItem->Type() == SCH_SHEET_T )
            {
                SCH_SHEET* sheet = static_cast<SCH_SHEET*>( existingItem );
                kiapi::schematic::types::SheetSymbol packed;

                for( const SCH_SHEET_INSTANCE& placement : sheetPlacements )
                    sheet->AddInstance( placement );

                ApplySheetInstance( sheet, sheetProto, existingPath, schematic() );

                if( PackSheet( &packed, sheet, existingPath ) )
                    newItem.PackFrom( packed );
            }
            else
            {
                existingItem->Serialize( newItem );
            }
        }

        aItemHandler( status, newItem );
    }

    if( !m_activeClients.contains( aClientName ) )
    {
        pushImplicitCommit( aClientName, aCreate ? _( "Created items via API" )
                                                : _( "Modified items via API" ) );
    }

    if( m_frame && connectivityChanged )
        frame()->RecalculateConnections( nullptr, LOCAL_CLEANUP );

    return ItemRequestStatus::IRS_OK;
}


void API_HANDLER_SCH::deleteItemsInternal( std::map<KIID, ItemDeletionStatus>& aItemsToDelete,
                                           const std::string& aClientName )
{
    SCH_SHEET_LIST hierarchy = schematic()->Hierarchy();
    COMMIT* commit = getCurrentCommit( aClientName );

    for( auto& [id, status] : aItemsToDelete )
    {
        SCH_SHEET_PATH path;
        SCH_ITEM* item = hierarchy.ResolveItem( id, &path, true );

        if( !item )
            continue;

        if( !s_allowedTypes.contains( item->Type() ) )
        {
            status = ItemDeletionStatus::IDS_IMMUTABLE;
            continue;
        }

        commit->Remove( item, path.LastScreen() );
        status = ItemDeletionStatus::IDS_OK;
    }

    if( !m_activeClients.contains( aClientName ) )
        pushImplicitCommit( aClientName, _( "Deleted items via API" ) );
}


std::optional<EDA_ITEM*> API_HANDLER_SCH::getItemFromDocument( const DocumentSpecifier& aDocument, const KIID& aId )
{
    if( !validateDocument( aDocument ) )
        return std::nullopt;

    SCH_ITEM* item = schematic()->Hierarchy().ResolveItem( aId, nullptr, true );

    if( !item)
        return std::nullopt;

    return item;
}


std::optional<SCH_SHEET_PATH> API_HANDLER_SCH::resolveSheet( const DocumentSpecifier& aDocument ) const
{
    if( !schematic() || !schematic()->IsValid() )
        return std::nullopt;

    if( aDocument.has_sheet_path() )
    {
        KIID_PATH path = UnpackSheetPath( aDocument.sheet_path() );

        if( std::optional<SCH_SHEET_PATH> resolved = schematic()->Hierarchy().GetSheetPathByKIIDPath( path ) )
            return resolved;
    }

    if( std::optional<SCH_SHEET_PATH> current = m_context->GetCurrentSheet() )
        return current;

    // Headless: the root sheet
    if( schematic()->Hierarchy().empty() )
        return std::nullopt;

    return schematic()->Hierarchy().at( 0 );
}


SCH_SCREEN* API_HANDLER_SCH::resolveScreenFromDocument( const DocumentSpecifier& aDocument ) const
{
    std::optional<SCH_SHEET_PATH> sheet = resolveSheet( aDocument );

    return sheet ? sheet->LastScreen() : nullptr;
}


std::optional<TITLE_BLOCK*> API_HANDLER_SCH::getTitleBlock( const DocumentSpecifier& aDocument )
{
    if( SCH_SCREEN* screen = resolveScreenFromDocument( aDocument ) )
        return &screen->GetTitleBlock();

    return std::nullopt;
}


std::optional<PAGE_INFO> API_HANDLER_SCH::getPageSettings( const DocumentSpecifier& aDocument )
{
    if( SCH_SCREEN* screen = resolveScreenFromDocument( aDocument ) )
        return screen->GetPageSettings();

    return std::nullopt;
}


bool API_HANDLER_SCH::setPageSettings( const DocumentSpecifier& aDocument, const PAGE_INFO& aPageInfo )
{
    if( SCH_SCREEN* screen = resolveScreenFromDocument( aDocument ) )
    {
        screen->SetPageSettings( aPageInfo );
        return true;
    }

    return false;
}


wxString API_HANDLER_SCH::getDrawingSheetFileName()
{
    return BASE_SCREEN::m_DrawingSheetFileName;
}


void API_HANDLER_SCH::setDrawingSheetFileName( const wxString& aFileName )
{
    BASE_SCREEN::m_DrawingSheetFileName = aFileName;
    schematic()->Settings().m_SchDrawingSheetFileName = aFileName;

    if( m_frame )
        frame()->LoadDrawingSheet();
}


void API_HANDLER_SCH::onModified()
{
    API_HANDLER_EDITOR::onModified();

    if( m_frame )
    {
        frame()->Refresh();
        frame()->OnModify();
    }
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_SCH::handleRunSchematicJobExportSvg(
        const HANDLER_CONTEXT<kiapi::schematic::jobs::RunSchematicJobExportSvg>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.job_settings().document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    auto plotJob = std::make_unique<JOB_EXPORT_SCH_PLOT_SVG>();
    plotJob->m_filename = m_context->GetCurrentFileName();

    if( !aCtx.Request.job_settings().output_path().empty() )
        plotJob->SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    const kiapi::schematic::jobs::SchematicPlotSettings& settings = aCtx.Request.plot_settings();

    plotJob->m_drawingSheet = wxString::FromUTF8( settings.drawing_sheet() );
    plotJob->m_defaultFont = wxString::FromUTF8( settings.default_font() );
    plotJob->m_variant = wxString::FromUTF8( settings.variant() );
    plotJob->m_plotAll = settings.plot_all();
    plotJob->m_plotDrawingSheet = settings.plot_drawing_sheet();
    plotJob->m_show_hop_over = settings.show_hop_over();
    plotJob->m_blackAndWhite = settings.black_and_white();
    plotJob->m_useBackgroundColor = settings.use_background_color();
    plotJob->m_minPenWidth = settings.min_pen_width();
    plotJob->m_theme = wxString::FromUTF8( settings.theme() );

    plotJob->m_plotPages.clear();

    for( const std::string& page : settings.plot_pages() )
        plotJob->m_plotPages.push_back( wxString::FromUTF8( page ) );

    if( aCtx.Request.plot_settings().page_size() != kiapi::schematic::jobs::SchematicJobPageSize::SJPS_UNKNOWN )
    {
        plotJob->m_pageSizeSelect = FromProtoEnum<JOB_PAGE_SIZE>( aCtx.Request.plot_settings().page_size() );
    }

    return runSchematicJob( aCtx.Request.job_settings(), std::move( plotJob ) );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_SCH::handleRunSchematicJobExportDxf(
        const HANDLER_CONTEXT<kiapi::schematic::jobs::RunSchematicJobExportDxf>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.job_settings().document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    auto plotJob = std::make_unique<JOB_EXPORT_SCH_PLOT_DXF>();
    plotJob->m_filename = m_context->GetCurrentFileName();

    if( !aCtx.Request.job_settings().output_path().empty() )
        plotJob->SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    const kiapi::schematic::jobs::SchematicPlotSettings& settings = aCtx.Request.plot_settings();

    plotJob->m_drawingSheet = wxString::FromUTF8( settings.drawing_sheet() );
    plotJob->m_defaultFont = wxString::FromUTF8( settings.default_font() );
    plotJob->m_variant = wxString::FromUTF8( settings.variant() );
    plotJob->m_plotAll = settings.plot_all();
    plotJob->m_plotDrawingSheet = settings.plot_drawing_sheet();
    plotJob->m_show_hop_over = settings.show_hop_over();
    plotJob->m_blackAndWhite = settings.black_and_white();
    plotJob->m_useBackgroundColor = settings.use_background_color();
    plotJob->m_minPenWidth = settings.min_pen_width();
    plotJob->m_theme = wxString::FromUTF8( settings.theme() );

    plotJob->m_plotPages.clear();

    for( const std::string& page : settings.plot_pages() )
        plotJob->m_plotPages.push_back( wxString::FromUTF8( page ) );

    if( aCtx.Request.plot_settings().page_size() != kiapi::schematic::jobs::SchematicJobPageSize::SJPS_UNKNOWN )
    {
        plotJob->m_pageSizeSelect = FromProtoEnum<JOB_PAGE_SIZE>( aCtx.Request.plot_settings().page_size() );
    }

    return runSchematicJob( aCtx.Request.job_settings(), std::move( plotJob ) );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_SCH::handleRunSchematicJobExportPdf(
        const HANDLER_CONTEXT<kiapi::schematic::jobs::RunSchematicJobExportPdf>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.job_settings().document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    auto plotJob = std::make_unique<JOB_EXPORT_SCH_PLOT_PDF>( false );
    plotJob->m_filename = m_context->GetCurrentFileName();

    if( !aCtx.Request.job_settings().output_path().empty() )
        plotJob->SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    const kiapi::schematic::jobs::SchematicPlotSettings& settings = aCtx.Request.plot_settings();

    plotJob->m_drawingSheet = wxString::FromUTF8( settings.drawing_sheet() );
    plotJob->m_defaultFont = wxString::FromUTF8( settings.default_font() );
    plotJob->m_variant = wxString::FromUTF8( settings.variant() );
    plotJob->m_plotAll = settings.plot_all();
    plotJob->m_plotDrawingSheet = settings.plot_drawing_sheet();
    plotJob->m_show_hop_over = settings.show_hop_over();
    plotJob->m_blackAndWhite = settings.black_and_white();
    plotJob->m_useBackgroundColor = settings.use_background_color();
    plotJob->m_minPenWidth = settings.min_pen_width();
    plotJob->m_theme = wxString::FromUTF8( settings.theme() );

    plotJob->m_plotPages.clear();

    for( const std::string& page : settings.plot_pages() )
        plotJob->m_plotPages.push_back( wxString::FromUTF8( page ) );

    if( aCtx.Request.plot_settings().page_size() != kiapi::schematic::jobs::SchematicJobPageSize::SJPS_UNKNOWN )
    {
        plotJob->m_pageSizeSelect = FromProtoEnum<JOB_PAGE_SIZE>( aCtx.Request.plot_settings().page_size() );
    }

    plotJob->m_PDFPropertyPopups = aCtx.Request.property_popups();
    plotJob->m_PDFHierarchicalLinks = aCtx.Request.hierarchical_links();
    plotJob->m_PDFMetadata = aCtx.Request.include_metadata();

    return runSchematicJob( aCtx.Request.job_settings(), std::move( plotJob ) );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_SCH::handleRunSchematicJobExportPs(
        const HANDLER_CONTEXT<kiapi::schematic::jobs::RunSchematicJobExportPs>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.job_settings().document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    auto plotJob = std::make_unique<JOB_EXPORT_SCH_PLOT_PS>();
    plotJob->m_filename = m_context->GetCurrentFileName();

    if( !aCtx.Request.job_settings().output_path().empty() )
        plotJob->SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    const kiapi::schematic::jobs::SchematicPlotSettings& settings = aCtx.Request.plot_settings();

    plotJob->m_drawingSheet = wxString::FromUTF8( settings.drawing_sheet() );
    plotJob->m_defaultFont = wxString::FromUTF8( settings.default_font() );
    plotJob->m_variant = wxString::FromUTF8( settings.variant() );
    plotJob->m_plotAll = settings.plot_all();
    plotJob->m_plotDrawingSheet = settings.plot_drawing_sheet();
    plotJob->m_show_hop_over = settings.show_hop_over();
    plotJob->m_blackAndWhite = settings.black_and_white();
    plotJob->m_useBackgroundColor = settings.use_background_color();
    plotJob->m_minPenWidth = settings.min_pen_width();
    plotJob->m_theme = wxString::FromUTF8( settings.theme() );

    plotJob->m_plotPages.clear();

    for( const std::string& page : settings.plot_pages() )
        plotJob->m_plotPages.push_back( wxString::FromUTF8( page ) );

    if( aCtx.Request.plot_settings().page_size() != kiapi::schematic::jobs::SchematicJobPageSize::SJPS_UNKNOWN )
    {
        plotJob->m_pageSizeSelect = FromProtoEnum<JOB_PAGE_SIZE>( aCtx.Request.plot_settings().page_size() );
    }

    return runSchematicJob( aCtx.Request.job_settings(), std::move( plotJob ) );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_SCH::handleRunSchematicJobExportNetlist(
        const HANDLER_CONTEXT<kiapi::schematic::jobs::RunSchematicJobExportNetlist>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.job_settings().document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( aCtx.Request.format() == kiapi::schematic::jobs::SchematicNetlistFormat::SNF_UNKNOWN )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "RunSchematicJobExportNetlist requires a valid format" );
        return tl::unexpected( e );
    }

    std::unique_ptr<JOB_EXPORT_SCH_NETLIST> netlistJobPtr = std::make_unique<JOB_EXPORT_SCH_NETLIST>();
    JOB_EXPORT_SCH_NETLIST&                 netlistJob = *netlistJobPtr;
    netlistJob.m_filename = m_context->GetCurrentFileName();

    if( !aCtx.Request.job_settings().output_path().empty() )
        netlistJob.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    netlistJob.format = FromProtoEnum<JOB_EXPORT_SCH_NETLIST::FORMAT>( aCtx.Request.format() );

    if( !aCtx.Request.variant_name().empty() )
        netlistJob.m_variantNames.emplace_back( wxString::FromUTF8( aCtx.Request.variant_name() ) );

    return runSchematicJob( aCtx.Request.job_settings(), std::move( netlistJobPtr ) );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_SCH::handleRunSchematicJobExportBOM(
        const HANDLER_CONTEXT<kiapi::schematic::jobs::RunSchematicJobExportBOM>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.job_settings().document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    std::unique_ptr<JOB_EXPORT_BOM> bomJobPtr = std::make_unique<JOB_EXPORT_BOM>();
    JOB_EXPORT_BOM&                 bomJob = *bomJobPtr;
    bomJob.m_filename = m_context->GetCurrentFileName();

    if( !aCtx.Request.job_settings().output_path().empty() )
        bomJob.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    // Without a format preset the delimiters are taken as-is, so an omitted one has to be filled
    // in with the value `kicad-cli sch export bom` defaults to; otherwise the export writes
    // unquoted, unseparated rows
    auto delimiter =
            []( const std::string& aValue, const wxString& aDefault )
            {
                return aValue.empty() ? aDefault : wxString::FromUTF8( aValue );
            };

    bomJob.m_bomFmtPresetName = wxString::FromUTF8( aCtx.Request.format().preset_name() );
    bomJob.m_fieldDelimiter = delimiter( aCtx.Request.format().field_delimiter(), wxS( "," ) );
    bomJob.m_stringDelimiter = delimiter( aCtx.Request.format().string_delimiter(), wxS( "\"" ) );
    bomJob.m_refDelimiter = delimiter( aCtx.Request.format().ref_delimiter(), wxS( "," ) );
    bomJob.m_refRangeDelimiter = delimiter( aCtx.Request.format().ref_range_delimiter(), wxS( "-" ) );
    bomJob.m_keepTabs = aCtx.Request.format().keep_tabs();
    bomJob.m_keepLineBreaks = aCtx.Request.format().keep_line_breaks();
    bomJob.m_includeByteOrderMark = aCtx.Request.format().include_byte_order_mark();

    bomJob.m_bomPresetName = wxString::FromUTF8( aCtx.Request.fields().preset_name() );
    bomJob.m_sortField = aCtx.Request.fields().sort_field().empty()
                                 ? wxString( wxS( "Reference" ) )
                                 : wxString::FromUTF8( aCtx.Request.fields().sort_field() );
    bomJob.m_filterString = wxString::FromUTF8( aCtx.Request.fields().filter() );

    switch( aCtx.Request.fields().filter_scope() )
    {
    case kiapi::schematic::jobs::BOMFilterScope::BFS_VISIBLE:
        bomJob.m_filterScope = BOM_FILTER_SCOPE::VISIBLE;
        break;

    case kiapi::schematic::jobs::BOMFilterScope::BFS_ALL:
        bomJob.m_filterScope = BOM_FILTER_SCOPE::ALL;
        break;

    case kiapi::schematic::jobs::BOMFilterScope::BFS_REFERENCE:
    default:
        bomJob.m_filterScope = BOM_FILTER_SCOPE::REFERENCE;
        break;
    }

    if( aCtx.Request.fields().sort_direction() == kiapi::schematic::jobs::BOMSortDirection::BSD_ASCENDING )
    {
        bomJob.m_sortAsc = true;
    }
    else if( aCtx.Request.fields().sort_direction() == kiapi::schematic::jobs::BOMSortDirection::BSD_DESCENDING )
    {
        bomJob.m_sortAsc = false;
    }

    for( const kiapi::schematic::jobs::BOMField& field : aCtx.Request.fields().fields() )
    {
        bomJob.m_fieldsOrdered.emplace_back( wxString::FromUTF8( field.name() ) );
        bomJob.m_fieldsLabels.emplace_back( wxString::FromUTF8( field.label() ) );

        if( field.group_by() )
            bomJob.m_fieldsGroupBy.emplace_back( wxString::FromUTF8( field.name() ) );
    }

    // A request that names neither a preset nor any field would export a BOM with no columns at
    // all, which writes an empty file; give it the columns `kicad-cli sch export bom` defaults to
    if( bomJob.m_bomPresetName.IsEmpty() && bomJob.m_fieldsOrdered.empty() )
    {
        bomJob.m_fieldsOrdered = { wxS( "Reference" ), wxS( "Value" ), wxS( "Footprint" ), wxS( "QUANTITY" ),
                                   wxS( "DNP" ) };
        bomJob.m_fieldsLabels = { wxS( "Refs" ), wxS( "Value" ), wxS( "Footprint" ), wxS( "Qty" ),
                                  wxS( "DNP" ) };
    }

    bomJob.m_excludeDNP = aCtx.Request.exclude_dnp();
    bomJob.m_groupSymbols = aCtx.Request.group_symbols();

    if( !aCtx.Request.variant_name().empty() )
        bomJob.m_variantNames.emplace_back( wxString::FromUTF8( aCtx.Request.variant_name() ) );

    return runSchematicJob( aCtx.Request.job_settings(), std::move( bomJobPtr ) );
}


void API_HANDLER_SCH::packSheetInstance( kiapi::schematic::types::SheetInstance* aInstance, SCH_SHEET_PATH& aPath,
                                          SCH_SHEET* aSheet )
{
    aPath.push_back( aSheet );

    PackSheetPath( *aInstance->mutable_path(), aPath.Path() );

    wxString sheetName = aSheet->GetShownName( false );

    if( sheetName.IsEmpty() && aSheet->GetScreen() )
    {
        wxFileName fn( aSheet->GetScreen()->GetFileName() );
        sheetName = fn.GetName();
    }

    aInstance->set_name( sheetName.ToUTF8() );
    aInstance->set_filename( aSheet->GetFileName().ToUTF8() );
    aInstance->set_page_number( aPath.GetPageNumber().ToUTF8() );

    if( aSheet->GetScreen() )
    {
        std::vector<SCH_ITEM*> childSheets;
        aSheet->GetScreen()->GetSheets( &childSheets );

        std::ranges::sort( childSheets,
                           [&]( SCH_ITEM* a, SCH_ITEM* b )
                           {
                               SCH_SHEET_PATH pathA = aPath;
                               pathA.push_back( static_cast<SCH_SHEET*>( a ) );

                               SCH_SHEET_PATH pathB = aPath;
                               pathB.push_back( static_cast<SCH_SHEET*>( b ) );

                               return pathA.ComparePageNum( pathB ) < 0;
                           } );

        for( SCH_ITEM* childItem : childSheets )
        {
            SCH_SHEET* childSheet = static_cast<SCH_SHEET*>( childItem );
            kiapi::schematic::types::SheetInstance* childInstance = aInstance->add_children();
            packSheetInstance( childInstance, aPath, childSheet );
        }
    }

    aPath.pop_back();
}


HANDLER_RESULT<kiapi::schematic::commands::SchematicHierarchyResponse> API_HANDLER_SCH::handleGetSchematicHierarchy(
        const HANDLER_CONTEXT<kiapi::schematic::commands::GetSchematicHierarchy>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    kiapi::schematic::commands::SchematicHierarchyResponse response;
    response.mutable_document()->CopyFrom( aCtx.Request.document() );

    if( !schematic()->HasHierarchy() )
        schematic()->RefreshHierarchy();

    SCH_SHEET_PATH path;
    std::vector<SCH_SHEET*> topLevelSheets = schematic()->GetTopLevelSheets();

    std::ranges::sort( topLevelSheets,
               [&]( SCH_SHEET* a, SCH_SHEET* b )
               {
                   SCH_SHEET_PATH pathA;
                   pathA.push_back( a );

                   SCH_SHEET_PATH pathB;
                   pathB.push_back( b );

                   return pathA.ComparePageNum( pathB ) < 0;
               } );

    for( SCH_SHEET* topSheet : topLevelSheets )
    {
        kiapi::schematic::types::SheetInstance* instance = response.add_top_level_sheets();
        packSheetInstance( instance, path, topSheet );
    }

    return response;
}


HANDLER_RESULT<kiapi::schematic::commands::SchematicNetlistResponse>
API_HANDLER_SCH::handleGetSchematicNetlist( const HANDLER_CONTEXT<kiapi::schematic::commands::GetSchematicNetlist>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    std::vector<KICAD_T> types = parseRequestedItemTypes( aCtx.Request.types() );
    const bool filterByType = aCtx.Request.types_size() > 0;

    if( filterByType && types.empty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "none of the requested types are valid for a Schematic object" );
        return tl::unexpected( e );
    }

    std::set<KICAD_T> typeFilter( types.begin(), types.end() );

    CONNECTION_GRAPH* connectionGraph = schematic()->ConnectionGraph();

    if( !connectionGraph )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "schematic has no connection graph" );
        return tl::unexpected( e );
    }

    kiapi::schematic::commands::SchematicNetlistResponse response;
    response.mutable_document()->CopyFrom( aCtx.Request.document() );

    for( const auto& [key, subgraphList] : connectionGraph->GetNetMap() )
    {
        if( subgraphList.empty() )
            continue;

        CONNECTION_SUBGRAPH* firstSubgraph = subgraphList[0];

        if( firstSubgraph->GetDriverConnection() && firstSubgraph->GetDriverConnection()->IsBus() )
            continue;

        if( firstSubgraph->GetDriverPriority() < CONNECTION_SUBGRAPH::PRIORITY::PIN )
            continue;

        kiapi::schematic::types::SchematicNet* net = response.add_nets();
        net->set_name( key.Name.ToUTF8() );

        for( CONNECTION_SUBGRAPH* subGraph : subgraphList )
        {
            kiapi::schematic::types::SchematicNetSheetContents* sheetContents = net->add_sheets();
            PackSheetPath( *sheetContents->mutable_path(), subGraph->GetSheet().Path() );

            for( SCH_ITEM* item : subGraph->GetItems() )
            {
                if( filterByType && !typeFilter.contains( item->Type() ) )
                    continue;

                sheetContents->add_items()->set_value( item->m_Uuid.AsStdString() );
            }
        }
    }

    return response;
}


// TODO(JE) factor out
HANDLER_RESULT<CrossProbeAnnounceResponse> API_HANDLER_SCH::handleCrossProbeAnnounce(
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


bool findSymbolsAndPins( const SCH_SHEET_LIST& aSchematicSheetList, const SCH_SHEET_PATH& aSheetPath,
                         std::unordered_map<wxString, std::vector<SCH_REFERENCE>>&             aSyncSymMap,
                         std::unordered_map<wxString, std::unordered_map<wxString, SCH_PIN*>>& aSyncPinMap,
                         const wxString& aVariantName = wxEmptyString, bool aRecursive = false )
{
    if( aRecursive )
    {
        // Iterate over children
        for( const SCH_SHEET_PATH& candidate : aSchematicSheetList )
        {
            if( candidate == aSheetPath || !candidate.IsContainedWithin( aSheetPath ) )
                continue;

            findSymbolsAndPins( aSchematicSheetList, candidate, aSyncSymMap, aSyncPinMap, aVariantName, aRecursive );
        }
    }

    SCH_REFERENCE_LIST references;

    aSheetPath.GetSymbols( references, SYMBOL_FILTER_NON_POWER, true );

    for( unsigned ii = 0; ii < references.GetCount(); ii++ )
    {
        SCH_REFERENCE& schRef = references[ii];

        if( schRef.IsSplitNeeded() )
            schRef.Split();

        SCH_SYMBOL* symbol = schRef.GetSymbol();
        wxString    refNum = schRef.GetRefNumber();
        wxString    fullRef = schRef.GetRef() + refNum;

        // Skip power symbols
        if( fullRef.StartsWith( wxS( "#" ) ) )
            continue;

        // Unannotated symbols are not supported
        if( refNum.compare( wxS( "?" ) ) == 0 )
            continue;

        // Look for whole footprint
        auto symMatchIt = aSyncSymMap.find( fullRef );

        if( symMatchIt != aSyncSymMap.end() )
        {
            symMatchIt->second.emplace_back( schRef );

            // Whole footprint was selected, no need to select pins
            continue;
        }

        // Look for pins
        auto symPinMatchIt = aSyncPinMap.find( fullRef );

        if( symPinMatchIt != aSyncPinMap.end() )
        {
            std::unordered_map<wxString, SCH_PIN*>& pinMap = symPinMatchIt->second;
            std::vector<SCH_PIN*>                   pinsOnSheet = symbol->GetPins( &aSheetPath );

            for( SCH_PIN* pin : pinsOnSheet )
            {
                int pinUnit = pin->GetLibPin()->GetUnit();

                if( pinUnit > 0 && pinUnit != schRef.GetUnit() )
                    continue;

                // Reverse-map the requested pad back to the owning pin (issue #2282).  A pin may
                // resolve to several pads via the map; match the first that pcbnew asked for.
                for( const wxString& pad :
                     ExpandStackedPinNotation( pin->GetEffectivePadNumber( aSheetPath, aVariantName ) ) )
                {
                    auto pinIt = pinMap.find( pad );

                    if( pinIt != pinMap.end() )
                    {
                        pinIt->second = pin;
                        break;
                    }
                }
            }
        }
    }

    return false;
}


bool sheetContainsOnlyWantedItems(
        const SCH_SHEET_LIST& aSchematicSheetList, const SCH_SHEET_PATH& aSheetPath,
        std::unordered_map<wxString, std::vector<SCH_REFERENCE>>&             aSyncSymMap,
        std::unordered_map<wxString, std::unordered_map<wxString, SCH_PIN*>>& aSyncPinMap,
        std::unordered_map<SCH_SHEET_PATH, bool>&                             aCache )
{
    auto cacheIt = aCache.find( aSheetPath );

    if( cacheIt != aCache.end() )
        return cacheIt->second;

    // Iterate over children
    for( const SCH_SHEET_PATH& candidate : aSchematicSheetList )
    {
        if( candidate == aSheetPath || !candidate.IsContainedWithin( aSheetPath ) )
            continue;

        bool childRet = sheetContainsOnlyWantedItems( aSchematicSheetList, candidate, aSyncSymMap,
                                                      aSyncPinMap, aCache );

        if( !childRet )
        {
            aCache.emplace( aSheetPath, false );
            return false;
        }
    }

    SCH_REFERENCE_LIST references;
    aSheetPath.GetSymbols( references, SYMBOL_FILTER_NON_POWER, true );

    if( references.GetCount() == 0 )    // Empty sheet, obviously do not contain wanted items
    {
        aCache.emplace( aSheetPath, false );
        return false;
    }

    for( unsigned ii = 0; ii < references.GetCount(); ii++ )
    {
        SCH_REFERENCE& schRef = references[ii];

        if( schRef.IsSplitNeeded() )
            schRef.Split();

        wxString refNum = schRef.GetRefNumber();
        wxString fullRef = schRef.GetRef() + refNum;

        // Skip power symbols
        if( fullRef.StartsWith( wxS( "#" ) ) )
            continue;

        // Unannotated symbols are not supported
        if( refNum.compare( wxS( "?" ) ) == 0 )
            continue;

        if( aSyncSymMap.find( fullRef ) == aSyncSymMap.end() )
        {
            aCache.emplace( aSheetPath, false );
            return false; // Some symbol is not wanted.
        }

        if( aSyncPinMap.find( fullRef ) != aSyncPinMap.end() )
        {
            aCache.emplace( aSheetPath, false );
            return false; // Looking for specific pins, so can't be mapped
        }
    }

    aCache.emplace( aSheetPath, true );
    return true;
}


std::optional<std::tuple<SCH_SHEET_PATH, SCH_ITEM*, std::vector<SCH_ITEM*>>>
findItemsFromSyncSelection( const SCHEMATIC& aSchematic,
                            const kiapi::common::commands::SyncSelection& aSync )
{
    std::unordered_map<wxString, std::vector<SCH_REFERENCE>>             syncSymMap;
    std::unordered_map<wxString, std::unordered_map<wxString, SCH_PIN*>> syncPinMap;
    std::unordered_map<SCH_SHEET_PATH, bool>                             fullyWantedCache;

    std::optional<wxString>                                    focusSymbol;
    std::optional<std::pair<wxString, wxString>>               focusPin;
    std::unordered_map<SCH_SHEET_PATH, std::vector<SCH_ITEM*>> focusItemResults;

    const SCH_SHEET_LIST allSheetsList = aSchematic.Hierarchy();

    // In orderedSheets, the current sheet comes first.
    std::vector<SCH_SHEET_PATH> orderedSheets;
    orderedSheets.reserve( allSheetsList.size() );
    orderedSheets.push_back( aSchematic.CurrentSheet() );

    for( const SCH_SHEET_PATH& sheetPath : allSheetsList )
    {
        if( sheetPath != aSchematic.CurrentSheet() )
            orderedSheets.push_back( sheetPath );
    }

    const bool focusOnFirst = ( aSync.mode() == kiapi::common::commands::SSM_ITEMS_AND_NETS ) && aSync.has_focus_item();

    for( const kiapi::common::commands::SelectionSpec& spec : aSync.items() )
    {
        switch( spec.spec_case() )
        {
        case kiapi::common::commands::SelectionSpec::kFootprint:
        {
            wxString symRef = wxString::FromUTF8( spec.footprint().reference() );
            syncSymMap[symRef] = std::vector<SCH_REFERENCE>();
            break;
        }

        case kiapi::common::commands::SelectionSpec::kPad:
        {
            wxString symRef = wxString::FromUTF8( spec.pad().reference() );
            wxString padNum = wxString::FromUTF8( spec.pad().number() );
            syncPinMap[symRef][padNum] = nullptr;
            break;
        }

        default:
            break;
        }
    }

    if( focusOnFirst )
    {
        const kiapi::common::commands::SelectionSpec& focusSpec = aSync.focus_item();

        if( focusSpec.has_footprint() )
            focusSymbol = wxString::FromUTF8( focusSpec.footprint().reference() );
        else if( focusSpec.has_pad() )
            focusPin = std::make_pair( wxString::FromUTF8( focusSpec.pad().reference() ),
                                       wxString::FromUTF8( focusSpec.pad().number() ) );
    }

    // Lambda definitions
    auto flattenSyncMaps =
            [&syncSymMap, &syncPinMap]() -> std::vector<SCH_ITEM*>
            {
                std::vector<SCH_ITEM*> allVec;

                for( const auto& [symRef, symbols] : syncSymMap )
                {
                    for( const SCH_REFERENCE& ref : symbols )
                        allVec.push_back( ref.GetSymbol() );
                }

                for( const auto& [symRef, pinMap] : syncPinMap )
                {
                    for( const auto& [padNum, pin] : pinMap )
                    {
                        if( pin )
                            allVec.push_back( pin );
                    }
                }

                return allVec;
            };

    auto clearSyncMaps =
            [&syncSymMap, &syncPinMap]()
            {
                for( auto& [symRef, symbols] : syncSymMap )
                    symbols.clear();

                for( auto& [reference, pins] : syncPinMap )
                {
                    for( auto& [number, pin] : pins )
                        pin = nullptr;
                }
            };

    auto syncMapsValuesEmpty =
            [&syncSymMap, &syncPinMap]() -> bool
            {
                for( const auto& [symRef, symbols] : syncSymMap )
                {
                    if( symbols.size() > 0 )
                        return false;
                }

                for( const auto& [symRef, pins] : syncPinMap )
                {
                    for( const auto& [padNum, pin] : pins )
                    {
                        if( pin )
                            return false;
                    }
                }

                return true;
            };

    auto checkFocusItems =
            [&]( const SCH_SHEET_PATH& aSheet )
            {
                if( focusSymbol )
                {
                    auto findIt = syncSymMap.find( *focusSymbol );

                    if( findIt != syncSymMap.end() )
                    {
                        if( findIt->second.size() > 0 )
                            focusItemResults[aSheet].push_back( findIt->second.front().GetSymbol() );
                    }
                }
                else if( focusPin )
                {
                    auto findIt = syncPinMap.find( focusPin->first );

                    if( findIt != syncPinMap.end() )
                    {
                        if( findIt->second[focusPin->second] )
                            focusItemResults[aSheet].push_back( findIt->second[focusPin->second] );
                    }
                }
            };

    auto makeRetForSheet =
            [&]( const SCH_SHEET_PATH& aSheet, SCH_ITEM* aFocusItem )
            {
                clearSyncMaps();

                // Fill sync maps
                findSymbolsAndPins( allSheetsList, aSheet, syncSymMap, syncPinMap, aSchematic.GetCurrentVariant() );
                std::vector<SCH_ITEM*> itemsVector = flattenSyncMaps();

                // Add fully wanted sheets to vector
                for( SCH_ITEM* item : aSheet.LastScreen()->Items().OfType( SCH_SHEET_T ) )
                {
                    KIID_PATH kiidPath = aSheet.Path();
                    kiidPath.push_back( item->m_Uuid );

                    std::optional<SCH_SHEET_PATH> subsheetPath =
                            allSheetsList.GetSheetPathByKIIDPath( kiidPath );

                    if( !subsheetPath )
                        continue;

                    if( sheetContainsOnlyWantedItems( allSheetsList, *subsheetPath, syncSymMap,
                                                      syncPinMap, fullyWantedCache ) )
                    {
                        itemsVector.push_back( item );
                    }
                }

                return std::make_tuple( aSheet, aFocusItem, itemsVector );
            };

    if( focusOnFirst )
    {
        for( const SCH_SHEET_PATH& sheetPath : orderedSheets )
        {
            clearSyncMaps();

            findSymbolsAndPins( allSheetsList, sheetPath, syncSymMap, syncPinMap, aSchematic.GetCurrentVariant() );

            checkFocusItems( sheetPath );
        }

        if( focusItemResults.size() > 0 )
        {
            for( const SCH_SHEET_PATH& sheetPath : orderedSheets )
            {
                const std::vector<SCH_ITEM*>& items = focusItemResults[sheetPath];

                if( !items.empty() )
                    return makeRetForSheet( sheetPath, items.front() );
            }
        }
    }
    else
    {
        for( const SCH_SHEET_PATH& sheetPath : orderedSheets )
        {
            clearSyncMaps();

            findSymbolsAndPins( allSheetsList, sheetPath, syncSymMap, syncPinMap, aSchematic.GetCurrentVariant() );

            if( !syncMapsValuesEmpty() )
            {
                // Something found on sheet
                return makeRetForSheet( sheetPath, nullptr );
            }
        }
    }

    return std::nullopt;
}


HANDLER_RESULT<SyncSelectionResponse> API_HANDLER_SCH::handleSyncSelection(
        const HANDLER_CONTEXT<SyncSelection>& aCtx )
{
    if( std::optional<ApiResponseStatus> headless = checkForHeadless( "SyncSelection" ) )
        return tl::unexpected( *headless );

    SyncSelectionResponse response;

    const CROSS_PROBING_SETTINGS& settings = frame()->eeconfig()->m_CrossProbing;

    if( !settings.on_selection && aCtx.Request.context() != SyncSelectionContext::SSC_EXPLICIT )
    {
        response.set_status( CPS_DISABLED );
        response.set_message( "implicit selection sync disabled by user" );
        return response;
    }

    // A request carrying no items asks for nothing to be selected, so there is nothing to find.
    if( aCtx.Request.items_size() == 0 )
    {
        frame()->SetSyncingSelection( true ); // recursion guard

        frame()->GetToolManager()->GetTool<SCH_SELECTION_TOOL>()->SyncSelection( std::nullopt, nullptr, {} );

        frame()->SetSyncingSelection( false );

        response.set_status( CPS_OK );
        return response;
    }

    std::optional<std::tuple<SCH_SHEET_PATH, SCH_ITEM*, std::vector<SCH_ITEM*>>> findRet =
                    findItemsFromSyncSelection( *schematic(), aCtx.Request );

    if( findRet )
    {
        auto& [sheetPath, focusItem, items] = *findRet;

        frame()->SetSyncingSelection( true ); // recursion guard

        frame()->GetToolManager()->GetTool<SCH_SELECTION_TOOL>()->SyncSelection( sheetPath, focusItem, items );

        frame()->SetSyncingSelection( false );

        if( frame()->eeconfig()->m_CrossProbing.flash_selection )
        {
            wxLogTrace( traceCrossProbeFlash, "MAIL_SELECTION(_FORCE): flash enabled, items=%zu",
                        items.size() );

            if( items.empty() )
            {
                wxLogTrace( traceCrossProbeFlash, "MAIL_SELECTION(_FORCE): nothing to flash" );
            }
            else
            {
                std::vector<SCH_ITEM*> itemPtrs;
                std::copy( items.begin(), items.end(), std::back_inserter( itemPtrs ) );

                frame()->StartCrossProbeFlash( itemPtrs );
            }
        }
        else
        {
            wxLogTrace( traceCrossProbeFlash, "MAIL_SELECTION(_FORCE): flash disabled" );
        }
    }

    response.set_status( CPS_OK );
    return response;
}


HANDLER_RESULT<HighlightNetsResponse> API_HANDLER_SCH::handleHighlightNets(
        const HANDLER_CONTEXT<HighlightNets>& aCtx )
{
    if( std::optional<ApiResponseStatus> headless = checkForHeadless( "HighlightNets" ) )
        return tl::unexpected( *headless );

    HighlightNetsResponse response;
    CROSS_PROBING_SETTINGS& crossProbingSettings = frame()->eeconfig()->m_CrossProbing;

    if( aCtx.ClientName == StandaloneCrossProbeClientName
        || aCtx.ClientName == KiwayClientName )
    {
        if( !crossProbingSettings.auto_highlight )
        {
            response.set_status( CPS_DISABLED );
            return response;
        }
    }

    wxString net;

    if( aCtx.Request.net_name_size() > 0 )
        net = wxString::FromUTF8( aCtx.Request.net_name( 0 ) );

    frame()->HandleRemoteNetHighlight( net );

    response.set_status( CPS_OK );
    return response;
}



HANDLER_RESULT<ExpandTextVariablesResponse>
API_HANDLER_SCH::handleExpandTextVariables( const HANDLER_CONTEXT<ExpandTextVariables>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    SCH_SHEET_PATH path = m_context->GetCurrentSheet().value_or( *schematic()->Hierarchy().begin() );

    if( aCtx.Request.document().has_sheet_path() )
    {
        KIID_PATH kiidPath = UnpackSheetPath( aCtx.Request.document().sheet_path() );

        if( std::optional<SCH_SHEET_PATH> resolvedPath = schematic()->Hierarchy().GetSheetPathByKIIDPath( kiidPath ) )
        {
            path = *resolvedPath;
        }
    }

    ExpandTextVariablesResponse reply;

    std::function<bool( wxString* )> textResolver =
        [&]( wxString* token ) -> bool
        {
            return schematic()->ResolveTextVar( &path, token, 0 );
        };

    PROJECT& project = m_context->Prj();

    for( const std::string& textMsg : aCtx.Request.text() )
    {
        wxString text = ExpandTextVars( wxString::FromUTF8( textMsg ), &textResolver );

        if( aCtx.Request.expand_env_vars() )
            text = ExpandEnvVarSubstitutions( text, &project );

        reply.add_text( text.ToUTF8() );
    }

    return reply;
}


HANDLER_RESULT<VariantsResponse> API_HANDLER_SCH::handleGetVariants( const HANDLER_CONTEXT<GetVariants>& aCtx )
{
    if( aCtx.Request.document().type() != DocumentType::DOCTYPE_SCHEMATIC )
        return tl::unexpected( MakeResponseStatus( AS_UNHANDLED ) );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    VariantsResponse response;

    response.mutable_document()->CopyFrom( aCtx.Request.document() );

    for( const wxString& name : schematic()->GetVariantNames() )
    {
        types::DesignVariant* var = response.add_variants();
        var->set_name( name.ToUTF8() );
        var->set_description( schematic()->GetVariantDescription( name ).ToUTF8() );
    }

    return response;
}


HANDLER_RESULT<Empty> API_HANDLER_SCH::handleAddVariant( const HANDLER_CONTEXT<AddVariant>& aCtx )
{
    if( aCtx.Request.document().type() != DocumentType::DOCTYPE_SCHEMATIC )
        return tl::unexpected( MakeResponseStatus( AS_UNHANDLED ) );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    SCHEMATIC* schematic = this->schematic();
    wxString   name = wxString::FromUTF8( aCtx.Request.name() );

    if( name.IsEmpty() || name.CmpNoCase( GetDefaultVariantName() ) == 0 || schematic->HasVariant( name ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "'{}' is not a usable new variant name", aCtx.Request.name() ) );
        return tl::unexpected( e );
    }

    schematic->AddVariant( name );

    if( aCtx.Request.has_description() )
        schematic->SetVariantDescription( name, wxString::FromUTF8( aCtx.Request.description() ) );

    if( m_frame )
        frame()->UpdateVariantSelectionCtrl( frame()->Schematic().GetVariantNamesForUI() );

    bumpRevision();
    publishProjectChanged( kiapi::common::events::PCK_VARIANTS, aCtx.ClientName );

    return Empty();
}


HANDLER_RESULT<Empty> API_HANDLER_SCH::handleDeleteVariant( const HANDLER_CONTEXT<DeleteVariant>& aCtx )
{
    if( aCtx.Request.document().type() != DocumentType::DOCTYPE_SCHEMATIC )
        return tl::unexpected( MakeResponseStatus( AS_UNHANDLED ) );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    SCH_COMMIT commit( m_frame ? frame()->GetToolManager() : toolManager() );

    SCHEMATIC* schematic = this->schematic();
    wxString   name = wxString::FromUTF8( aCtx.Request.name() );

    if( name.IsEmpty() || name.CmpNoCase( GetDefaultVariantName() ) == 0 )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "'{}' is not a valid variant name", aCtx.Request.name() ) );
        return tl::unexpected( e );
    }

    if( !schematic->HasVariant( name ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "no variant named '{}' exists", aCtx.Request.name() ) );
        return tl::unexpected( e );
    }

    schematic->DeleteVariant( name, &commit );

    if( m_frame )
    {
        if( frame()->Schematic().GetCurrentVariant().CmpNoCase( name ) == 0 )
            frame()->SetCurrentVariant( wxEmptyString );

        frame()->UpdateVariantSelectionCtrl( frame()->Schematic().GetVariantNamesForUI() );
        frame()->GetCanvas()->Refresh();
    }

    bumpRevision();
    publishProjectChanged( kiapi::common::events::PCK_VARIANTS, aCtx.ClientName );

    return Empty();
}


HANDLER_RESULT<Empty> API_HANDLER_SCH::handleRenameVariant( const HANDLER_CONTEXT<RenameVariant>& aCtx )
{
    if( aCtx.Request.document().type() != DocumentType::DOCTYPE_SCHEMATIC )
        return tl::unexpected( MakeResponseStatus( AS_UNHANDLED ) );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    SCH_COMMIT commit( m_frame ? frame()->GetToolManager() : toolManager() );

    SCHEMATIC* schematic = this->schematic();
    wxString   oldName = wxString::FromUTF8( aCtx.Request.old_name() );
    wxString   newName = wxString::FromUTF8( aCtx.Request.new_name() );

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

    if( !schematic->HasVariant( oldName ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "no variant named '{}' exists", aCtx.Request.old_name() ) );
        return tl::unexpected( e );
    }

    if( schematic->HasVariant( newName ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "a variant named '{}' already exists", aCtx.Request.new_name() ) );
        return tl::unexpected( e );
    }

    schematic->RenameVariant( oldName, newName, &commit );

    if( m_frame )
        frame()->UpdateVariantSelectionCtrl( frame()->Schematic().GetVariantNamesForUI() );

    bumpRevision();
    publishProjectChanged( kiapi::common::events::PCK_VARIANTS, aCtx.ClientName );

    return Empty();
}


HANDLER_RESULT<Empty> API_HANDLER_SCH::handleCopyVariant( const HANDLER_CONTEXT<CopyVariant>& aCtx )
{
    if( aCtx.Request.document().type() != DocumentType::DOCTYPE_SCHEMATIC )
        return tl::unexpected( MakeResponseStatus( AS_UNHANDLED ) );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    SCH_COMMIT commit( m_frame ? frame()->GetToolManager() : toolManager() );

    SCHEMATIC* schematic = this->schematic();
    wxString   oldName = wxString::FromUTF8( aCtx.Request.old_name() );
    wxString   newName = wxString::FromUTF8( aCtx.Request.new_name() );

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

    if( !schematic->HasVariant( oldName ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "no variant named '{}' exists", aCtx.Request.old_name() ) );
        return tl::unexpected( e );
    }

    if( schematic->HasVariant( newName ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "a variant named '{}' already exists", aCtx.Request.new_name() ) );
        return tl::unexpected( e );
    }

    schematic->CopyVariant( oldName, newName, &commit );

    if( m_frame )
        frame()->UpdateVariantSelectionCtrl( frame()->Schematic().GetVariantNamesForUI() );

    bumpRevision();
    publishProjectChanged( kiapi::common::events::PCK_VARIANTS, aCtx.ClientName );

    return Empty();
}


HANDLER_RESULT<Empty> API_HANDLER_SCH::handleSetVariantDescription( const HANDLER_CONTEXT<SetVariantDescription>& aCtx )
{
    if( aCtx.Request.document().type() != DocumentType::DOCTYPE_SCHEMATIC )
        return tl::unexpected( MakeResponseStatus( AS_UNHANDLED ) );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    SCHEMATIC* schematic = this->schematic();
    wxString   name = wxString::FromUTF8( aCtx.Request.name() );

    if( name.IsEmpty() || name.CmpNoCase( GetDefaultVariantName() ) == 0 || !schematic->HasVariant( name ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "no variant named '{}' exists", aCtx.Request.name() ) );
        return tl::unexpected( e );
    }

    schematic->SetVariantDescription( name, wxString::FromUTF8( aCtx.Request.description() ) );

    bumpRevision();
    publishProjectChanged( kiapi::common::events::PCK_VARIANTS, aCtx.ClientName );

    return Empty();
}


HANDLER_RESULT<Empty> API_HANDLER_SCH::handleSetCurrentVariant( const HANDLER_CONTEXT<SetCurrentVariant>& aCtx )
{
    if( aCtx.Request.document().type() != DocumentType::DOCTYPE_SCHEMATIC )
        return tl::unexpected( MakeResponseStatus( AS_UNHANDLED ) );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    SCHEMATIC* schematic = this->schematic();

    if( aCtx.Request.has_name() && !aCtx.Request.name().empty() )
    {
        if( wxString name = wxString::FromUTF8( aCtx.Request.name() ); !schematic->HasVariant( name ) )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( fmt::format( "no variant named '{}' exists", aCtx.Request.name() ) );
            return tl::unexpected( e );
        }
    }

    wxString name = aCtx.Request.has_name() ? wxString::FromUTF8( aCtx.Request.name() ) : wxString();

    if( m_frame )
        frame()->SetCurrentVariant( name );
    else
        schematic->SetCurrentVariant( name );

    bumpRevision();
    publishProjectChanged( kiapi::common::events::PCK_VARIANTS, aCtx.ClientName );

    return Empty();
}


HANDLER_RESULT<CurrentVariantResponse>
API_HANDLER_SCH::handleGetCurrentVariant( const HANDLER_CONTEXT<GetCurrentVariant>& aCtx )
{
    if( aCtx.Request.document().type() != DocumentType::DOCTYPE_SCHEMATIC )
        return tl::unexpected( MakeResponseStatus( AS_UNHANDLED ) );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    CurrentVariantResponse response;

    if( wxString current = schematic()->GetCurrentVariant(); !current.IsEmpty() )
        response.set_name( current.ToUTF8() );

    return response;
}


//// Annotation (Since 11.0) ////

HANDLER_RESULT<bool> API_HANDLER_SCH::resolveAnnotateScope( const DocumentSpecifier& aDocument,
                                                            kiapi::schematic::commands::AnnotateScope aScope,
                                                            const google::protobuf::RepeatedPtrField<types::KIID>& aItems,
                                                            bool aRecursive, SCH_SHEET_PATH& aCurrentSheet,
                                                            SCH_SHEET_LIST& aSubSheets,
                                                            SCH_SHEET_LIST& aSelectedSheets,
                                                            std::unordered_set<SCH_SYMBOL*>& aSelectedSymbols )
{
    ApiResponseStatus e;
    e.set_status( ApiStatusCode::AS_BAD_REQUEST );

    std::optional<SCH_SHEET_PATH> current = resolveSheet( aDocument );

    if( !current || !current->LastScreen() )
    {
        e.set_error_message( "the schematic has no sheets" );
        return tl::unexpected( e );
    }

    aCurrentSheet = *current;

    SCH_SHEET_LIST sheets = schematic()->Hierarchy();

    auto collectSubSheets =
            [&]( SCH_SHEET* aSheet, SCH_SHEET_LIST& aOut )
            {
                SCH_SHEET_PATH subSheetPath = aCurrentSheet;
                subSheetPath.push_back( aSheet );
                sheets.GetSheetsWithinPath( aOut, subSheetPath );
            };

    switch( aScope )
    {
    case kiapi::schematic::commands::ANS_ALL:
        break;

    case kiapi::schematic::commands::ANS_SHEET:
    {
        if( !aRecursive )
            break;

        std::vector<SCH_ITEM*> subSheets;
        aCurrentSheet.LastScreen()->GetSheets( &subSheets );

        for( SCH_ITEM* item : subSheets )
            collectSubSheets( static_cast<SCH_SHEET*>( item ), aSubSheets );

        break;
    }

    case kiapi::schematic::commands::ANS_SELECTION:
    {
        if( aItems.empty() )
        {
            e.set_error_message( "ANS_SELECTION needs at least one symbol or sheet in items" );
            return tl::unexpected( e );
        }

        for( const types::KIID& id : aItems )
        {
            SCH_ITEM* item = aCurrentSheet.ResolveItem( KIID( id.value() ) );

            if( !item )
            {
                e.set_error_message( fmt::format( "item {} is not on the sheet {}", id.value(),
                                                  aCurrentSheet.PathHumanReadable().ToStdString() ) );
                return tl::unexpected( e );
            }

            if( item->Type() == SCH_SYMBOL_T )
            {
                aSelectedSymbols.insert( static_cast<SCH_SYMBOL*>( item ) );
            }
            else if( item->Type() == SCH_SHEET_T )
            {
                if( aRecursive )
                    collectSubSheets( static_cast<SCH_SHEET*>( item ), aSelectedSheets );
            }
            else
            {
                e.set_error_message( fmt::format( "item {} is a {}, not a symbol or sheet", id.value(),
                                                  item->GetClass().ToStdString() ) );
                return tl::unexpected( e );
            }
        }

        break;
    }

    default:
        e.set_error_message( "scope must be ANS_ALL, ANS_SHEET or ANS_SELECTION" );
        return tl::unexpected( e );
    }

    return true;
}


HANDLER_RESULT<AnnotateResponse> API_HANDLER_SCH::handleAnnotate( const HANDLER_CONTEXT<Annotate>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.schematic() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    const AnnotateOptions&          options = aCtx.Request.options();
    const AnnotateScope             scope = aCtx.Request.scope();
    SCH_SHEET_PATH                  currentSheet;
    SCH_SHEET_LIST                  subSheets;
    SCH_SHEET_LIST                  selectedSheets;
    std::unordered_set<SCH_SYMBOL*> selectedSymbols;

    HANDLER_RESULT<bool> resolved = resolveAnnotateScope( aCtx.Request.schematic(), scope, aCtx.Request.items(),
                                                          options.recursive(), currentSheet, subSheets,
                                                          selectedSheets, selectedSymbols );

    if( !resolved )
        return tl::unexpected( resolved.error() );

    SCHEMATIC*          sch = schematic();
    SCHEMATIC_SETTINGS& settings = sch->Settings();
    SCH_SHEET_LIST      sheets = sch->Hierarchy();

    // Unset options take the project's annotation settings, as the dialog's defaults do
    ANNOTATE_ORDER_T sortOrder = static_cast<ANNOTATE_ORDER_T>( settings.m_AnnotateSortOrder );

    switch( options.sort_order() )
    {
    case ASO_X_POSITION: sortOrder = SORT_BY_X_POSITION; break;
    case ASO_Y_POSITION: sortOrder = SORT_BY_Y_POSITION; break;
    case ASO_UNSORTED:   sortOrder = UNSORTED;           break;
    default:                                              break;
    }

    ANNOTATE_ALGO_T algo = static_cast<ANNOTATE_ALGO_T>( settings.m_AnnotateMethod );

    switch( options.numbering() )
    {
    case ANM_INCREMENTAL:        algo = INCREMENTAL_BY_REF;  break;
    case ANM_SHEET_NUMBER_X100:  algo = SHEET_NUMBER_X_100;  break;
    case ANM_SHEET_NUMBER_X1000: algo = SHEET_NUMBER_X_1000; break;
    default:                                                 break;
    }

    int startNumber = options.start_number() > 0 ? static_cast<int>( options.start_number() )
                                                 : settings.m_AnnotateStartNum;

    // Symbols and sheets in scope, as SCH_EDIT_FRAME::AnnotateSymbols collects them
    auto collectReferences =
            [&]( SCH_REFERENCE_LIST& aReferences, SYMBOL_FILTER aCurrentSheetFilter )
            {
                switch( scope )
                {
                case ANS_ALL:
                    sheets.GetSymbols( aReferences, SYMBOL_FILTER_ALL );
                    break;

                case ANS_SHEET:
                    currentSheet.GetSymbols( aReferences, aCurrentSheetFilter );

                    if( options.recursive() )
                        subSheets.GetSymbolsWithinPath( aReferences, currentSheet, SYMBOL_FILTER_NON_POWER, true );

                    break;

                case ANS_SELECTION:
                    for( SCH_SYMBOL* symbol : selectedSymbols )
                        currentSheet.AppendSymbol( aReferences, symbol, SYMBOL_FILTER_NON_POWER, true );

                    if( options.recursive() )
                        selectedSheets.GetSymbolsWithinPath( aReferences, currentSheet, SYMBOL_FILTER_NON_POWER, true );

                    break;

                default:
                    break;
                }
            };

    // Multi-unit symbols keep their current groupings unless the caller asks to regroup them
    SCH_MULTI_UNIT_REFERENCE_MAP lockedSymbols;

    if( !options.regroup_units() )
    {
        switch( scope )
        {
        case ANS_ALL:
            sheets.GetMultiUnitSymbols( lockedSymbols, SYMBOL_FILTER_ALL );
            break;

        case ANS_SHEET:
            currentSheet.GetMultiUnitSymbols( lockedSymbols, SYMBOL_FILTER_ALL );

            if( options.recursive() )
                subSheets.GetMultiUnitSymbols( lockedSymbols, SYMBOL_FILTER_ALL );

            break;

        case ANS_SELECTION:
            for( SCH_SYMBOL* symbol : selectedSymbols )
                currentSheet.AppendMultiUnitSymbol( lockedSymbols, symbol, SYMBOL_FILTER_NON_POWER );

            if( options.recursive() )
                selectedSheets.GetMultiUnitSymbols( lockedSymbols, SYMBOL_FILTER_NON_POWER );

            break;

        default:
            break;
        }

        // A reset drops groups that already hold the same unit twice so they get fresh numbers
        if( options.reset_existing() )
        {
            std::erase_if( lockedSymbols,
                           []( const auto& aEntry )
                           {
                               std::set<int> seenUnits;

                               for( const SCH_REFERENCE& ref : aEntry.second )
                               {
                                   if( !seenUnits.insert( ref.GetUnit() ).second )
                                       return true;
                               }

                               return false;
                           } );
        }
    }

    // The previous references, to report what changed
    std::map<wxString, wxString> previousAnnotation;

    {
        SCH_REFERENCE_LIST all;
        sheets.GetSymbols( all, SYMBOL_FILTER_ALL );

        for( size_t i = 0; i < all.GetCount(); i++ )
        {
            SCH_SYMBOL*     symbol = all[i].GetSymbol();
            SCH_SHEET_PATH* sheetPath = &all[i].GetSheetPath();
            KIID_PATH       fullUuid = sheetPath->Path();

            fullUuid.push_back( symbol->m_Uuid );

            if( symbol->IsAnnotated( sheetPath ) )
                previousAnnotation[fullUuid.AsString()] = symbol->GetRef( sheetPath, true );
        }
    }

    sch->SetSheetNumberAndCount();

    SCH_REFERENCE_LIST references;
    collectReferences( references, SYMBOL_FILTER_ALL );

    if( options.reset_existing() )
        references.RemoveAnnotation();

    // References outside the scope must not be reused
    SCH_REFERENCE_LIST additionalRefs;

    if( scope != ANS_ALL )
    {
        SCH_REFERENCE_LIST allRefs;
        sheets.GetSymbols( allRefs, SYMBOL_FILTER_ALL );

        for( size_t i = 0; i < allRefs.GetCount(); i++ )
        {
            if( !references.Contains( allRefs[i] ) )
                additionalRefs.AddItem( allRefs[i] );
        }
    }

    references.SetRefDesTracker( settings.m_refDesTracker );
    references.SplitReferences();
    references.AnnotateByOptions( sortOrder, algo, startNumber, lockedSymbols, additionalRefs, false );

    SCH_COMMIT*      commit = static_cast<SCH_COMMIT*>( getCurrentCommit( aCtx.ClientName ) );
    AnnotateResponse response;

    for( size_t i = 0; i < references.GetCount(); i++ )
    {
        SCH_REFERENCE&  ref = references[i];
        SCH_SYMBOL*     symbol = ref.GetSymbol();
        SCH_SHEET_PATH* sheetPath = &ref.GetSheetPath();

        commit->Modify( symbol, sheetPath->LastScreen() );
        ref.Annotate();

        KIID_PATH fullUuid = sheetPath->Path();
        fullUuid.push_back( symbol->m_Uuid );

        wxString prevRef = previousAnnotation[fullUuid.AsString()];
        wxString newRef = symbol->GetRef( sheetPath, true );

        if( newRef == prevRef )
            continue;

        response.set_annotated_count( response.annotated_count() + 1 );

        wxString msg;

        if( prevRef.Length() )
        {
            msg.Printf( _( "Updated %s from %s to %s." ), symbol->GetValue( true, sheetPath, false ), prevRef,
                        newRef );
        }
        else
        {
            msg.Printf( _( "Annotated %s as %s." ), symbol->GetValue( true, sheetPath, false ), newRef );
        }

        response.add_messages( msg.ToUTF8() );
    }

    response.set_symbol_count( static_cast<uint32_t>( references.GetCount() ) );

    // Final check, on a fresh list as SCH_EDIT_FRAME::CheckAnnotate does
    SCH_REFERENCE_LIST checkList;
    collectReferences( checkList, SYMBOL_FILTER_NON_POWER );

    int errors = checkList.CheckAnnotation(
            [&]( ERCE_T, const wxString& aMsg, SCH_REFERENCE*, SCH_REFERENCE* )
            {
                response.add_messages( aMsg.ToUTF8() );
            } );

    response.set_error_count( static_cast<uint32_t>( errors ) );

    currentSheet.UpdateAllScreenReferences();
    sch->SetSheetNumberAndCount();

    if( !m_activeClients.contains( aCtx.ClientName ) )
        pushCurrentCommit( aCtx.ClientName, _( "Annotate" ) );

    if( m_frame )
    {
        frame()->SyncView();
        frame()->GetCanvas()->Refresh();
        frame()->UpdateNetHighlightStatus();
    }

    return response;
}


HANDLER_RESULT<AnnotateResponse>
API_HANDLER_SCH::handleClearAnnotation( const HANDLER_CONTEXT<ClearAnnotation>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.schematic() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    SCH_SHEET_PATH                  currentSheet;
    SCH_SHEET_LIST                  subSheets;
    SCH_SHEET_LIST                  selectedSheets;
    std::unordered_set<SCH_SYMBOL*> selectedSymbols;

    HANDLER_RESULT<bool> resolved = resolveAnnotateScope( aCtx.Request.schematic(), aCtx.Request.scope(),
                                                          aCtx.Request.items(), aCtx.Request.recursive(),
                                                          currentSheet, subSheets, selectedSheets,
                                                          selectedSymbols );

    if( !resolved )
        return tl::unexpected( resolved.error() );

    SCH_COMMIT*      commit = static_cast<SCH_COMMIT*>( getCurrentCommit( aCtx.ClientName ) );
    AnnotateResponse response;

    auto clearSymbol =
            [&]( SCH_SYMBOL* aSymbol, SCH_SCREEN* aScreen, SCH_SHEET_PATH* aSheet )
            {
                if( !aSymbol->IsAnnotated( aSheet ) )
                    return;

                commit->Modify( aSymbol, aScreen );

                wxString msg;
                msg.Printf( _( "Cleared annotation for %s." ), aSymbol->GetValue( true, aSheet, false ) );

                aSymbol->ClearAnnotation( aSheet, false );
                response.set_annotated_count( response.annotated_count() + 1 );
                response.add_messages( msg.ToUTF8() );
            };

    auto clearSheet =
            [&]( SCH_SHEET_PATH& aSheet )
            {
                for( SCH_ITEM* item : aSheet.LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
                {
                    response.set_symbol_count( response.symbol_count() + 1 );
                    clearSymbol( static_cast<SCH_SYMBOL*>( item ), aSheet.LastScreen(), &aSheet );
                }
            };

    switch( aCtx.Request.scope() )
    {
    case ANS_ALL:
        for( SCH_SHEET_PATH& sheet : schematic()->Hierarchy() )
            clearSheet( sheet );

        break;

    case ANS_SHEET:
        clearSheet( currentSheet );

        for( SCH_SHEET_PATH& sheet : subSheets )
            clearSheet( sheet );

        break;

    case ANS_SELECTION:
        for( SCH_SYMBOL* symbol : selectedSymbols )
        {
            response.set_symbol_count( response.symbol_count() + 1 );
            clearSymbol( symbol, currentSheet.LastScreen(), &currentSheet );
        }

        for( SCH_SHEET_PATH& sheet : selectedSheets )
            clearSheet( sheet );

        break;

    default:
        break;
    }

    currentSheet.UpdateAllScreenReferences();

    if( !m_activeClients.contains( aCtx.ClientName ) )
        pushCurrentCommit( aCtx.ClientName, _( "Delete Annotation" ) );

    if( m_frame )
    {
        frame()->SyncView();
        frame()->GetCanvas()->Refresh();
        frame()->UpdateNetHighlightStatus();
    }

    return response;
}


//// Board synchronization (Since 11.0) ////

HANDLER_RESULT<SyncSchematicToBoardResponse>
API_HANDLER_SCH::handleSyncSchematicToBoard( const HANDLER_CONTEXT<SyncSchematicToBoard>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.schematic() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    ApiResponseStatus e;
    e.set_status( ApiStatusCode::AS_BAD_REQUEST );

    if( aCtx.Request.board().type() != DocumentType::DOCTYPE_PCB )
    {
        e.set_error_message( "SyncSchematicToBoard.board must name a board" );
        return tl::unexpected( e );
    }

    if( !Server() )
    {
        e.set_error_message( "SyncSchematicToBoard needs a running API server to reach the board" );
        return tl::unexpected( e );
    }

    SCHEMATIC*     sch = schematic();
    SCH_SHEET_LIST sheets = sch->Hierarchy();

    // The netlist needs every symbol annotated, as SCH_EDIT_FRAME::ReadyToNetlist checks
    sheets.AnnotatePowerSymbols();

    SCH_REFERENCE_LIST references;
    sheets.GetSymbols( references, SYMBOL_FILTER_NON_POWER );

    std::vector<std::string> problems;

    references.CheckAnnotation(
            [&]( ERCE_T, const wxString& aMsg, SCH_REFERENCE*, SCH_REFERENCE* )
            {
                problems.push_back( aMsg.ToStdString() );
            } );

    if( !problems.empty() )
    {
        e.set_error_message( fmt::format( "the schematic must be fully annotated before it can be synchronized "
                                          "to the board ({} problem(s)): {}",
                                          problems.size(), fmt::join( problems, "; " ) ) );
        return tl::unexpected( e );
    }

    // The exporter reads the connection graph; make sure it reflects every change
    if( m_frame )
    {
        frame()->RecalculateConnections( nullptr, GLOBAL_CLEANUP );
    }
    else
    {
        SCH_COMMIT dummyCommit( toolManager() );
        sch->RecalculateConnections( &dummyCommit, NO_CLEANUP, toolManager() );
    }

    NETLIST_EXPORTER_KICAD exporter( sch );
    STRING_FORMATTER       formatter;

    exporter.SetKiway( m_context->GetKiway() );
    exporter.Format( &formatter, GNL_ALL | GNL_OPT_KICAD );

    wxString netlistPath = wxFileName::CreateTempFileName( wxS( "kicad-api-netlist-" ) );

    {
        wxFFile file( netlistPath, wxS( "wb" ) );

        if( !file.IsOpened() || !file.Write( formatter.GetString().c_str(), formatter.GetString().size() ) )
        {
            e.set_error_message( fmt::format( "could not write the netlist to '{}'", netlistPath.ToStdString() ) );
            return tl::unexpected( e );
        }
    }

    // Apply it through the board handler, exactly as an ImportNetlist request would
    kiapi::board::commands::ImportNetlist import;
    *import.mutable_board() = aCtx.Request.board();
    import.set_netlist_path( netlistPath.ToUTF8() );
    import.set_dry_run( aCtx.Request.dry_run() );
    import.set_match_mode( aCtx.Request.match_mode() );
    import.set_delete_extra_footprints( aCtx.Request.delete_extra_footprints() );
    import.set_update_footprints( aCtx.Request.update_footprints() );
    import.set_transfer_groups( aCtx.Request.transfer_groups() );
    import.set_override_locks( aCtx.Request.override_locks() );
    import.set_remove_extra_fields( aCtx.Request.remove_extra_fields() );

    if( aCtx.Request.has_update_fields() )
        import.set_update_fields( aCtx.Request.update_fields() );

    ApiRequest request;
    request.mutable_header()->set_client_name( aCtx.ClientName );
    request.mutable_message()->PackFrom( import );

    API_RESULT result = Server()->Dispatch( request );

    wxRemoveFile( netlistPath );

    ApiResponseStatus status;

    if( result.has_value() )
        status = result->status();
    else
        status = result.error();

    if( status.status() == ApiStatusCode::AS_UNHANDLED )
    {
        e.set_error_message( fmt::format( "the board '{}' is not open in this KiCad; open it first",
                                          aCtx.Request.board().board_filename() ) );
        return tl::unexpected( e );
    }
    else if( status.status() != ApiStatusCode::AS_OK )
    {
        return tl::unexpected( status );
    }

    SyncSchematicToBoardResponse response;
    response.set_netlist_path( netlistPath.ToUTF8() );

    if( !result->message().UnpackTo( response.mutable_result() ) )
    {
        e.set_error_message( "the board handler answered ImportNetlist with an unexpected message" );
        return tl::unexpected( e );
    }

    return response;
}


//// Schematic settings (Since 11.0) ////

void API_HANDLER_SCH::packSchematicSettings( kiapi::schematic::commands::SchematicSettings& aOut ) const
{
    const SCHEMATIC_SETTINGS& s = schematic()->Settings();

    PackDistance( *aOut.mutable_default_line_width(), s.m_DefaultLineWidth, schIUScale );
    PackDistance( *aOut.mutable_default_text_size(), s.m_DefaultTextSize, schIUScale );
    aOut.set_label_size_ratio( s.m_LabelSizeRatio );
    aOut.set_text_offset_ratio( s.m_TextOffsetRatio );
    PackDistance( *aOut.mutable_pin_symbol_size(), s.m_PinSymbolSize, schIUScale );

    aOut.set_junction_size_choice( s.m_JunctionSizeChoice );
    aOut.set_hop_over_size_choice( s.m_HopOverSizeChoice );
    aOut.set_show_dnp_markers( s.m_ShowDNPMarkers );
    PackDistance( *aOut.mutable_connection_grid_size(), s.m_ConnectionGridSize, schIUScale );

    aOut.set_annotate_start_number( s.m_AnnotateStartNum );

    switch( static_cast<ANNOTATE_ORDER_T>( s.m_AnnotateSortOrder ) )
    {
    case SORT_BY_X_POSITION: aOut.set_annotate_sort_order( ASO_X_POSITION ); break;
    case SORT_BY_Y_POSITION: aOut.set_annotate_sort_order( ASO_Y_POSITION ); break;
    case UNSORTED:           aOut.set_annotate_sort_order( ASO_UNSORTED );   break;
    }

    switch( static_cast<ANNOTATE_ALGO_T>( s.m_AnnotateMethod ) )
    {
    case INCREMENTAL_BY_REF:  aOut.set_annotate_numbering( ANM_INCREMENTAL );        break;
    case SHEET_NUMBER_X_100:  aOut.set_annotate_numbering( ANM_SHEET_NUMBER_X100 );  break;
    case SHEET_NUMBER_X_1000: aOut.set_annotate_numbering( ANM_SHEET_NUMBER_X1000 ); break;
    }

    aOut.set_intersheet_refs_show( s.m_IntersheetRefsShow );
    aOut.set_intersheet_refs_list_own_page( s.m_IntersheetRefsListOwnPage );
    aOut.set_intersheet_refs_format_short( s.m_IntersheetRefsFormatShort );
    aOut.set_intersheet_refs_prefix( s.m_IntersheetRefsPrefix.ToUTF8() );
    aOut.set_intersheet_refs_suffix( s.m_IntersheetRefsSuffix.ToUTF8() );

    aOut.set_dashed_line_dash_ratio( s.m_DashedLineDashRatio );
    aOut.set_dashed_line_gap_ratio( s.m_DashedLineGapRatio );

    aOut.set_drawing_sheet_file( s.m_SchDrawingSheetFileName.ToUTF8() );
    aOut.set_plot_directory( s.m_PlotDirectoryName.ToUTF8() );

    aOut.set_subpart_id_separator( s.m_SubpartIdSeparator == 0
                                           ? std::string()
                                           : wxString( static_cast<wxChar>( s.m_SubpartIdSeparator ) ).ToStdString() );
    aOut.set_subpart_first_id( wxString( static_cast<wxChar>( s.m_SubpartFirstId ) ).ToStdString() );
}


HANDLER_RESULT<kiapi::schematic::commands::SchematicSettings>
API_HANDLER_SCH::handleGetSchematicSettings( const HANDLER_CONTEXT<GetSchematicSettings>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.schematic() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    kiapi::schematic::commands::SchematicSettings response;
    packSchematicSettings( response );
    return response;
}


HANDLER_RESULT<kiapi::schematic::commands::SchematicSettings>
API_HANDLER_SCH::handleSetSchematicSettings( const HANDLER_CONTEXT<SetSchematicSettings>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.schematic() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    const kiapi::schematic::commands::SchematicSettings& in = aCtx.Request.settings();
    SCHEMATIC_SETTINGS&                                   s = schematic()->Settings();

    ApiResponseStatus e;
    e.set_status( ApiStatusCode::AS_BAD_REQUEST );

    auto positive =
            [&]( const types::Distance& aDistance, const char* aName ) -> std::optional<ApiResponseStatus>
            {
                if( aDistance.value_nm() <= 0 )
                {
                    e.set_error_message( fmt::format( "{} must be positive", aName ) );
                    return e;
                }

                return std::nullopt;
            };

    if( in.has_default_line_width() )
    {
        if( auto err = positive( in.default_line_width(), "default_line_width" ) )
            return tl::unexpected( *err );
    }

    if( in.has_default_text_size() )
    {
        if( auto err = positive( in.default_text_size(), "default_text_size" ) )
            return tl::unexpected( *err );
    }

    if( in.has_pin_symbol_size() )
    {
        if( auto err = positive( in.pin_symbol_size(), "pin_symbol_size" ) )
            return tl::unexpected( *err );
    }

    if( in.has_connection_grid_size() )
    {
        if( auto err = positive( in.connection_grid_size(), "connection_grid_size" ) )
            return tl::unexpected( *err );
    }

    if( in.has_junction_size_choice() && in.junction_size_choice() > 5 )
    {
        e.set_error_message( "junction_size_choice must be 0 (none) to 5 (largest)" );
        return tl::unexpected( e );
    }

    if( in.has_hop_over_size_choice() && in.hop_over_size_choice() > 5 )
    {
        e.set_error_message( "hop_over_size_choice must be 0 (none) to 5 (largest)" );
        return tl::unexpected( e );
    }

    if( in.has_subpart_id_separator() && in.subpart_id_separator().size() > 1 )
    {
        e.set_error_message( "subpart_id_separator must be empty or a single character" );
        return tl::unexpected( e );
    }

    if( in.has_subpart_first_id() && in.subpart_first_id() != "A" && in.subpart_first_id() != "1" )
    {
        e.set_error_message( "subpart_first_id must be \"A\" or \"1\"" );
        return tl::unexpected( e );
    }

    if( in.has_annotate_sort_order() && in.annotate_sort_order() == ASO_UNKNOWN )
    {
        e.set_error_message( "annotate_sort_order must be a known sort order" );
        return tl::unexpected( e );
    }

    if( in.has_annotate_numbering() && in.annotate_numbering() == ANM_UNKNOWN )
    {
        e.set_error_message( "annotate_numbering must be a known numbering method" );
        return tl::unexpected( e );
    }

    if( in.has_default_line_width() )
        s.m_DefaultLineWidth = UnpackDistance( in.default_line_width(), schIUScale );

    if( in.has_default_text_size() )
        s.m_DefaultTextSize = UnpackDistance( in.default_text_size(), schIUScale );

    if( in.has_label_size_ratio() )
        s.m_LabelSizeRatio = in.label_size_ratio();

    if( in.has_text_offset_ratio() )
        s.m_TextOffsetRatio = in.text_offset_ratio();

    if( in.has_pin_symbol_size() )
        s.m_PinSymbolSize = UnpackDistance( in.pin_symbol_size(), schIUScale );

    if( in.has_junction_size_choice() )
        s.m_JunctionSizeChoice = static_cast<int>( in.junction_size_choice() );

    if( in.has_hop_over_size_choice() )
        s.m_HopOverSizeChoice = static_cast<int>( in.hop_over_size_choice() );

    if( in.has_show_dnp_markers() )
        s.m_ShowDNPMarkers = in.show_dnp_markers();

    if( in.has_connection_grid_size() )
        s.m_ConnectionGridSize = UnpackDistance( in.connection_grid_size(), schIUScale );

    if( in.has_annotate_start_number() )
        s.m_AnnotateStartNum = static_cast<int>( in.annotate_start_number() );

    if( in.has_annotate_sort_order() )
    {
        switch( in.annotate_sort_order() )
        {
        case ASO_X_POSITION: s.m_AnnotateSortOrder = SORT_BY_X_POSITION; break;
        case ASO_Y_POSITION: s.m_AnnotateSortOrder = SORT_BY_Y_POSITION; break;
        default:             s.m_AnnotateSortOrder = UNSORTED;           break;
        }
    }

    if( in.has_annotate_numbering() )
    {
        switch( in.annotate_numbering() )
        {
        case ANM_SHEET_NUMBER_X100:  s.m_AnnotateMethod = SHEET_NUMBER_X_100;  break;
        case ANM_SHEET_NUMBER_X1000: s.m_AnnotateMethod = SHEET_NUMBER_X_1000; break;
        default:                     s.m_AnnotateMethod = INCREMENTAL_BY_REF;  break;
        }
    }

    if( in.has_intersheet_refs_show() )
        s.m_IntersheetRefsShow = in.intersheet_refs_show();

    if( in.has_intersheet_refs_list_own_page() )
        s.m_IntersheetRefsListOwnPage = in.intersheet_refs_list_own_page();

    if( in.has_intersheet_refs_format_short() )
        s.m_IntersheetRefsFormatShort = in.intersheet_refs_format_short();

    if( in.has_intersheet_refs_prefix() )
        s.m_IntersheetRefsPrefix = wxString::FromUTF8( in.intersheet_refs_prefix() );

    if( in.has_intersheet_refs_suffix() )
        s.m_IntersheetRefsSuffix = wxString::FromUTF8( in.intersheet_refs_suffix() );

    if( in.has_dashed_line_dash_ratio() )
        s.m_DashedLineDashRatio = in.dashed_line_dash_ratio();

    if( in.has_dashed_line_gap_ratio() )
        s.m_DashedLineGapRatio = in.dashed_line_gap_ratio();

    if( in.has_drawing_sheet_file() )
        setDrawingSheetFileName( wxString::FromUTF8( in.drawing_sheet_file() ) );

    if( in.has_plot_directory() )
        s.m_PlotDirectoryName = wxString::FromUTF8( in.plot_directory() );

    if( in.has_subpart_id_separator() )
        s.m_SubpartIdSeparator = in.subpart_id_separator().empty() ? 0 : in.subpart_id_separator()[0];

    if( in.has_subpart_first_id() )
        s.m_SubpartFirstId = in.subpart_first_id()[0];

    // Sizes derived from the settings (junctions, hop-overs, label text) are drawn from them
    if( m_frame && frame()->GetCanvas() )
    {
        frame()->GetCanvas()->GetView()->UpdateAllItems( KIGFX::ALL );
        frame()->GetCanvas()->Refresh();
    }

    // Project settings are persisted with the schematic (SaveDocument); the document revision
    // moves so that clients re-read them
    bumpRevision();

    kiapi::schematic::commands::SchematicSettings response;
    packSchematicSettings( response );
    publishProjectChanged( kiapi::common::events::PCK_SETTINGS, aCtx.ClientName );

    return response;
}


//// Symbol fields table (Since 11.0) ////

HANDLER_RESULT<SymbolFieldsTableResponse>
API_HANDLER_SCH::handleGetSymbolFieldsTable( const HANDLER_CONTEXT<GetSymbolFieldsTable>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.schematic() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    SCHEMATIC*         sch = schematic();
    wxString           variant = sch->GetCurrentVariant();
    SCH_REFERENCE_LIST references;

    sch->Hierarchy().GetSymbols( references, aCtx.Request.include_power_symbols() ? SYMBOL_FILTER_ALL
                                                                                    : SYMBOL_FILTER_NON_POWER );
    references.SortByReferenceOnly();

    std::set<wxString> wanted;

    for( const std::string& name : aCtx.Request.fields() )
        wanted.insert( wxString::FromUTF8( name ) );

    SymbolFieldsTableResponse response;

    for( size_t i = 0; i < references.GetCount(); i++ )
    {
        SCH_SYMBOL*     symbol = references[i].GetSymbol();
        SCH_SHEET_PATH& path = references[i].GetSheetPath();
        SymbolFieldsRow* row = response.add_rows();

        row->mutable_id()->set_value( symbol->m_Uuid.AsStdString() );
        PackSheetPath( *row->mutable_sheet_path(), path.Path() );
        row->set_reference( symbol->GetRef( &path, false ).ToUTF8() );
        row->set_unit( static_cast<uint32_t>( symbol->GetUnitSelection( &path ) ) );
        row->set_excluded_from_bom( symbol->GetExcludedFromBOM( &path, variant ) );
        row->set_excluded_from_board( symbol->GetExcludedFromBoard( &path, variant ) );
        row->set_do_not_populate( symbol->GetDNP( &path, variant ) );

        for( const SCH_FIELD& field : symbol->GetFields() )
        {
            wxString name = field.GetName();

            if( !wanted.empty() && !wanted.contains( name ) )
                continue;

            wxString value;

            switch( field.GetId() )
            {
            case FIELD_T::REFERENCE: value = symbol->GetRef( &path, false );                       break;
            case FIELD_T::VALUE:     value = symbol->GetValue( false, &path, false, variant );         break;
            case FIELD_T::FOOTPRINT: value = symbol->GetFootprintFieldText( false, &path, false, variant ); break;
            default:                 value = field.GetText();                                         break;
            }

            ( *row->mutable_fields() )[name.ToStdString()] = value.ToStdString();
        }
    }

    return response;
}


HANDLER_RESULT<SetSymbolFieldsResponse>
API_HANDLER_SCH::handleSetSymbolFields( const HANDLER_CONTEXT<SetSymbolFields>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.schematic() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    SCHEMATIC*     sch = schematic();
    SCH_SHEET_LIST hierarchy = sch->Hierarchy();
    wxString       variant = sch->GetCurrentVariant();
    SCH_COMMIT*    commit = static_cast<SCH_COMMIT*>( getCurrentCommit( aCtx.ClientName ) );

    SetSymbolFieldsResponse    response;
    std::set<SCH_SHEET_PATH>   touchedPaths;

    for( const SymbolFieldUpdate& update : aCtx.Request.updates() )
    {
        auto fail =
                [&]( const std::string& aMessage )
                {
                    response.add_errors( fmt::format( "{}: {}", update.id().value(), aMessage ) );
                };

        SCH_SHEET_PATH           path;
        std::optional<SCH_ITEM*> item = getItemById( KIID( update.id().value() ), &path );

        if( !item || ( *item )->Type() != SCH_SYMBOL_T )
        {
            fail( "no such symbol" );
            continue;
        }

        SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( *item );

        if( update.has_sheet_path() )
        {
            std::optional<SCH_SHEET_PATH> requested =
                    hierarchy.GetSheetPathByKIIDPath( UnpackSheetPath( update.sheet_path() ) );

            if( !requested || requested->LastScreen() != path.LastScreen() )
            {
                fail( "the symbol is not placed on the given sheet path" );
                continue;
            }

            path = *requested;
        }

        wxString name = wxString::FromUTF8( update.field() );
        wxString value = wxString::FromUTF8( update.value() );

        if( name.IsEmpty() )
        {
            fail( "a field name is required" );
            continue;
        }

        SCH_FIELD* field = FindField( symbol->GetFields(), name );

        if( update.remove() )
        {
            if( !field )
            {
                fail( fmt::format( "no field named '{}'", update.field() ) );
                continue;
            }

            if( field->IsMandatory() )
            {
                fail( fmt::format( "the {} field cannot be removed", update.field() ) );
                continue;
            }

            commit->Modify( symbol, path.LastScreen() );
            symbol->RemoveField( field );
        }
        else if( field && field->GetId() == FIELD_T::REFERENCE )
        {
            if( value.IsEmpty() )
            {
                fail( "a reference cannot be empty" );
                continue;
            }

            commit->Modify( symbol, path.LastScreen() );
            symbol->SetRef( &path, value );
        }
        else if( field && field->GetId() == FIELD_T::VALUE )
        {
            commit->Modify( symbol, path.LastScreen() );
            symbol->SetValueFieldText( value, &path, variant );
        }
        else if( field && field->GetId() == FIELD_T::FOOTPRINT )
        {
            commit->Modify( symbol, path.LastScreen() );
            symbol->SetFootprintFieldText( value );
        }
        else if( field )
        {
            commit->Modify( symbol, path.LastScreen() );
            field->SetText( value );
        }
        else
        {
            commit->Modify( symbol, path.LastScreen() );

            SCH_FIELD newField( symbol, FIELD_T::USER, name );
            newField.SetText( value );
            newField.SetVisible( false );
            newField.SetTextPos( symbol->GetPosition() );
            symbol->AddField( newField );
        }

        touchedPaths.insert( path );
        response.set_updated_count( response.updated_count() + 1 );
    }

    for( const SCH_SHEET_PATH& path : touchedPaths )
        path.UpdateAllScreenReferences();

    if( !m_activeClients.contains( aCtx.ClientName ) )
        pushCurrentCommit( aCtx.ClientName, _( "Edit Symbol Fields" ) );

    if( m_frame )
    {
        frame()->SyncView();
        frame()->GetCanvas()->Refresh();
    }

    return response;
}


//// Footprint assignment (Since 11.0) ////

HANDLER_RESULT<AssignFootprintsResponse>
API_HANDLER_SCH::handleAssignFootprints( const HANDLER_CONTEXT<AssignFootprints>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.schematic() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    SCH_REFERENCE_LIST references;
    schematic()->Hierarchy().GetSymbols( references, SYMBOL_FILTER_NON_POWER );

    SCH_COMMIT*              commit = static_cast<SCH_COMMIT*>( getCurrentCommit( aCtx.ClientName ) );
    AssignFootprintsResponse response;
    std::set<SCH_SYMBOL*>    assigned;

    for( const FootprintAssignment& assignment : aCtx.Request.assignments() )
    {
        wxString reference = wxString::FromUTF8( assignment.reference() );
        wxString footprint;

        if( !assignment.footprint().library_nickname().empty() || !assignment.footprint().entry_name().empty() )
        {
            LIB_ID id( wxString::FromUTF8( assignment.footprint().library_nickname() ),
                       wxString::FromUTF8( assignment.footprint().entry_name() ) );
            footprint = id.Format().wx_str();
        }

        bool matched = false;

        // Every unit of a multi-unit symbol shares the reference; CvPcb updates them all
        for( size_t i = 0; i < references.GetCount(); i++ )
        {
            if( references[i].GetRef() != reference )
                continue;

            matched = true;

            SCH_SYMBOL* symbol = references[i].GetSymbol();
            SCH_FIELD*  field = symbol->GetField( FIELD_T::FOOTPRINT );
            wxString    oldFootprint = references[i].GetFootprint();

            if( oldFootprint == footprint && !( oldFootprint.IsEmpty() && field->IsVisible() ) )
                continue;

            commit->Modify( symbol, references[i].GetSheetPath().LastScreen(), RECURSE_MODE::NO_RECURSE );

            if( oldFootprint.IsEmpty() && field->IsVisible() )
                field->SetVisible( false );

            field->SetText( footprint );
            assigned.insert( symbol );
        }

        if( !matched )
            response.add_unmatched_references( assignment.reference() );
    }

    response.set_assigned_count( static_cast<uint32_t>( assigned.size() ) );

    if( !m_activeClients.contains( aCtx.ClientName ) )
        pushCurrentCommit( aCtx.ClientName, _( "Assign Footprints" ) );

    if( m_frame )
        frame()->SyncView();

    return response;
}


//// Sheet files (Since 11.0) ////

wxString API_HANDLER_SCH::attachSheetFile( SCH_SHEET* aSheet, const SCH_SHEET_PATH& aParentPath )
{
    wxString fileName = aSheet->GetFileName();

    if( fileName.IsEmpty() )
        return wxS( "a new sheet needs a file name (the Sheetfile field)" );

    SCH_SCREEN* parentScreen = aParentPath.LastScreen();
    wxFileName  parentFile( parentScreen ? parentScreen->GetFileName() : wxString() );
    wxString    baseDir = parentFile.GetPath().IsEmpty() ? project().GetProjectPath() : parentFile.GetPath();

    // Sheet file names are relative to the parent sheet's file, as in the sheet dialog
    wxFileName fn( ExpandTextVars( fileName, &project() ) );

    if( !fn.Normalize( FN_NORMALIZE_FLAGS | wxPATH_NORM_ENV_VARS, baseDir ) )
        return wxString::Format( wxS( "cannot resolve sheet file '%s' against '%s'" ), fileName, baseDir );

    wxString absolute = fn.GetFullPath();
    absolute.Replace( wxT( "\\" ), wxT( "/" ) );

    // A file the hierarchy already uses is shared, as the dialog shares it
    SCH_SCREEN* existing = nullptr;

    if( schematic()->Root().SearchHierarchy( absolute, &existing ) && existing )
    {
        aSheet->SetScreen( existing );
        return wxEmptyString;
    }

    if( wxFileExists( absolute ) )
    {
        SCH_IO_MGR::SCH_FILE_T type = SCH_IO_MGR::GuessPluginTypeFromSchPath( absolute );

        if( type == SCH_IO_MGR::SCH_FILE_UNKNOWN )
            type = SCH_IO_MGR::SCH_KICAD;

        IO_RELEASER<SCH_IO>          pi( SCH_IO_MGR::FindPlugin( type ) );
        std::map<std::string, UTF8>  props;

        props["hierarchical_sheet_load"] = "1";

        // Loaded into a stand-in with the new sheet's UUID so that sub-sheet paths come out right
        std::unique_ptr<SCH_SHEET> loaded = std::make_unique<SCH_SHEET>( schematic() );
        const_cast<KIID&>( loaded->m_Uuid ) = aSheet->m_Uuid;
        loaded->SetFileName( absolute );

        try
        {
            pi->LoadSchematicFile( absolute, schematic(), loaded.get(), &props );
        }
        catch( const IO_ERROR& ioe )
        {
            return wxString::Format( wxS( "could not load sheet file '%s': %s" ), absolute, ioe.What() );
        }

        if( !loaded->GetScreen() )
            return wxString::Format( wxS( "sheet file '%s' has no content" ), absolute );

        SCH_SHEET_LIST loadedSheets( loaded.get() );

        if( parentScreen && schematic()->Hierarchy().TestForRecursion( loadedSheets, parentScreen->GetFileName() ) )
            return wxString::Format( wxS( "sheet file '%s' would create a recursive hierarchy" ), absolute );

        loaded->GetScreen()->MigrateSimModels();
        loadedSheets.AddNewSymbolInstances( aParentPath, project().GetProjectName() );
        loadedSheets.AddNewSheetInstances( aParentPath, schematic()->Hierarchy().GetLastVirtualPageNumber() );

        // The stand-in gives its screen up; the sheet takes it over (and its reference)
        SCH_SCREEN* screen = loaded->GetScreen();
        aSheet->SetScreen( screen );
        loaded->SetScreen( nullptr );

        return wxEmptyString;
    }

    // A new file: an empty sheet with the parent's page settings, written now so that the
    // hierarchy on disk matches what GetSchematicHierarchy reports
    SCH_SCREEN* screen = new SCH_SCREEN( schematic() );
    screen->SetFileName( absolute );
    screen->SetContentModified();

    if( parentScreen )
        screen->SetPageSettings( parentScreen->GetPageSettings() );

    aSheet->SetScreen( screen );

    if( !SCH_API_SAVE::SaveSheetToFile( aSheet, *schematic(), absolute ) )
        return wxString::Format( wxS( "could not create sheet file '%s'" ), absolute );

    return wxEmptyString;
}
