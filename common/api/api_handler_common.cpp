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

#include <ranges>
#include <tuple>

#include <api/api_handler_common.h>
#include <api/api_job_registry.h>
#include <build_version.h>
#include <eda_shape.h>
#include <eda_text.h>
#include <gestfich.h>
#include <font/font.h>
#include <geometry/shape_compound.h>
#include <google/protobuf/empty.pb.h>
#include <paths.h>
#include <pgm_base.h>
#include <api/api_plugin.h>
#include <api/api_utils.h>
#include <project/net_settings.h>
#include <project/project_file.h>
#include <settings/app_settings.h>
#include <settings/color_settings.h>
#include <settings/settings_manager.h>
#include <wx/string.h>

using namespace kiapi::common::commands;
using namespace kiapi::common::types;
using google::protobuf::Empty;


/**
 * Lay out the text of a text box the way PCB_TEXTBOX and PCB_TABLECELL do when they draw: break
 * the text to the column width and move it to the anchor the box's justification and margins
 * imply, so that the glyph shapes come back in document coordinates instead of around the origin.
 *
 * @param aText is the text to place, already carrying the box's attributes.
 * @param aBox is the box as the client sent it.
 * @return the four corners of the box in drawing order, rotated with the text.
 */
static std::vector<VECTOR2I> layOutTextBox( EDA_TEXT& aText, const TextBox& aBox )
{
    VECTOR2I one = kiapi::common::UnpackVector2( aBox.top_left() );
    VECTOR2I two = kiapi::common::UnpackVector2( aBox.bottom_right() );

    const int left = std::min( one.x, two.x );
    const int right = std::max( one.x, two.x );
    const int top = std::min( one.y, two.y );
    const int bottom = std::max( one.y, two.y );

    // The box itself stays axis-aligned; the rotation lives in the text angle
    EDA_ANGLE angle = aText.GetTextAngle();
    angle.Normalize();

    const VECTOR2I center( ( left + right ) / 2, ( top + bottom ) / 2 );

    std::vector<VECTOR2I> corners = { VECTOR2I( left, top ), VECTOR2I( right, top ),
                                      VECTOR2I( right, bottom ), VECTOR2I( left, bottom ) };

    if( !angle.IsZero() )
    {
        for( VECTOR2I& corner : corners )
            RotatePoint( corner, center, angle );
    }

    const int marginLeft = aBox.margin_left().value_nm();
    const int marginTop = aBox.margin_top().value_nm();
    const int marginRight = aBox.margin_right().value_nm();
    const int marginBottom = aBox.margin_bottom().value_nm();

    // Break the text to the column width the box leaves for it
    int colWidth = right - left;

    if( angle.IsHorizontal() )
        colWidth -= marginLeft + marginRight;
    else
        colWidth -= marginTop + marginBottom;

    if( KIFONT::FONT* font = aText.GetDrawFont( nullptr ) )
    {
        wxString shown = aText.GetShownText( true );
        font->LinebreakText( shown, colWidth, aText.GetTextSize(), aText.GetEffectiveTextPenWidth(),
                             aText.IsBold(), aText.IsItalic() );
        aText.SetText( shown );
    }

    GR_TEXT_H_ALIGN_T hAlign = aText.GetHorizJustify();
    GR_TEXT_V_ALIGN_T vAlign = aText.GetVertJustify();

    if( aText.IsMirrored() )
    {
        if( hAlign == GR_TEXT_H_ALIGN_LEFT )
            hAlign = GR_TEXT_H_ALIGN_RIGHT;
        else if( hAlign == GR_TEXT_H_ALIGN_RIGHT )
            hAlign = GR_TEXT_H_ALIGN_LEFT;
    }

    const VECTOR2I midTop = ( corners[0] + corners[1] ) / 2;
    const VECTOR2I midBottom = ( corners[3] + corners[2] ) / 2;
    const VECTOR2I midLeft = ( corners[0] + corners[3] ) / 2;
    const VECTOR2I midRight = ( corners[1] + corners[2] ) / 2;
    const VECTOR2I boxCenter = ( corners[0] + corners[1] + corners[2] + corners[3] ) / 4;

    VECTOR2I anchor = boxCenter;
    VECTOR2I offset;

    if( hAlign != GR_TEXT_H_ALIGN_INDETERMINATE && vAlign != GR_TEXT_V_ALIGN_INDETERMINATE )
    {
        switch( vAlign )
        {
        case GR_TEXT_V_ALIGN_TOP:
            anchor = hAlign == GR_TEXT_H_ALIGN_LEFT    ? corners[0]
                     : hAlign == GR_TEXT_H_ALIGN_RIGHT ? corners[1]
                                                       : midTop;
            break;

        case GR_TEXT_V_ALIGN_BOTTOM:
            anchor = hAlign == GR_TEXT_H_ALIGN_LEFT    ? corners[3]
                     : hAlign == GR_TEXT_H_ALIGN_RIGHT ? corners[2]
                                                       : midBottom;
            break;

        default:
            anchor = hAlign == GR_TEXT_H_ALIGN_LEFT    ? midLeft
                     : hAlign == GR_TEXT_H_ALIGN_RIGHT ? midRight
                                                       : boxCenter;
            break;
        }

        if( hAlign == GR_TEXT_H_ALIGN_LEFT )
            offset.x = marginLeft;
        else if( hAlign == GR_TEXT_H_ALIGN_RIGHT )
            offset.x = -marginRight;

        if( vAlign == GR_TEXT_V_ALIGN_TOP )
            offset.y = marginTop;
        else if( vAlign == GR_TEXT_V_ALIGN_BOTTOM )
            offset.y = -marginBottom;

        RotatePoint( offset, angle );
    }

    aText.SetTextPos( anchor + offset );

    return corners;
}


API_HANDLER_COMMON::API_HANDLER_COMMON() :
        API_HANDLER()
{
    registerHandler<commands::GetVersion, GetVersionResponse>( &API_HANDLER_COMMON::handleGetVersion );
    registerHandler<GetKiCadBinaryPath, PathResponse>(
            &API_HANDLER_COMMON::handleGetKiCadBinaryPath );
    registerHandler<GetPaths, GetPathsResponse>( &API_HANDLER_COMMON::handleGetPaths );
    registerHandler<GetNetClasses, NetClassesResponse>( &API_HANDLER_COMMON::handleGetNetClasses );
    registerHandler<SetNetClasses, Empty>( &API_HANDLER_COMMON::handleSetNetClasses );
    registerHandler<Ping, Empty>( &API_HANDLER_COMMON::handlePing );

    // Since 11.0
    registerHandler<ListColorThemes, ColorThemesResponse>( &API_HANDLER_COMMON::handleListColorThemes );
    registerHandler<GetColorTheme, ColorThemeResponse>( &API_HANDLER_COMMON::handleGetColorTheme );
    registerHandler<commands::GetAppSettings, AppSettings>( &API_HANDLER_COMMON::handleGetAppSettings );
    registerHandler<GetTextExtents, types::Box2>( &API_HANDLER_COMMON::handleGetTextExtents );
    registerHandler<GetTextAsShapes, GetTextAsShapesResponse>(
            &API_HANDLER_COMMON::handleGetTextAsShapes );
    registerHandler<ExpandTextVariables, ExpandTextVariablesResponse>(
            &API_HANDLER_COMMON::handleExpandTextVariables );
    registerHandler<GetPluginSettingsPath, StringResponse>(
            &API_HANDLER_COMMON::handleGetPluginSettingsPath );
    registerHandler<GetTextVariables, project::TextVariables>(
            &API_HANDLER_COMMON::handleGetTextVariables );
    registerHandler<SetTextVariables, Empty>(
            &API_HANDLER_COMMON::handleSetTextVariables );
    registerHandler<OpenDocument, OpenDocumentResponse>(
            &API_HANDLER_COMMON::handleOpenDocument );
    registerHandler<CloseDocument, Empty>(
            &API_HANDLER_COMMON::handleCloseDocument );
    registerHandler<CloseAllDocuments, Empty>(
            &API_HANDLER_COMMON::handleCloseAllDocuments );
    registerHandler<NewProject, OpenDocumentResponse>( &API_HANDLER_COMMON::handleNewProject );
    registerHandler<NewDocument, OpenDocumentResponse>( &API_HANDLER_COMMON::handleNewDocument );
    registerHandler<GetProjectInfo, ProjectInfoResponse>( &API_HANDLER_COMMON::handleGetProjectInfo );
    registerHandler<GetJobStatus, GetJobStatusResponse>( &API_HANDLER_COMMON::handleGetJobStatus );

}


HANDLER_RESULT<GetVersionResponse> API_HANDLER_COMMON::handleGetVersion(
        const HANDLER_CONTEXT<commands::GetVersion>& )
{
    GetVersionResponse reply;

    reply.mutable_version()->set_full_version( GetBuildVersion().ToStdString() );

    std::tuple<int, int, int> version = GetMajorMinorPatchTuple();
    reply.mutable_version()->set_major( std::get<0>( version ) );
    reply.mutable_version()->set_minor( std::get<1>( version ) );
    reply.mutable_version()->set_patch( std::get<2>( version ) );

    return reply;
}


HANDLER_RESULT<PathResponse> API_HANDLER_COMMON::handleGetKiCadBinaryPath(
        const HANDLER_CONTEXT<GetKiCadBinaryPath>& aCtx )
{
    wxFileName fn( wxEmptyString, wxString::FromUTF8( aCtx.Request.binary_name() ) );
#ifdef _WIN32
    fn.SetExt( wxT( "exe" ) );
#endif

    wxString path = FindKicadFile( fn.GetFullName() );
    PathResponse reply;
    reply.set_path( path.ToUTF8() );
    return reply;
}


HANDLER_RESULT<NetClassesResponse> API_HANDLER_COMMON::handleGetNetClasses(
        const HANDLER_CONTEXT<GetNetClasses>& aCtx )
{
    NetClassesResponse reply;

    std::shared_ptr<NET_SETTINGS>& netSettings =
            Pgm().GetSettingsManager().Prj().GetProjectFile().m_NetSettings;

    google::protobuf::Any any;

    netSettings->GetDefaultNetclass()->Serialize( any );
    any.UnpackTo( reply.add_net_classes() );

    for( const auto& netClass : netSettings->GetNetclasses() | std::views::values )
    {
        netClass->Serialize( any );
        any.UnpackTo( reply.add_net_classes() );
    }

    return reply;
}


HANDLER_RESULT<Empty> API_HANDLER_COMMON::handleSetNetClasses(
        const HANDLER_CONTEXT<SetNetClasses>& aCtx )
{
    std::shared_ptr<NET_SETTINGS>& netSettings =
            Pgm().GetSettingsManager().Prj().GetProjectFile().m_NetSettings;

    if( aCtx.Request.merge_mode() == MapMergeMode::MMM_REPLACE )
        netSettings->ClearNetclasses();

    auto netClasses = netSettings->GetNetclasses();
    google::protobuf::Any any;

    for( const auto& ncProto : aCtx.Request.net_classes() )
    {
        any.PackFrom( ncProto );
        wxString name = wxString::FromUTF8( ncProto.name() );

        if( name == wxT( "Default" ) )
        {
            netSettings->GetDefaultNetclass()->Deserialize( any );
        }
        else
        {
            if( !netClasses.contains( name ) )
                netClasses.insert( { name, std::make_shared<NETCLASS>( name, false ) } );

            netClasses[name]->Deserialize( any );
        }
    }

    netSettings->SetNetclasses( netClasses );
    publishProjectChanged( kiapi::common::events::PCK_NET_CLASSES, aCtx.ClientName );

    return Empty();
}


HANDLER_RESULT<Empty> API_HANDLER_COMMON::handlePing( const HANDLER_CONTEXT<Ping>& aCtx )
{
    return Empty();
}


HANDLER_RESULT<types::Box2> API_HANDLER_COMMON::handleGetTextExtents(
        const HANDLER_CONTEXT<GetTextExtents>& aCtx )
{
    EDA_TEXT text( pcbIUScale );
    google::protobuf::Any any;
    any.PackFrom( aCtx.Request.text() );

    if( !text.Deserialize( any ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "Could not decode text in GetTextExtents message" );
        return tl::unexpected( e );
    }

    types::Box2 response;

    BOX2I bbox = text.GetTextBox( nullptr );
    EDA_ANGLE angle = text.GetTextAngle();

    if( !angle.IsZero() )
        bbox = bbox.GetBoundingBoxRotated( text.GetTextPos(), text.GetTextAngle() );

    response.mutable_position()->set_x_nm( bbox.GetPosition().x );
    response.mutable_position()->set_y_nm( bbox.GetPosition().y );
    response.mutable_size()->set_x_nm( bbox.GetSize().x );
    response.mutable_size()->set_y_nm( bbox.GetSize().y );

    return response;
}


HANDLER_RESULT<GetTextAsShapesResponse> API_HANDLER_COMMON::handleGetTextAsShapes(
        const HANDLER_CONTEXT<GetTextAsShapes>& aCtx )
{
    GetTextAsShapesResponse reply;

    for( const TextOrTextBox& textMsg : aCtx.Request.text() )
    {
        Text dummyText;
        const Text* textPtr = &textMsg.text();

        if( textMsg.has_textbox() )
        {
            dummyText.set_text( textMsg.textbox().text() );
            dummyText.mutable_attributes()->CopyFrom( textMsg.textbox().attributes() );
            textPtr = &dummyText;
        }

        EDA_TEXT text( pcbIUScale );
        google::protobuf::Any any;
        any.PackFrom( *textPtr );

        if( !text.Deserialize( any ) )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( "Could not decode text in GetTextAsShapes message" );
            return tl::unexpected( e );
        }

        // An item with no thickness of its own is plotted with the default pen for its size, and
        // the stroke font lays glyphs out around that pen, so fill it in before asking for shapes
        text.SetTextThickness( text.GetEffectiveTextPenWidth() );

        std::vector<VECTOR2I> corners;

        // A text box lays its text out relative to the box, so the glyphs come back where the
        // plotter draws them and not around the origin
        if( textMsg.has_textbox() )
            corners = layOutTextBox( text, textMsg.textbox() );

        std::shared_ptr<SHAPE_COMPOUND> shapes = text.GetEffectiveTextShape( false );

        TextWithShapes* entry = reply.add_text_with_shapes();
        entry->mutable_text()->CopyFrom( textMsg );

        for( SHAPE* subshape : shapes->Shapes() )
        {
            EDA_SHAPE proxy( *subshape );
            proxy.Serialize( any );
            GraphicShape* shapeMsg = entry->mutable_shapes()->add_shapes();
            any.UnpackTo( shapeMsg );
        }

        // The border follows the box, so it turns with the text
        for( size_t ii = 0; ii < corners.size(); ii++ )
        {
            GraphicShape* border = entry->mutable_shapes()->add_shapes();
            int width = textMsg.textbox().attributes().stroke_width().value_nm();
            border->mutable_attributes()->mutable_stroke()->mutable_width()->set_value_nm( width );
            PackVector2( *border->mutable_segment()->mutable_start(), corners[ii] );
            PackVector2( *border->mutable_segment()->mutable_end(), corners[( ii + 1 ) % corners.size()] );
        }
    }

    return reply;
}


HANDLER_RESULT<ExpandTextVariablesResponse> API_HANDLER_COMMON::handleExpandTextVariables(
        const HANDLER_CONTEXT<ExpandTextVariables>& aCtx )
{
    if( !aCtx.Request.has_document() || aCtx.Request.document().type() != DOCTYPE_PROJECT )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        // No error message, this is a flag that the server should try a different handler
        return tl::unexpected( e );
    }

    ExpandTextVariablesResponse reply;
    PROJECT& project = Pgm().GetSettingsManager().Prj();

    for( const std::string& textMsg : aCtx.Request.text() )
    {
        wxString result = ExpandTextVars( wxString::FromUTF8( textMsg ), &project );

        if( aCtx.Request.expand_env_vars() )
            result = ExpandEnvVarSubstitutions( result, &project );

        reply.add_text( result.ToUTF8() );
    }

    return reply;
}


HANDLER_RESULT<GetJobStatusResponse> API_HANDLER_COMMON::handleGetJobStatus(
        const HANDLER_CONTEXT<GetJobStatus>& aCtx )
{
    if( std::optional<GetJobStatusResponse> status = API_JOB_REGISTRY::Instance().Status( aCtx.Request.job_id() ) )
        return *status;

    ApiResponseStatus e;
    e.set_status( ApiStatusCode::AS_BAD_REQUEST );
    e.set_error_message( fmt::format( "unknown job id {}", aCtx.Request.job_id() ) );
    return tl::unexpected( e );
}


HANDLER_RESULT<StringResponse> API_HANDLER_COMMON::handleGetPluginSettingsPath(
        const HANDLER_CONTEXT<GetPluginSettingsPath>& aCtx )
{
    wxString identifier = wxString::FromUTF8( aCtx.Request.identifier() );

    if( identifier.IsEmpty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "plugin identifier is missing" );
        return tl::unexpected( e );
    }

    if( !API_PLUGIN::IsValidIdentifier( identifier ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "plugin identifier is invalid" );
        return tl::unexpected( e );
    }

    wxFileName path( PATHS::GetUserSettingsPath(), wxEmptyString );
    path.AppendDir( "plugins" );

    // Create the base plugins path if needed, but leave the specific plugin to create its own path
    PATHS::EnsurePathExists( path.GetPath() );

    path.AppendDir( identifier );

    StringResponse reply;
    reply.set_response( path.GetPath() );
    return reply;
}


HANDLER_RESULT<project::TextVariables> API_HANDLER_COMMON::handleGetTextVariables(
        const HANDLER_CONTEXT<GetTextVariables>& aCtx )
{
    if( !aCtx.Request.has_document() || aCtx.Request.document().type() != DOCTYPE_PROJECT )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        // No error message, this is a flag that the server should try a different handler
        return tl::unexpected( e );
    }

    const PROJECT& project = Pgm().GetSettingsManager().Prj();

    if( project.IsNullProject() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_NOT_READY );
        e.set_error_message( "no valid project is loaded, cannot get text variables" );
        return tl::unexpected( e );
    }

    const std::map<wxString, wxString>& vars = project.GetTextVars();

    project::TextVariables reply;
    auto map = reply.mutable_variables();

    for( const auto& [key, value] : vars )
        ( *map )[ std::string( key.ToUTF8() ) ] = value.ToUTF8();

    return reply;
}


HANDLER_RESULT<Empty> API_HANDLER_COMMON::handleSetTextVariables(
    const HANDLER_CONTEXT<SetTextVariables>& aCtx )
{
    if( !aCtx.Request.has_document() || aCtx.Request.document().type() != DOCTYPE_PROJECT )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        // No error message, this is a flag that the server should try a different handler
        return tl::unexpected( e );
    }

    PROJECT& project = Pgm().GetSettingsManager().Prj();

    if( project.IsNullProject() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_NOT_READY );
        e.set_error_message( "no valid project is loaded, cannot set text variables" );
        return tl::unexpected( e );
    }

    const project::TextVariables& newVars = aCtx.Request.variables();
    std::map<wxString, wxString>& vars = project.GetTextVars();

    if( aCtx.Request.merge_mode() == MapMergeMode::MMM_REPLACE )
        vars.clear();

    for( const auto& [key, value] : newVars.variables() )
        vars[wxString::FromUTF8( key )] = wxString::FromUTF8( value );

    Pgm().GetSettingsManager().SaveProject();
    publishProjectChanged( kiapi::common::events::PCK_TEXT_VARIABLES, aCtx.ClientName );

    return Empty();
}


HANDLER_RESULT<OpenDocumentResponse> API_HANDLER_COMMON::handleNewProject(
        const HANDLER_CONTEXT<NewProject>& aCtx )
{
    if( !m_newProjectHandler )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNIMPLEMENTED );
        e.set_error_message( "NewProject is not available in this KiCad mode" );
        return tl::unexpected( e );
    }

    return m_newProjectHandler( aCtx.Request );
}


HANDLER_RESULT<OpenDocumentResponse> API_HANDLER_COMMON::handleNewDocument(
        const HANDLER_CONTEXT<NewDocument>& aCtx )
{
    if( !m_newDocumentHandler )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNIMPLEMENTED );
        e.set_error_message( "NewDocument is not available in this KiCad mode" );
        return tl::unexpected( e );
    }

    return m_newDocumentHandler( aCtx.Request );
}


HANDLER_RESULT<ProjectInfoResponse> API_HANDLER_COMMON::handleGetProjectInfo(
        const HANDLER_CONTEXT<GetProjectInfo>& aCtx )
{
    if( !m_getProjectInfoHandler )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNIMPLEMENTED );
        e.set_error_message( "GetProjectInfo is not available in this KiCad mode" );
        return tl::unexpected( e );
    }

    return m_getProjectInfoHandler( aCtx.Request );
}


HANDLER_RESULT<OpenDocumentResponse> API_HANDLER_COMMON::handleOpenDocument(
        const HANDLER_CONTEXT<OpenDocument>& aCtx )
{
    if( !m_openDocumentHandler )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNIMPLEMENTED );
        e.set_error_message( "OpenDocument is not available in this KiCad mode" );
        return tl::unexpected( e );
    }

    return m_openDocumentHandler( aCtx.Request );
}


HANDLER_RESULT<Empty> API_HANDLER_COMMON::handleCloseDocument(
        const HANDLER_CONTEXT<CloseDocument>& aCtx )
{
    if( !m_closeDocumentHandler )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNIMPLEMENTED );
        e.set_error_message( "CloseDocument is not available in this KiCad mode" );
        return tl::unexpected( e );
    }

    return m_closeDocumentHandler( aCtx.Request );
}


HANDLER_RESULT<Empty> API_HANDLER_COMMON::handleCloseAllDocuments( const HANDLER_CONTEXT<CloseAllDocuments>& aCtx )
{
    if( !m_closeAllDocumentsHandler )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNIMPLEMENTED );
        e.set_error_message( "CloseAllDocuments is not available in this KiCad mode" );
        return tl::unexpected( e );
    }

    return m_closeAllDocumentsHandler( aCtx.Request );
}


HANDLER_RESULT<GetPathsResponse> API_HANDLER_COMMON::handleGetPaths( const HANDLER_CONTEXT<GetPaths>& )
{
    GetPathsResponse reply;

    auto addPath = [&]( types::PathType aType, const wxString& aPath )
    {
        PathEntry* entry = reply.add_paths();
        entry->set_type( aType );
        entry->set_path( aPath.ToUTF8() );
    };

    addPath( types::PATH_USER_PLUGINS, PATHS::GetUserPluginsPath() );
    addPath( types::PATH_USER_TEMPLATES, PATHS::GetUserTemplatesPath() );
    addPath( types::PATH_USER_SETTINGS, PATHS::GetUserSettingsPath() );
    addPath( types::PATH_STOCK_SYMBOLS, PATHS::GetStockSymbolsPath() );
    addPath( types::PATH_STOCK_FOOTPRINTS, PATHS::GetStockFootprintsPath() );
    addPath( types::PATH_STOCK_DESIGN_BLOCKS, PATHS::GetStockDesignBlocksPath() );
    addPath( types::PATH_STOCK_3DMODELS, PATHS::GetStock3dmodelsPath() );
    addPath( types::PATH_STOCK_TEMPLATES, PATHS::GetStockTemplatesPath() );

    return reply;
}


//// Settings (Since 11.0) ////

namespace
{

void packThemeInfo( ColorThemeInfo* aOut, const COLOR_SETTINGS* aTheme )
{
    aOut->set_name( aTheme->GetName().ToUTF8() );
    aOut->set_read_only( aTheme->IsReadOnly() );

    // Built-in themes have no file
    if( !aTheme->IsReadOnly() || !aTheme->GetFilename().StartsWith( wxS( "_" ) ) )
        aOut->set_filename( aTheme->GetFilename().ToUTF8() );
}

} // namespace


HANDLER_RESULT<ColorThemesResponse>
API_HANDLER_COMMON::handleListColorThemes( const HANDLER_CONTEXT<ListColorThemes>& aCtx )
{
    ColorThemesResponse response;

    for( const COLOR_SETTINGS* theme : Pgm().GetSettingsManager().GetColorSettingsList() )
        packThemeInfo( response.add_themes(), theme );

    return response;
}


HANDLER_RESULT<ColorThemeResponse> API_HANDLER_COMMON::handleGetColorTheme( const HANDLER_CONTEXT<GetColorTheme>& aCtx )
{
    SETTINGS_MANAGER& manager = Pgm().GetSettingsManager();
    wxString          name = wxString::FromUTF8( aCtx.Request.name() );
    COLOR_SETTINGS*   theme = nullptr;

    if( name.IsEmpty() )
        name = COLOR_SETTINGS::COLOR_BUILTIN_DEFAULT;

    // GetColorSettings invents a theme for an unknown name; only answer for known ones
    for( COLOR_SETTINGS* candidate : manager.GetColorSettingsList() )
    {
        if( candidate->GetFilename() == name || candidate->GetName().CmpNoCase( name ) == 0 )
        {
            theme = candidate;
            break;
        }
    }

    if( !theme )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "no color theme named '{}'; see ListColorThemes", aCtx.Request.name() ) );
        return tl::unexpected( e );
    }

    ColorThemeResponse response;
    packThemeInfo( response.mutable_theme(), theme );
    response.set_override_schematic_item_colors( theme->GetOverrideSchItemColors() );

    for( const auto& [key, layer] : theme->GetColorKeys() )
    {
        ColorThemeEntry* entry = response.add_colors();
        entry->set_key( key );
        entry->set_layer( layer );

        KIGFX::COLOR4D color = theme->GetColor( layer );
        entry->mutable_color()->set_r( color.r );
        entry->mutable_color()->set_g( color.g );
        entry->mutable_color()->set_b( color.b );
        entry->mutable_color()->set_a( color.a );
    }

    return response;
}


void API_HANDLER_COMMON::publishProjectChanged( kiapi::common::events::ProjectChangeKind aKind,
                                                const std::string& aClientName )
{
    if( !Server() )
        return;

    PROJECT& project = Pgm().GetSettingsManager().Prj();

    if( project.IsNullProject() )
        return;

    kiapi::common::events::Event           event;
    kiapi::common::events::ProjectChanged& changed = *event.mutable_project_changed();
    changed.mutable_project()->set_name( project.GetProjectName().ToUTF8() );
    changed.mutable_project()->set_path( project.GetProjectPath().ToUTF8() );
    changed.set_kind( aKind );
    changed.set_client_name( aClientName );
    publish( event );
}


wxString API_HANDLER_COMMON::AppSettingsFilename( AppType aApp )
{
    switch( aApp )
    {
    case APP_PCB_EDITOR:       return wxS( "pcbnew" );
    case APP_SCHEMATIC_EDITOR: return wxS( "eeschema" );
    case APP_FOOTPRINT_EDITOR: return wxS( "fpedit" );
    case APP_SYMBOL_EDITOR:    return wxS( "symbol_editor" );
    default:                   return wxEmptyString;
    }
}


HANDLER_RESULT<AppSettings> API_HANDLER_COMMON::handleGetAppSettings( const HANDLER_CONTEXT<commands::GetAppSettings>& aCtx )
{
    ApiResponseStatus e;
    e.set_status( ApiStatusCode::AS_BAD_REQUEST );

    wxString filename = AppSettingsFilename( aCtx.Request.app() );

    if( filename.IsEmpty() )
    {
        e.set_error_message( "app must be one of the editors" );
        return tl::unexpected( e );
    }

    SETTINGS_MANAGER& manager = Pgm().GetSettingsManager();

    if( !manager.GetSettingsByFilename( filename ) && m_ensureAppSettingsHandler )
    {
        wxString error;

        if( !m_ensureAppSettingsHandler( aCtx.Request.app(), &error ) )
        {
            e.set_error_message( fmt::format( "the editor's settings are not available: {}", error.ToUTF8().data() ) );
            return tl::unexpected( e );
        }
    }

    JSON_SETTINGS* json = manager.GetSettingsByFilename( filename );

    if( !json )
    {
        e.set_status( ApiStatusCode::AS_NOT_READY );
        e.set_error_message( fmt::format( "the settings of '{}' are not loaded in this KiCad", filename.ToUTF8().data() ) );
        return tl::unexpected( e );
    }

    // Every editor's settings file derives from APP_SETTINGS_BASE; a dynamic_cast cannot be
    // trusted across the kiface boundary, so the file name stands for the type
    APP_SETTINGS_BASE* app = static_cast<APP_SETTINGS_BASE*>( json );
    AppSettings        response;

    response.set_app( aCtx.Request.app() );
    response.set_settings_file( app->GetFullFilename().ToUTF8() );
    response.set_color_theme( app->m_ColorTheme.ToUTF8() );
    response.set_max_undo_items( static_cast<uint32_t>( std::max( 0, app->m_System.max_undo_items ) ) );

    switch( static_cast<EDA_UNITS>( app->m_System.units ) )
    {
    case EDA_UNITS::INCH: response.set_units( US_INCHES );      break;
    case EDA_UNITS::MM:   response.set_units( US_MILLIMETRES ); break;
    case EDA_UNITS::MILS: response.set_units( US_MILS );        break;
    default:                                                    break;
    }

    const GRID_SETTINGS& grid = app->m_Window.grid;

    for( const GRID& definition : grid.grids )
    {
        GridDefinition* out = response.add_grids();
        out->set_name( definition.name.ToUTF8() );
        out->set_x( definition.x.ToUTF8() );
        out->set_y( definition.y.ToUTF8() );
    }

    response.set_current_grid( static_cast<uint32_t>( std::max( 0, grid.last_size_idx ) ) );
    response.set_grid_visible( grid.show );
    response.set_grid_axes_visible( grid.axes_enabled );
    response.set_grid_style( static_cast<uint32_t>( std::max( 0, grid.style ) ) );
    response.set_grid_snap( static_cast<uint32_t>( std::max( 0, grid.snap ) ) );

    for( double factor : app->m_Window.zoom_factors )
        response.add_zoom_factors( factor );

    // Item defaults live in the settings file's "drawing" section (schematic and symbol editors);
    // the board editors keep theirs in the document
    app->Store();

    if( std::optional<nlohmann::json> drawing = app->GetJson( "drawing" ) )
    {
        if( drawing->is_object() )
        {
            for( auto it = drawing->begin(); it != drawing->end(); ++it )
            {
                const nlohmann::json& value = it.value();

                if( value.is_string() )
                    ( *response.mutable_defaults() )[it.key()] = value.get<std::string>();
                else if( value.is_boolean() )
                    ( *response.mutable_defaults() )[it.key()] = value.get<bool>() ? "true" : "false";
                else if( value.is_number() )
                    ( *response.mutable_defaults() )[it.key()] = value.dump();
            }
        }
    }

    return response;
}
