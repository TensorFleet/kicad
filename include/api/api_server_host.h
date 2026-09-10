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

#ifndef KICAD_API_SERVER_HOST_H
#define KICAD_API_SERVER_HOST_H

#include <memory>
#include <optional>
#include <vector>

#include <wx/filename.h>
#include <wx/string.h>

#include <kicommon.h>
#include <kiway.h>
#include <lib_id.h>
#include <api/api_handler.h>
#include <api/api_handler_common.h>
#include <api/common/types/base_types.pb.h>

class API_HANDLER_LIBRARY;
class KICAD_API_SERVER;
class PROJECT;


/**
 * The document- and project-lifecycle half of a headless API host.
 *
 * A KICAD_API_SERVER only routes requests to handlers; something has to own the notion of "the
 * open project and its documents" and answer the API_HANDLER_COMMON lifecycle commands
 * (OpenDocument, CloseDocument, CloseAllDocuments, NewProject, NewDocument, GetProjectInfo,
 * and the GetAppSettings kiface check).  In the GUI that is the project manager frame; headless
 * it is this class, which drives the pcbnew and eeschema kifaces through
 * KIWAY::ProcessApiOpenDocument / ProcessApiCloseDocument.
 *
 * Eventually we might support opening multiple projects at once, but for now we support one
 * project at a time, with multiple documents within that project (e.g. up to one schematic, up
 * to one board, and arbitrarily many library files which are not associated with the project).
 *
 * The server and the kiway must outlive the host.  Since 11.0
 */
class API_SERVER_HOST
{
public:
    API_SERVER_HOST( KIWAY& aKiway, KICAD_API_SERVER& aServer );

    ~API_SERVER_HOST();

    /**
     * Install the document lifecycle handlers on the common handler and register that handler
     * with the server.  Call once, before the server starts replying.
     */
    void Install();

    /**
     * Open one document or project by path, the way the kicad-cli positional argument does: the
     * document type is taken from the file extension, and anything that is not a schematic or a
     * board is opened as a project.
     *
     * @param aPath is the path to open; an empty path does nothing and succeeds
     * @param aError is filled with the failure message when this returns false
     * @return true if aPath was empty or opened successfully
     */
    bool Preload( const wxString& aPath, wxString* aError );

    /**
     * Close every open document and the open project, and deregister the common handler.  Called
     * by the destructor; safe to call more than once.
     */
    void Shutdown();

    /// @return the handler serving the common (non-editor) commands
    API_HANDLER_COMMON& CommonHandler() { return m_commonHandler; }

private:
    struct OPEN_DOCUMENT
    {
        kiapi::common::types::DocumentType type;
        wxString                           fileName;
        LIB_ID                             libId;
    };

    static KIWAY::FACE_T faceForDocument( kiapi::common::types::DocumentType aType );

    /// How the kiface addresses a document: library items by LIB_ID, files by name
    static KIFACE::DOCUMENT_SPEC closeSpec( const OPEN_DOCUMENT& aDoc, bool aExact );

    /// Minimal files, identical to the stubs the project manager writes for a new project
    static bool writeStubDocument( const wxFileName& aFile,
                                   kiapi::common::types::DocumentType aType );

    /// The project has no API handler of its own, so its open/close events are published here
    void publishProjectEvent( const PROJECT& aProject, bool aOpened );

    /// Tell the pcbnew and eeschema kifaces that the project opened or closed, and (re)build the
    /// design block library handler, which has no kiface of its own
    void notifyProjectFaces( const wxFileName& aProjectPath, bool aOpened );

    HANDLER_RESULT<commands::OpenDocumentResponse> openDocument( const commands::OpenDocument& aRequest );

    HANDLER_RESULT<Empty> closeDocument( const commands::CloseDocument& aRequest );

    HANDLER_RESULT<Empty> closeAllDocuments( const commands::CloseAllDocuments& aRequest );

    HANDLER_RESULT<commands::OpenDocumentResponse> newProject( const commands::NewProject& aRequest );

    HANDLER_RESULT<commands::OpenDocumentResponse> newDocument( const commands::NewDocument& aRequest );

    HANDLER_RESULT<commands::ProjectInfoResponse> getProjectInfo( const commands::GetProjectInfo& aRequest );

    /// GetAppSettings reads an editor's settings file, which its kiface registers when it loads
    bool ensureAppSettings( commands::AppType aApp, wxString* aError );

    KIWAY&            m_kiway;
    KICAD_API_SERVER& m_server;

    API_HANDLER_COMMON m_commonHandler;

    /// The library commands are served by the pcbnew and eeschema kifaces for the open project;
    /// the design block tables have no kiface of their own and are served (tables only) here.
    std::unique_ptr<API_HANDLER_LIBRARY> m_designBlockLibraries;

    std::optional<wxFileName> m_openProjectPath;

    std::vector<OPEN_DOCUMENT> m_openDocuments;

    bool m_installed = false;
};

#endif // KICAD_API_SERVER_HOST_H
