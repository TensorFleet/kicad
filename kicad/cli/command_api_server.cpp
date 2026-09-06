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
#include <atomic>
#include <chrono>
#include <csignal>
#include <vector>

#include <api/api_handler_common.h>
#include <api/api_handler_library.h>
#include <api/api_utils.h>
#include <api/api_server.h>
#include <build_version.h>
#include <cli/exit_codes.h>
#include <lib_id.h>
#include <paths.h>
#include <sch_file_versions.h>
#include <settings/settings_manager.h>
#include <wildcards_and_files_ext.h>
#include <wx/app.h>
#include <wx/crt.h>
#include <wx/dir.h>
#include <wx/ffile.h>
#include <wx/filename.h>

#include <../pcbnew/pcb_io/kicad_sexpr/pcb_io_kicad_sexpr.h>   // for SEXPR_BOARD_FILE_VERSION def

#include "command_api_server.h"
#include "project_template.h"

#define ARG_PATH "path"
#define ARG_SOCKET "--socket"
#define ARG_TOKEN "--token"
#define ARG_NO_EVENTS "--no-events"


std::atomic_bool g_apiServerExitRequested{ false };

void apiServerSignalHandler( int )
{
    g_apiServerExitRequested.store( true );
}


CLI::API_SERVER_COMMAND::API_SERVER_COMMAND() :
        COMMAND( "api-server" )
{
    m_argParser.add_description( UTF8STDSTR( _( "Run the KiCad IPC API server in headless mode" ) ) );

    m_argParser.add_argument( ARG_PATH )
            .default_value( std::string() )
            .nargs( argparse::nargs_pattern::optional )
            .help( UTF8STDSTR( _( "Optional path to a .kicad_pro, .kicad_pcb, or .kicad_sch file to pre-load" ) ) )
            .metavar( "PROJECT_OR_FILE" );

    m_argParser.add_argument( ARG_SOCKET )
            .default_value( std::string() )
            .help( UTF8STDSTR( _( "Listen at this socket path or nng URL (ipc://path, tcp://host:port, "
                                  "ws://host:port/path) instead of the default socket" ) ) )
            .metavar( "SOCKET_PATH_OR_URL" );

    m_argParser.add_argument( ARG_TOKEN )
            .default_value( std::string() )
            .help( UTF8STDSTR( _( "Use this API token instead of a random one" ) ) )
            .metavar( "TOKEN" );

    m_argParser.add_argument( ARG_NO_EVENTS )
            .help( UTF8STDSTR( _( "Do not open the events socket" ) ) )
            .flag();
}


int CLI::API_SERVER_COMMAND::doPerform( KIWAY& aKiway )
{
    using namespace kiapi::common;

    std::unique_ptr<KICAD_API_SERVER> server = std::make_unique<KICAD_API_SERVER>( false );
    API_HANDLER_COMMON                commonHandler;

    wxString socketPath = wxString::FromUTF8( m_argParser.get<std::string>( ARG_SOCKET ) );

    if( !socketPath.IsEmpty() )
        server->SetSocketPath( socketPath );

    server->SetToken( m_argParser.get<std::string>( ARG_TOKEN ) );
    server->SetPublishEvents( !m_argParser.get<bool>( ARG_NO_EVENTS ) );

    // Eventually we might support opening multiple projects at once, but for now
    // we support one project at a time, but multiple documents within that project
    // (e.g. up to one schematic, up to one board, and arbitrarily many library files
    // which are not associated with the project)
    std::optional<wxFileName> openProjectPath;

    struct OPEN_DOCUMENT
    {
        types::DocumentType type;
        wxString            fileName;
        LIB_ID              libId;
    };

    std::vector<OPEN_DOCUMENT> openDocuments;

    auto faceForDocument = []( types::DocumentType aType ) -> KIWAY::FACE_T
    {
        switch( aType )
        {
        case types::DOCTYPE_SCHEMATIC:  return KIWAY::FACE_SCH;
        case types::DOCTYPE_SYMBOL:     return KIWAY::FACE_SCH;
        case types::DOCTYPE_PCB:        return KIWAY::FACE_PCB;
        case types::DOCTYPE_FOOTPRINT:  return KIWAY::FACE_PCB;
        default:                        return KIWAY::KIWAY_FACE_COUNT;
        }
    };

    // The project has no API handler of its own, so its open/close events are published here
    auto publishProjectEvent = [&]( const PROJECT& aProject, bool aOpened )
    {
        kiapi::common::events::Event    event;
        types::DocumentSpecifier* doc = aOpened ? event.mutable_document_opened()->mutable_document()
                                                : event.mutable_document_closed()->mutable_document();
        doc->set_type( types::DOCTYPE_PROJECT );
        doc->mutable_project()->set_name( aProject.GetProjectName().ToUTF8() );
        doc->mutable_project()->set_path( aProject.GetProjectPath().ToUTF8() );
        server->Publish( std::move( event ) );
    };

    // The library commands are served by the pcbnew and eeschema kifaces for the open project;
    // both are told when it opens and closes.  Failure to load a face is not fatal here.  The
    // design block tables have no kiface of their own and are served (tables only) from here.
    std::unique_ptr<API_HANDLER_LIBRARY> designBlockLibraries;

    auto notifyProjectFaces = [&]( const wxFileName& aProjectPath, bool aOpened )
    {
        if( designBlockLibraries )
        {
            server->DeregisterHandler( designBlockLibraries.get() );
            designBlockLibraries.reset();
        }

        if( aOpened )
        {
            designBlockLibraries = std::make_unique<API_HANDLER_LIBRARY>( LIBRARY_TABLE_TYPE::DESIGN_BLOCK, nullptr,
                                                                           &Pgm().GetSettingsManager().Prj() );
            server->RegisterHandler( designBlockLibraries.get() );
        }

        KIFACE::DOCUMENT_SPEC spec;
        spec.kind = KIFACE::DOCUMENT_SPEC::KIND::PROJECT_KIND;
        spec.path = aProjectPath.GetFullPath();

        for( KIWAY::FACE_T face : { KIWAY::FACE_PCB, KIWAY::FACE_SCH } )
        {
            wxString error;
            bool     ok = aOpened ? aKiway.ProcessApiOpenDocument( face, spec, server.get(), &error )
                                  : aKiway.ProcessApiCloseDocument( face, spec, server.get(), &error );

            if( !ok )
                wxLogTrace( traceApi, "Project %s notification failed: %s", aOpened ? "open" : "close", error );
        }
    };

    // How the kiface addresses a document: library items by LIB_ID, files by name.  The kiface
    // owns the truth about what is open (OpenLibraryItem can switch the open library item), so
    // aExact = false closes whatever document of that kind it has.
    auto closeSpec = []( const OPEN_DOCUMENT& aDoc, bool aExact )
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
    };

    auto closeAllDocuments =
            [&]( const commands::CloseAllDocuments& aRequest ) -> HANDLER_RESULT<google::protobuf::Empty>
    {
        for( const OPEN_DOCUMENT& doc : openDocuments )
        {
            // The project has no document face; it is released by UnloadProject below.
            if( doc.type == types::DOCTYPE_PROJECT )
                continue;

            wxString error;
            aKiway.ProcessApiCloseDocument( faceForDocument( doc.type ), closeSpec( doc, false ), server.get(),
                                            &error );
        }

        openDocuments.clear();

        if( openProjectPath )
        {
            notifyProjectFaces( *openProjectPath, false );

            PROJECT& project = Pgm().GetSettingsManager().Prj();
            publishProjectEvent( project, false );
            Pgm().GetSettingsManager().UnloadProject( &project, false );
        }

        openProjectPath.reset();

        return google::protobuf::Empty();
    };

    auto openDocument = [&]( const commands::OpenDocument& aRequest )
            -> HANDLER_RESULT<commands::OpenDocumentResponse>
    {
        types::DocumentType requestType = aRequest.type();

        if( requestType != types::DOCTYPE_PCB && requestType != types::DOCTYPE_SCHEMATIC
            && requestType != types::DOCTYPE_PROJECT && requestType != types::DOCTYPE_FOOTPRINT
            && requestType != types::DOCTYPE_SYMBOL )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_UNIMPLEMENTED );
            e.set_error_message( "Only PCB, schematic, footprint, symbol, and project document types are supported" );
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

            if( openProjectPath )
                spec.path = openProjectPath->GetFullPath();

            wxString error;

            if( !aKiway.ProcessApiOpenDocument( faceForDocument( requestType ), spec, server.get(), &error ) )
            {
                ApiResponseStatus e;
                e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                e.set_error_message( error.ToStdString() );
                return tl::unexpected( e );
            }

            // The kiface closes any library document of the same type it had open
            std::erase_if( openDocuments,
                           [&]( const OPEN_DOCUMENT& d )
                           {
                               return d.type == requestType;
                           } );

            OPEN_DOCUMENT doc;
            doc.type = requestType;
            doc.libId = fpid;
            openDocuments.push_back( doc );

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

        if( openProjectPath && projectPath.GetFullPath() != openProjectPath->GetFullPath() )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( wxString::Format( "cannot open a document from project '%s' because project "
                                                   "'%s' is already open.",
                                                   projectPath.GetFullName(), openProjectPath->GetFullName() )
                                         .ToStdString() );
            return tl::unexpected( e );
        }

        if( requestType == types::DOCTYPE_PROJECT )
        {
            if( !openProjectPath )
            {
                if( !openDocuments.empty() )
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

                openProjectPath = projectPath;
                publishProjectEvent( Pgm().GetSettingsManager().Prj(), true );
                notifyProjectFaces( projectPath, true );
            }

            if( std::ranges::find_if( openDocuments,
                                      []( const OPEN_DOCUMENT& d )
                                      {
                                          return d.type == types::DOCTYPE_PROJECT;
                                      } ) == openDocuments.end() )
            {
                OPEN_DOCUMENT doc;
                doc.type = types::DOCTYPE_PROJECT;
                doc.fileName = projectPath.GetFullName();
                openDocuments.push_back( doc );
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
            auto existing = std::ranges::find_if( openDocuments,
                                                  [&]( const OPEN_DOCUMENT& d )
                                                  {
                                                      return d.type == requestType;
                                                  } );

            if( existing != openDocuments.end() )
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

        if( !aKiway.ProcessApiOpenDocument( face, spec, server.get(), &error ) )
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
        openDocuments.push_back( doc );

        // Opening a board or schematic implicitly opens its project
        if( !openProjectPath )
        {
            publishProjectEvent( Pgm().GetSettingsManager().Prj(), true );
            notifyProjectFaces( projectPath, true );
        }

        openProjectPath = projectPath;

        commands::OpenDocumentResponse response;
        types::DocumentSpecifier*      docSpec = response.mutable_document();
        PROJECT&                       project = Pgm().GetSettingsManager().Prj();

        docSpec->set_type( requestType );

        if( requestType == types::DOCTYPE_PCB )
            docSpec->set_board_filename( doc.fileName.ToStdString() );

        docSpec->mutable_project()->set_name( project.GetProjectName().ToUTF8() );
        docSpec->mutable_project()->set_path( project.GetProjectPath().ToUTF8() );

        return response;
    };

    auto closeDocument =
            [&]( const commands::CloseDocument& aRequest ) -> HANDLER_RESULT<google::protobuf::Empty>
    {
        if( openDocuments.empty() )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( "No document is currently open" );
            return tl::unexpected( e );
        }

        auto it = openDocuments.end();

        if( aRequest.has_document() )
        {
            types::DocumentType typeToClose = aRequest.document().type();

            it = std::ranges::find_if( openDocuments,
                                       [&]( const OPEN_DOCUMENT& d )
                                       {
                                           return d.type == typeToClose;
                                       } );

            if( it == openDocuments.end() )
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
                && openProjectPath
                && aRequest.document().project().name() != openProjectPath->GetName().ToStdString() )
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
            it = openDocuments.begin();
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

            if( !aKiway.ProcessApiCloseDocument( faceForDocument( it->type ), closeSpec( *it, exact ), server.get(),
                                                 &error ) )
            {
                ApiResponseStatus e;
                e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                e.set_error_message( error.ToStdString() );
                return tl::unexpected( e );
            }
        }

        openDocuments.erase( it );

        if( openDocuments.empty() && openProjectPath )
        {
            notifyProjectFaces( *openProjectPath, false );

            PROJECT& project = Pgm().GetSettingsManager().Prj();
            publishProjectEvent( project, false );
            Pgm().GetSettingsManager().UnloadProject( &project, false );
            openProjectPath.reset();
        }

        return google::protobuf::Empty();
    };

    // Minimal files, identical to the stubs the project manager writes for a new project
    auto writeStubDocument = []( const wxFileName& aFile, types::DocumentType aType ) -> bool
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
    };

    auto newProject = [&]( const commands::NewProject& aRequest ) -> HANDLER_RESULT<commands::OpenDocumentResponse>
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
    };

    auto newDocument = [&]( const commands::NewDocument& aRequest ) -> HANDLER_RESULT<commands::OpenDocumentResponse>
    {
        types::DocumentType type = aRequest.type();

        if( type != types::DOCTYPE_SCHEMATIC && type != types::DOCTYPE_PCB )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( "NewDocument can only create schematic or PCB documents" );
            return tl::unexpected( e );
        }

        if( !openProjectPath )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( "NewDocument requires an open project" );
            return tl::unexpected( e );
        }

        wxString ext = type == types::DOCTYPE_SCHEMATIC ? FILEEXT::KiCadSchematicFileExtension
                                                        : FILEEXT::KiCadPcbFileExtension;
        wxFileName file( *openProjectPath );
        file.SetExt( ext );

        if( !aRequest.path().empty() )
        {
            wxFileName requested( wxString::FromUTF8( aRequest.path() ) );

            if( requested.GetExt().IsEmpty() )
                requested.SetExt( ext );

            if( !requested.IsAbsolute() )
                requested.MakeAbsolute( openProjectPath->GetPath() );

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
    };

    auto getProjectInfo = [&]( const commands::GetProjectInfo& ) -> HANDLER_RESULT<commands::ProjectInfoResponse>
    {
        if( !openProjectPath )
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
        response.set_kicad_pro_path( openProjectPath->GetFullPath().ToUTF8() );

        auto isOpen = [&]( types::DocumentType aType, const wxString& aFullName )
        {
            return std::ranges::any_of( openDocuments,
                                        [&]( const OPEN_DOCUMENT& d )
                                        {
                                            return d.type == aType && ( aType == types::DOCTYPE_PROJECT
                                                                        || d.fileName == aFullName );
                                        } );
        };

        wxDir dir( openProjectPath->GetPath() );

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
            wxFileName fn( openProjectPath->GetPath(), entry );
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
                if( fn.GetFullName() != openProjectPath->GetFullName() )
                    continue;

                file.set_kind( commands::PFT_PROJECT );
                file.set_type( types::DOCTYPE_PROJECT );
                file.set_is_open( true );
            }
            else if( ext == FILEEXT::KiCadSchematicFileExtension )
            {
                file.set_kind( commands::PFT_SCHEMATIC );
                file.set_is_root( fn.GetName() == openProjectPath->GetName() );

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

                if( fn.GetName() == openProjectPath->GetName() )
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
    };

    commonHandler.SetOpenDocumentHandler( openDocument );
    commonHandler.SetCloseDocumentHandler( closeDocument );
    commonHandler.SetCloseAllDocumentsHandler( closeAllDocuments );
    commonHandler.SetNewProjectHandler( newProject );
    commonHandler.SetNewDocumentHandler( newDocument );
    commonHandler.SetGetProjectInfoHandler( getProjectInfo );

    // GetAppSettings reads an editor's settings file, which its kiface registers when it loads
    commonHandler.SetEnsureAppSettingsHandler(
            [&]( commands::AppType aApp, wxString* aError ) -> bool
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
                    return aKiway.KiFACE( face ) != nullptr;
                }
                catch( const IO_ERROR& ioe )
                {
                    if( aError )
                        *aError = ioe.What();

                    return false;
                }
            } );

    server->RegisterHandler( &commonHandler );
    server->Start();

    if( !server->Running() )
    {
        wxFprintf( stderr, _( "Failed to start API server\n" ) );
        return EXIT_CODES::ERR_UNKNOWN;
    }

    wxString preloadPath = wxString::FromUTF8( m_argParser.get<std::string>( ARG_PATH ) );

    if( !preloadPath.IsEmpty() )
    {
        using namespace kiapi::common;

        wxFileName preloadFile( preloadPath );
        types::DocumentType preloadType = types::DOCTYPE_PROJECT;

        if( preloadFile.GetExt() == FILEEXT::KiCadSchematicFileExtension )
            preloadType = types::DOCTYPE_SCHEMATIC;
        else if( preloadFile.GetExt() == FILEEXT::KiCadPcbFileExtension )
            preloadType = types::DOCTYPE_PCB;

        commands::OpenDocument request;
        request.set_type( preloadType );
        request.set_path( preloadPath.ToStdString() );

        auto preloadResult = openDocument( request );

        if( !preloadResult )
        {
            wxFprintf( stderr, "%s\n", preloadResult.error().error_message() );
            server->DeregisterHandler( &commonHandler );
            return EXIT_CODES::ERR_ARGS;
        }
    }

    server->SetReadyToReply( true );

    wxString listenPath = wxString::FromUTF8( server->SocketPath() );
    wxString eventsPath = wxString::FromUTF8( server->EventsSocketPath() );
    wxFprintf( stdout, "KiCad API server listening at %s\n", listenPath );

    if( !eventsPath.IsEmpty() )
        wxFprintf( stdout, "KiCad API events published at %s\n", eventsPath );
    else
        wxFprintf( stdout, "KiCad API events not published\n" );

    fflush( stdout );

    auto oldSigInt = std::signal( SIGINT, apiServerSignalHandler );
#ifdef SIGTERM
    auto oldSigTerm = std::signal( SIGTERM, apiServerSignalHandler );
#endif

    g_apiServerExitRequested.store( false );

    // There is no wx event loop in kicad-cli, so requests queued by the server thread are pumped
    // by hand.  Block until one arrives instead of polling: the timeout only bounds how long a
    // SIGINT/SIGTERM (which merely sets a flag) waits to be noticed.
    while( !g_apiServerExitRequested.load() )
    {
        server->WaitForRequest( std::chrono::milliseconds( 100 ) );
        wxTheApp->ProcessPendingEvents();
    }

    std::signal( SIGINT, oldSigInt );
#ifdef SIGTERM
    std::signal( SIGTERM, oldSigTerm );
#endif

    wxFprintf( stdout, "Shutting down\n" );

    commands::CloseAllDocuments closeAllReq;
    closeAllDocuments( closeAllReq );
    server->DeregisterHandler( &commonHandler );

    return EXIT_CODES::OK;
}
