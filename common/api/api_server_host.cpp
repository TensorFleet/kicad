/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 * @author Jon Evans <jon@craftyjon.com>
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

#include <algorithm>
#include <vector>

#include <api/api_handler_library.h>
#include <api/api_server.h>
#include <api/api_server_host.h>
#include <api/api_utils.h>
#include <build_version.h>
#include <ki_exception.h>
#include <kiid.h>
#include <lib_id.h>
#include <paths.h>
#include <pgm_base.h>
#include <project.h>
#include <project_template.h>
#include <settings/settings_manager.h>
#include <wildcards_and_files_ext.h>
#include <wx/dir.h>
#include <wx/ffile.h>
#include <wx/filename.h>

#include <../eeschema/sch_file_versions.h>                     // for SEXPR_SCHEMATIC_FILE_VERSION def
#include <../pcbnew/pcb_io/kicad_sexpr/pcb_io_kicad_sexpr.h>   // for SEXPR_BOARD_FILE_VERSION def


API_SERVER_HOST::API_SERVER_HOST( KIWAY& aKiway, KICAD_API_SERVER& aServer ) :
        m_kiway( aKiway ),
        m_server( aServer )
{
}


API_SERVER_HOST::~API_SERVER_HOST()
{
    Shutdown();

    // Shutdown drops the design block handler with the project, but a host that never opened one
    // (or that failed part-way through) may still have it registered.
    if( m_designBlockLibraries )
    {
        m_server.DeregisterHandler( m_designBlockLibraries.get() );
        m_designBlockLibraries.reset();
    }
}


void API_SERVER_HOST::Install()
{
    if( m_installed )
        return;

    m_commonHandler.SetOpenDocumentHandler(
            [this]( const commands::OpenDocument& aRequest )
            {
                return openDocument( aRequest );
            } );

    m_commonHandler.SetCloseDocumentHandler(
            [this]( const commands::CloseDocument& aRequest )
            {
                return closeDocument( aRequest );
            } );

    m_commonHandler.SetCloseAllDocumentsHandler(
            [this]( const commands::CloseAllDocuments& aRequest )
            {
                return closeAllDocuments( aRequest );
            } );

    m_commonHandler.SetNewProjectHandler(
            [this]( const commands::NewProject& aRequest )
            {
                return newProject( aRequest );
            } );

    m_commonHandler.SetNewDocumentHandler(
            [this]( const commands::NewDocument& aRequest )
            {
                return newDocument( aRequest );
            } );

    m_commonHandler.SetGetProjectInfoHandler(
            [this]( const commands::GetProjectInfo& aRequest )
            {
                return getProjectInfo( aRequest );
            } );

    m_commonHandler.SetEnsureAppSettingsHandler(
            [this]( commands::AppType aApp, wxString* aError )
            {
                return ensureAppSettings( aApp, aError );
            } );

    m_server.RegisterHandler( &m_commonHandler );
    m_installed = true;
}


void API_SERVER_HOST::Shutdown()
{
    if( !m_installed )
        return;

    closeAllDocuments( commands::CloseAllDocuments() );
    m_server.DeregisterHandler( &m_commonHandler );
    m_installed = false;
}


bool API_SERVER_HOST::Preload( const wxString& aPath, wxString* aError )
{
    if( aPath.IsEmpty() )
        return true;

    wxFileName          preloadFile( aPath );
    types::DocumentType preloadType = types::DOCTYPE_PROJECT;

    if( preloadFile.GetExt() == FILEEXT::KiCadSchematicFileExtension )
        preloadType = types::DOCTYPE_SCHEMATIC;
    else if( preloadFile.GetExt() == FILEEXT::KiCadPcbFileExtension )
        preloadType = types::DOCTYPE_PCB;

    commands::OpenDocument request;
    request.set_type( preloadType );
    request.set_path( aPath.ToStdString() );

    HANDLER_RESULT<commands::OpenDocumentResponse> result = openDocument( request );

    if( !result )
    {
        if( aError )
            *aError = wxString::FromUTF8( result.error().error_message() );

        return false;
    }

    return true;
}


KIWAY::FACE_T API_SERVER_HOST::faceForDocument( types::DocumentType aType )
{
    switch( aType )
    {
    case types::DOCTYPE_SCHEMATIC:  return KIWAY::FACE_SCH;
    case types::DOCTYPE_SYMBOL:     return KIWAY::FACE_SCH;
    case types::DOCTYPE_PCB:        return KIWAY::FACE_PCB;
    case types::DOCTYPE_FOOTPRINT:  return KIWAY::FACE_PCB;
    default:                        return KIWAY::KIWAY_FACE_COUNT;
    }
}


void API_SERVER_HOST::publishProjectEvent( const PROJECT& aProject, bool aOpened )
{
    kiapi::common::events::Event event;
    types::DocumentSpecifier* doc = aOpened ? event.mutable_document_opened()->mutable_document()
                                            : event.mutable_document_closed()->mutable_document();
    doc->set_type( types::DOCTYPE_PROJECT );
    doc->mutable_project()->set_name( aProject.GetProjectName().ToUTF8() );
    doc->mutable_project()->set_path( aProject.GetProjectPath().ToUTF8() );
    m_server.Publish( std::move( event ) );
}


void API_SERVER_HOST::notifyProjectFaces( const wxFileName& aProjectPath, bool aOpened )
{
    if( m_designBlockLibraries )
    {
        m_server.DeregisterHandler( m_designBlockLibraries.get() );
        m_designBlockLibraries.reset();
    }

    if( aOpened )
    {
        m_designBlockLibraries = std::make_unique<API_HANDLER_LIBRARY>( LIBRARY_TABLE_TYPE::DESIGN_BLOCK, nullptr,
                                                                       &Pgm().GetSettingsManager().Prj() );
        m_server.RegisterHandler( m_designBlockLibraries.get() );
    }

    KIFACE::DOCUMENT_SPEC spec;
    spec.kind = KIFACE::DOCUMENT_SPEC::KIND::PROJECT_KIND;
    spec.path = aProjectPath.GetFullPath();

    for( KIWAY::FACE_T face : { KIWAY::FACE_PCB, KIWAY::FACE_SCH } )
    {
        wxString error;
        bool     ok = aOpened ? m_kiway.ProcessApiOpenDocument( face, spec, &m_server, &error )
                              : m_kiway.ProcessApiCloseDocument( face, spec, &m_server, &error );

        if( !ok )
            wxLogTrace( traceApi, "Project %s notification failed: %s", aOpened ? "open" : "close", error );
    }
}


KIFACE::DOCUMENT_SPEC API_SERVER_HOST::closeSpec( const OPEN_DOCUMENT& aDoc, bool aExact )
{
    KIFACE::DOCUMENT_SPEC spec;

    if( aDoc.type == types::DOCTYPE_FOOTPRINT || aDoc.type == types::DOCTYPE_SYMBOL )
    {
        spec.kind = KIFACE::DOCUMENT_SPEC::KIND::FPID_KIND;

        if( aExact )
            spec.libId = aDoc.libId;
    }
    else
    {
        spec.kind = KIFACE::DOCUMENT_SPEC::KIND::FILE_KIND;

        if( aExact )
            spec.path = aDoc.fileName;
    }

    return spec;
}


HANDLER_RESULT<Empty> API_SERVER_HOST::closeAllDocuments( const commands::CloseAllDocuments& aRequest )
{
    for( const OPEN_DOCUMENT& doc : m_openDocuments )
    {
        // The project has no document face; it is released by UnloadProject below.
        if( doc.type == types::DOCTYPE_PROJECT )
            continue;

        wxString error;
        m_kiway.ProcessApiCloseDocument( faceForDocument( doc.type ), closeSpec( doc, false ), &m_server,
                                         &error );
    }

    m_openDocuments.clear();

    if( m_openProjectPath )
    {
        notifyProjectFaces( *m_openProjectPath, false );

        PROJECT& project = Pgm().GetSettingsManager().Prj();
        publishProjectEvent( project, false );
        Pgm().GetSettingsManager().UnloadProject( &project, false );
    }

    m_openProjectPath.reset();

    return Empty();
}


HANDLER_RESULT<commands::OpenDocumentResponse> API_SERVER_HOST::openDocument(
        const commands::OpenDocument& aRequest )
{
    types::DocumentType requestType = aRequest.type();

    if( requestType != types::DOCTYPE_PCB && requestType != types::DOCTYPE_SCHEMATIC
        && requestType != types::DOCTYPE_PROJECT && requestType != types::DOCTYPE_FOOTPRINT
        && requestType != types::DOCTYPE_SYMBOL )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNIMPLEMENTED );
        e.set_error_message(
                requestType == types::DOCTYPE_DRAWING_SHEET
                        ? "Drawing sheets cannot be opened as a document; they are selected "
                          "by name through SetPageSettings"
                        : "Only PCB, schematic, footprint, symbol, and project document "
                          "types are supported" );
        return tl::unexpected( e );
    }

    wxString inputPath = wxString::FromUTF8( aRequest.path() );

    if( inputPath.IsEmpty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "OpenDocument requires a non-empty path" );
        return tl::unexpected( e );
    }

    if( requestType == types::DOCTYPE_FOOTPRINT || requestType == types::DOCTYPE_SYMBOL )
    {
        LIB_ID fpid;

        if( fpid.Parse( inputPath ) >= 0 )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( wxString::Format( wxS( "Invalid LIB_ID: %s" ), inputPath ).ToStdString() );
            return tl::unexpected( e );
        }

        KIFACE::DOCUMENT_SPEC spec;
        spec.kind = KIFACE::DOCUMENT_SPEC::KIND::FPID_KIND;
        spec.libId = fpid;

        if( m_openProjectPath )
            spec.path = m_openProjectPath->GetFullPath();

        wxString error;

        if( !m_kiway.ProcessApiOpenDocument( faceForDocument( requestType ), spec, &m_server, &error ) )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( error.ToStdString() );
            return tl::unexpected( e );
        }

        // The kiface closes any library document of the same type it had open
        std::erase_if( m_openDocuments,
                       [&]( const OPEN_DOCUMENT& d )
                       {
                           return d.type == requestType;
                       } );

        OPEN_DOCUMENT doc;
        doc.type = requestType;
        doc.libId = fpid;
        m_openDocuments.push_back( doc );

        commands::OpenDocumentResponse response;
        types::DocumentSpecifier* docSpec = response.mutable_document();
        docSpec->set_type( requestType );
        docSpec->mutable_lib_id()->set_library_nickname( fpid.GetUniStringLibNickname() );
        docSpec->mutable_lib_id()->set_entry_name( fpid.GetUniStringLibItemName() );

        return response;
    }

    wxFileName projectPath( inputPath );
    projectPath.SetExt( FILEEXT::ProjectFileExtension );
    projectPath.MakeAbsolute();

    if( m_openProjectPath && projectPath.GetFullPath() != m_openProjectPath->GetFullPath() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( wxString::Format( "cannot open a document from project '%s' because project "
                                               "'%s' is already open.",
                                               projectPath.GetFullName(), m_openProjectPath->GetFullName() )
                                     .ToStdString() );
        return tl::unexpected( e );
    }

    if( requestType == types::DOCTYPE_PROJECT )
    {
        if( !m_openProjectPath )
        {
            if( !m_openDocuments.empty() )
            {
                auto closeResult = closeAllDocuments( commands::CloseAllDocuments() );

                if( !closeResult )
                    return tl::unexpected( closeResult.error() );
            }

            if( !Pgm().GetSettingsManager().LoadProject( projectPath.GetFullPath(), true ) )
            {
                wxLogTrace( traceApi, "Warning: no project file found for %s", inputPath );
            }

            if( !Pgm().GetSettingsManager().GetProject( projectPath.GetFullPath() ) )
            {
                ApiResponseStatus e;
                e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                e.set_error_message( wxString::Format( "failed to load project '%s'", projectPath.GetFullPath() )
                                             .ToStdString() );
                return tl::unexpected( e );
            }

            m_openProjectPath = projectPath;
            publishProjectEvent( Pgm().GetSettingsManager().Prj(), true );
            notifyProjectFaces( projectPath, true );
        }

        if( std::ranges::find_if( m_openDocuments,
                                  []( const OPEN_DOCUMENT& d )
                                  {
                                      return d.type == types::DOCTYPE_PROJECT;
                                  } ) == m_openDocuments.end() )
        {
            OPEN_DOCUMENT doc;
            doc.type = types::DOCTYPE_PROJECT;
            doc.fileName = projectPath.GetFullName();
            m_openDocuments.push_back( doc );
        }

        commands::OpenDocumentResponse response;
        types::DocumentSpecifier*      doc = response.mutable_document();
        PROJECT&                       project = Pgm().GetSettingsManager().Prj();

        doc->set_type( types::DOCTYPE_PROJECT );
        doc->mutable_project()->set_name( project.GetProjectName().ToUTF8() );
        doc->mutable_project()->set_path( project.GetProjectPath().ToUTF8() );

        return response;
    }

    KIWAY::FACE_T face = faceForDocument( requestType );

    if( face == KIWAY::KIWAY_FACE_COUNT )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "unsupported document type" );
        return tl::unexpected( e );
    }

    if( requestType == types::DOCTYPE_PCB || requestType == types::DOCTYPE_SCHEMATIC )
    {
        auto existing = std::ranges::find_if( m_openDocuments,
                                              [&]( const OPEN_DOCUMENT& d )
                                              {
                                                  return d.type == requestType;
                                              } );

        if( existing != m_openDocuments.end() )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( "a document of this type is already open" );
            return tl::unexpected( e );
        }
    }

    KIFACE::DOCUMENT_SPEC spec;
    spec.kind = KIFACE::DOCUMENT_SPEC::KIND::FILE_KIND;
    spec.path = projectPath.GetFullPath();

    wxString error;

    if( !m_kiway.ProcessApiOpenDocument( face, spec, &m_server, &error ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( error.ToStdString() );
        return tl::unexpected( e );
    }

    wxFileName docFile( inputPath );
    docFile.MakeAbsolute();

    OPEN_DOCUMENT doc;
    doc.type = requestType;
    doc.fileName = docFile.GetFullName();
    m_openDocuments.push_back( doc );

    // Opening a board or schematic implicitly opens its project
    if( !m_openProjectPath )
    {
        publishProjectEvent( Pgm().GetSettingsManager().Prj(), true );
        notifyProjectFaces( projectPath, true );
    }

    m_openProjectPath = projectPath;

    commands::OpenDocumentResponse response;
    types::DocumentSpecifier*      docSpec = response.mutable_document();
    PROJECT&                       project = Pgm().GetSettingsManager().Prj();

    docSpec->set_type( requestType );

    if( requestType == types::DOCTYPE_PCB )
        docSpec->set_board_filename( doc.fileName.ToStdString() );

    docSpec->mutable_project()->set_name( project.GetProjectName().ToUTF8() );
    docSpec->mutable_project()->set_path( project.GetProjectPath().ToUTF8() );

    return response;
}


HANDLER_RESULT<Empty> API_SERVER_HOST::closeDocument( const commands::CloseDocument& aRequest )
{
    if( m_openDocuments.empty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "No document is currently open" );
        return tl::unexpected( e );
    }

    auto it = m_openDocuments.end();

    if( aRequest.has_document() )
    {
        types::DocumentType typeToClose = aRequest.document().type();

        it = std::ranges::find_if( m_openDocuments,
                                   [&]( const OPEN_DOCUMENT& d )
                                   {
                                       return d.type == typeToClose;
                                   } );

        if( it == m_openDocuments.end() )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( "Requested document type does not match any open document" );
            return tl::unexpected( e );
        }

        if( typeToClose == types::DOCTYPE_PCB
            && !aRequest.document().board_filename().empty()
            && it->fileName != wxString::FromUTF8( aRequest.document().board_filename() ) )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( "Requested document does not match the open document" );
            return tl::unexpected( e );
        }

        if( ( typeToClose == types::DOCTYPE_SCHEMATIC || typeToClose == types::DOCTYPE_PROJECT )
            && aRequest.document().has_project()
            && m_openProjectPath
            && aRequest.document().project().name() != m_openProjectPath->GetName().ToStdString() )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( "Requested document does not match the open project" );
            return tl::unexpected( e );
        }

        if( ( typeToClose == types::DOCTYPE_FOOTPRINT || typeToClose == types::DOCTYPE_SYMBOL )
            && aRequest.document().has_lib_id() )
        {
            LIB_ID fpid = UnpackLibId( aRequest.document().lib_id() );

            if( !fpid.IsValid() )
            {
                ApiResponseStatus e;
                e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                e.set_error_message( wxString::Format( wxS( "Invalid LIB_ID: %s" ),
                                                       fpid.GetUniStringLibId() ).ToStdString() );
                return tl::unexpected( e );
            }

            // The kiface checks it against the item actually open (see closeSpec)
            it->libId = fpid;
        }
    }
    else
    {
        // No document specifier: close the first open document.
        it = m_openDocuments.begin();
    }

    if( it->type == types::DOCTYPE_PROJECT )
    {
        return closeAllDocuments( commands::CloseAllDocuments() );
    }
    else
    {
        wxString error;
        bool     exact = ( it->type != types::DOCTYPE_FOOTPRINT && it->type != types::DOCTYPE_SYMBOL )
                     || aRequest.document().has_lib_id();

        if( !m_kiway.ProcessApiCloseDocument( faceForDocument( it->type ), closeSpec( *it, exact ), &m_server,
                                              &error ) )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( error.ToStdString() );
            return tl::unexpected( e );
        }
    }

    m_openDocuments.erase( it );

    if( m_openDocuments.empty() && m_openProjectPath )
    {
        notifyProjectFaces( *m_openProjectPath, false );

        PROJECT& project = Pgm().GetSettingsManager().Prj();
        publishProjectEvent( project, false );
        Pgm().GetSettingsManager().UnloadProject( &project, false );
        m_openProjectPath.reset();
    }

    return Empty();
}


bool API_SERVER_HOST::writeStubDocument( const wxFileName& aFile, types::DocumentType aType )
{
    wxFFile file( aFile.GetFullPath(), "wb" );

    if( !file.IsOpened() )
        return false;

    if( aType == types::DOCTYPE_SCHEMATIC )
    {
        return file.Write( wxString::Format( "(kicad_sch\n"
                                             "\t(version %d)\n"
                                             "\t(generator \"eeschema\")\n"
                                             "\t(generator_version \"%s\")\n"
                                             "\t(uuid %s)\n"
                                             "\t(paper \"A4\")\n"
                                             "\t(lib_symbols)\n"
                                             "\t(sheet_instances\n"
                                             "\t\t(path \"/\"\n"
                                             "\t\t\t(page \"1\")\n"
                                             "\t\t)\n"
                                             "\t)\n"
                                             "\t(embedded_fonts no)\n"
                                             ")",
                                             SEXPR_SCHEMATIC_FILE_VERSION, GetMajorMinorVersion(),
                                             KIID().AsString() ) );
    }

    return file.Write( wxString::Format( "(kicad_pcb (version %d) (generator \"pcbnew\") "
                                         "(generator_version \"%s\")\n)",
                                         SEXPR_BOARD_FILE_VERSION, GetMajorMinorVersion() ) );
}


HANDLER_RESULT<commands::OpenDocumentResponse> API_SERVER_HOST::newProject( const commands::NewProject& aRequest )
{
    wxString inputPath = wxString::FromUTF8( aRequest.path() );

    if( inputPath.IsEmpty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "NewProject requires a non-empty path" );
        return tl::unexpected( e );
    }

    wxFileName pro;

    // A directory (existing, or a path without the project extension) names the project
    if( wxFileName::DirExists( inputPath ) || wxFileName( inputPath ).GetExt() != FILEEXT::ProjectFileExtension )
    {
        pro.AssignDir( inputPath );
        wxArrayString dirs = pro.GetDirs();

        if( dirs.IsEmpty() )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( "NewProject path must name a project file or a directory" );
            return tl::unexpected( e );
        }

        pro.SetName( dirs.Last() );
        pro.SetExt( FILEEXT::ProjectFileExtension );
    }
    else
    {
        pro.Assign( inputPath );
    }

    pro.MakeAbsolute();

    if( pro.FileExists() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( wxString::Format( "project '%s' already exists", pro.GetFullPath() ).ToStdString() );
        return tl::unexpected( e );
    }

    if( !pro.DirExists() && !wxFileName::Mkdir( pro.GetPath(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( wxString::Format( "cannot create folder '%s'", pro.GetPath() ).ToStdString() );
        return tl::unexpected( e );
    }

    if( aRequest.has_template_path() && !aRequest.template_path().empty() )
    {
        wxString templatePath = wxString::FromUTF8( aRequest.template_path() );

        if( !wxFileName::DirExists( templatePath ) )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( wxString::Format( "template folder '%s' does not exist", templatePath )
                                         .ToStdString() );
            return tl::unexpected( e );
        }

        PROJECT_TEMPLATE projectTemplate( templatePath );
        wxString         error;
        wxFileName       newProjectPath( pro );

        if( !projectTemplate.CreateProject( newProjectPath, &error ) )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( wxString::Format( "failed to create project from template: %s", error )
                                         .ToStdString() );
            return tl::unexpected( e );
        }
    }

    if( !pro.FileExists() )
    {
        // The stock blank project, falling back to a minimal file as the project manager does
        wxFileName stock( PATHS::GetStockTemplatesPath(), wxS( "kicad" ), FILEEXT::ProjectFileExtension );

        if( !stock.FileExists() || !wxCopyFile( stock.GetFullPath(), pro.GetFullPath() ) )
        {
            wxFFile file( pro.GetFullPath(), "wb" );

            if( !file.IsOpened() || !file.Write( wxT( "{\n}\n" ) ) )
            {
                ApiResponseStatus e;
                e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                e.set_error_message( wxString::Format( "cannot write '%s'", pro.GetFullPath() ).ToStdString() );
                return tl::unexpected( e );
            }
        }
    }

    // Stub root schematic and board, as the project manager creates them
    if( !aRequest.skip_stub_documents() )
    {
        wxFileName sch( pro );
        sch.SetExt( FILEEXT::KiCadSchematicFileExtension );

        if( !sch.FileExists() )
            writeStubDocument( sch, types::DOCTYPE_SCHEMATIC );

        wxFileName pcb( pro );
        pcb.SetExt( FILEEXT::KiCadPcbFileExtension );
        wxFileName legacyPcb( pro );
        legacyPcb.SetExt( FILEEXT::LegacyPcbFileExtension );

        if( !pcb.FileExists() && !legacyPcb.FileExists() )
            writeStubDocument( pcb, types::DOCTYPE_PCB );
    }

    if( aRequest.open() )
    {
        if( auto closeResult = closeAllDocuments( commands::CloseAllDocuments() ); !closeResult )
            return tl::unexpected( closeResult.error() );

        commands::OpenDocument openRequest;
        openRequest.set_type( types::DOCTYPE_PROJECT );
        openRequest.set_path( pro.GetFullPath().ToUTF8() );
        return openDocument( openRequest );
    }

    commands::OpenDocumentResponse response;
    types::DocumentSpecifier*      doc = response.mutable_document();
    doc->set_type( types::DOCTYPE_PROJECT );
    doc->mutable_project()->set_name( pro.GetName().ToUTF8() );
    doc->mutable_project()->set_path( pro.GetPathWithSep().ToUTF8() );
    return response;
}


HANDLER_RESULT<commands::OpenDocumentResponse> API_SERVER_HOST::newDocument( const commands::NewDocument& aRequest )
{
    types::DocumentType type = aRequest.type();

    if( type != types::DOCTYPE_SCHEMATIC && type != types::DOCTYPE_PCB )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "NewDocument can only create schematic or PCB documents" );
        return tl::unexpected( e );
    }

    if( !m_openProjectPath )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "NewDocument requires an open project" );
        return tl::unexpected( e );
    }

    wxString ext = type == types::DOCTYPE_SCHEMATIC ? FILEEXT::KiCadSchematicFileExtension
                                                    : FILEEXT::KiCadPcbFileExtension;
    wxFileName file( *m_openProjectPath );
    file.SetExt( ext );

    if( !aRequest.path().empty() )
    {
        wxFileName requested( wxString::FromUTF8( aRequest.path() ) );

        if( requested.GetExt().IsEmpty() )
            requested.SetExt( ext );

        if( !requested.IsAbsolute() )
            requested.MakeAbsolute( m_openProjectPath->GetPath() );

        // Documents are located through the project name, so only the main files can be
        // opened once created
        if( requested.GetFullPath() != file.GetFullPath() )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( wxString::Format( "only the project's main document '%s' can be created",
                                                   file.GetFullName() ).ToStdString() );
            return tl::unexpected( e );
        }
    }

    if( file.FileExists() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( wxString::Format( "'%s' already exists", file.GetFullPath() ).ToStdString() );
        return tl::unexpected( e );
    }

    if( !writeStubDocument( file, type ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( wxString::Format( "cannot write '%s'", file.GetFullPath() ).ToStdString() );
        return tl::unexpected( e );
    }

    commands::OpenDocument openRequest;
    openRequest.set_type( type );
    openRequest.set_path( file.GetFullPath().ToUTF8() );
    return openDocument( openRequest );
}


HANDLER_RESULT<commands::ProjectInfoResponse> API_SERVER_HOST::getProjectInfo( const commands::GetProjectInfo& )
{
    if( !m_openProjectPath )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "no project is open" );
        return tl::unexpected( e );
    }

    PROJECT& project = Pgm().GetSettingsManager().Prj();

    commands::ProjectInfoResponse response;
    response.mutable_project()->set_name( project.GetProjectName().ToUTF8() );
    response.mutable_project()->set_path( project.GetProjectPath().ToUTF8() );
    response.set_kicad_pro_path( m_openProjectPath->GetFullPath().ToUTF8() );

    auto isOpen = [&]( types::DocumentType aType, const wxString& aFullName )
    {
        return std::ranges::any_of( m_openDocuments,
                                    [&]( const OPEN_DOCUMENT& d )
                                    {
                                        return d.type == aType && ( aType == types::DOCTYPE_PROJECT
                                                                    || d.fileName == aFullName );
                                    } );
    };

    wxDir dir( m_openProjectPath->GetPath() );

    if( !dir.IsOpened() )
        return response;

    std::vector<wxString> names;
    wxString              name;

    for( bool more = dir.GetFirst( &name, wxEmptyString, wxDIR_FILES | wxDIR_DIRS ); more;
         more = dir.GetNext( &name ) )
    {
        names.push_back( name );
    }

    std::ranges::sort( names );

    for( const wxString& entry : names )
    {
        wxFileName fn( m_openProjectPath->GetPath(), entry );
        wxString   ext = fn.GetExt();
        bool       isDir = wxFileName::DirExists( fn.GetFullPath() );

        commands::ProjectFile file;
        file.set_kind( commands::PFT_UNKNOWN );
        file.set_type( types::DOCTYPE_UNKNOWN );

        if( isDir )
        {
            if( ext != FILEEXT::KiCadFootprintLibPathExtension )
                continue;

            file.set_kind( commands::PFT_FOOTPRINT_LIBRARY );
        }
        else if( ext == FILEEXT::ProjectFileExtension )
        {
            if( fn.GetFullName() != m_openProjectPath->GetFullName() )
                continue;

            file.set_kind( commands::PFT_PROJECT );
            file.set_type( types::DOCTYPE_PROJECT );
            file.set_is_open( true );
        }
        else if( ext == FILEEXT::KiCadSchematicFileExtension )
        {
            file.set_kind( commands::PFT_SCHEMATIC );
            file.set_is_root( fn.GetName() == m_openProjectPath->GetName() );

            // Only the root sheet is addressable as a document; sub-sheets load with it
            if( file.is_root() )
            {
                file.set_type( types::DOCTYPE_SCHEMATIC );
                file.set_is_open( isOpen( types::DOCTYPE_SCHEMATIC, fn.GetFullName() ) );
            }
        }
        else if( ext == FILEEXT::KiCadPcbFileExtension )
        {
            file.set_kind( commands::PFT_PCB );

            if( fn.GetName() == m_openProjectPath->GetName() )
            {
                file.set_type( types::DOCTYPE_PCB );
                file.set_is_open( isOpen( types::DOCTYPE_PCB, fn.GetFullName() ) );
            }
        }
        else if( ext == FILEEXT::DesignRulesFileExtension )
            file.set_kind( commands::PFT_DESIGN_RULES );
        else if( ext == FILEEXT::ProjectLocalSettingsFileExtension )
            file.set_kind( commands::PFT_LOCAL_SETTINGS );
        else if( ext == FILEEXT::KiCadSymbolLibFileExtension )
            file.set_kind( commands::PFT_SYMBOL_LIBRARY );
        else if( ext == FILEEXT::DrawingSheetFileExtension )
            file.set_kind( commands::PFT_DRAWING_SHEET );
        else if( ext == FILEEXT::KiCadJobSetFileExtension )
            file.set_kind( commands::PFT_JOBSET );
        else if( fn.GetFullName() == FILEEXT::SymbolLibraryTableFileName )
            file.set_kind( commands::PFT_SYMBOL_LIB_TABLE );
        else if( fn.GetFullName() == FILEEXT::FootprintLibraryTableFileName )
            file.set_kind( commands::PFT_FOOTPRINT_LIB_TABLE );
        else
            continue;

        file.set_path( fn.GetFullPath().ToUTF8() );
        *response.add_files() = std::move( file );
    }

    return response;
}


bool API_SERVER_HOST::ensureAppSettings( commands::AppType aApp, wxString* aError )
{
    KIWAY::FACE_T face = KIWAY::KIWAY_FACE_COUNT;

    switch( aApp )
    {
    case commands::APP_PCB_EDITOR:
    case commands::APP_FOOTPRINT_EDITOR: face = KIWAY::FACE_PCB; break;
    case commands::APP_SCHEMATIC_EDITOR:
    case commands::APP_SYMBOL_EDITOR:    face = KIWAY::FACE_SCH; break;
    default:                                                     break;
    }

    if( face == KIWAY::KIWAY_FACE_COUNT )
    {
        if( aError )
            *aError = wxS( "unknown editor" );

        return false;
    }

    try
    {
        return m_kiway.KiFACE( face ) != nullptr;
    }
    catch( const IO_ERROR& ioe )
    {
        if( aError )
            *aError = ioe.What();

        return false;
    }
}
