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

#ifndef KICAD_API_HANDLER_LIBRARY_H
#define KICAD_API_HANDLER_LIBRARY_H

#include <google/protobuf/empty.pb.h>

#include <api/api_handler.h>
#include <api/common/commands/library_commands.pb.h>
#include <kicommon.h>
#include <libraries/library_table.h>

class LIBRARY_MANAGER_ADAPTER;
class PROJECT;

/**
 * Serves the library commands (library_commands.proto) for one library table type.
 *
 * The table commands (GetLibraryTables, CreateLibrary, AddLibraryTableRow, RemoveLibraryTableRow)
 * are implemented here on top of LIBRARY_MANAGER; the item commands are implemented by the
 * per-type subclasses in pcbnew and eeschema.  Requests for another library type are answered
 * AS_UNHANDLED so that the server passes them to the handler for that type.
 *
 * Since 11.0
 */
// Built into the static `common` library (like API_HANDLER_COMMON and API_HANDLER_EDITOR),
// not the kicommon DLL, so it must not carry KICOMMON_API: on MSVC that would make every
// user expect a DLL import that nothing exports (LNK2019 in _pcbnew.dll / _eeschema.dll).
class API_HANDLER_LIBRARY : public API_HANDLER
{
public:
    /**
     * @param aType is the library table type this handler serves
     * @param aAdapter is the adapter for aType (may be nullptr; CreateLibrary is then refused)
     * @param aProject is the project whose tables are served
     */
    API_HANDLER_LIBRARY( LIBRARY_TABLE_TYPE aType, LIBRARY_MANAGER_ADAPTER* aAdapter, PROJECT* aProject );

    ~API_HANDLER_LIBRARY() override {}

    static kiapi::common::commands::LibraryType ToProtoType( LIBRARY_TABLE_TYPE aType );

    static LIBRARY_TABLE_TYPE FromProtoType( kiapi::common::commands::LibraryType aType );

    /// Pack one table row, including its resolved URI
    void PackRow( kiapi::common::commands::LibraryTableRow& aOut, const LIBRARY_TABLE_ROW& aRow ) const;

protected:
    /// @return an AS_UNHANDLED status if aType is not the type this handler serves
    std::optional<ApiResponseStatus> checkType( kiapi::common::commands::LibraryType aType ) const;

    /// @return the row named aNickname in the tables of the given scope, valid or not, or nullptr
    const LIBRARY_TABLE_ROW* findRow( const wxString& aNickname, LIBRARY_TABLE_SCOPE aScope ) const;

    /// @return the (already loaded) table of the given scope, or an error status
    HANDLER_RESULT<LIBRARY_TABLE*> table( kiapi::common::commands::LibraryTableScope aScope ) const;

    /// Save aTable and reload the manager's view of it; @return an error status on failure
    std::optional<ApiResponseStatus> saveTable( LIBRARY_TABLE* aTable, const std::string& aClientName );

    /// Publish a ProjectChanged{PCK_LIBRARY_TABLES} event, if the server is publishing
    void notifyTablesChanged( const std::string& aClientName );

    static ApiResponseStatus badRequest( const std::string& aMessage );

    static ApiResponseStatus unimplemented( const std::string& aMessage );

    HANDLER_RESULT<kiapi::common::commands::GetLibraryTablesResponse> handleGetLibraryTables(
            const HANDLER_CONTEXT<kiapi::common::commands::GetLibraryTables>& aCtx );

    HANDLER_RESULT<kiapi::common::commands::LibraryTableRow> handleCreateLibrary(
            const HANDLER_CONTEXT<kiapi::common::commands::CreateLibrary>& aCtx );

    HANDLER_RESULT<kiapi::common::commands::LibraryTableRow> handleAddLibraryTableRow(
            const HANDLER_CONTEXT<kiapi::common::commands::AddLibraryTableRow>& aCtx );

    HANDLER_RESULT<google::protobuf::Empty> handleRemoveLibraryTableRow(
            const HANDLER_CONTEXT<kiapi::common::commands::RemoveLibraryTableRow>& aCtx );

    // Item commands: the base implementation answers AS_UNIMPLEMENTED for the handler's own
    // type (and AS_UNHANDLED for other types)

    virtual HANDLER_RESULT<kiapi::common::commands::ListLibraryEntriesResponse> handleListLibraryEntries(
            const HANDLER_CONTEXT<kiapi::common::commands::ListLibraryEntries>& aCtx );

    virtual HANDLER_RESULT<kiapi::common::commands::GetLibraryItemResponse> handleGetLibraryItem(
            const HANDLER_CONTEXT<kiapi::common::commands::GetLibraryItem>& aCtx );

    virtual HANDLER_RESULT<kiapi::common::commands::SaveLibraryItemResponse> handleSaveLibraryItem(
            const HANDLER_CONTEXT<kiapi::common::commands::SaveLibraryItem>& aCtx );

    virtual HANDLER_RESULT<google::protobuf::Empty> handleDeleteLibraryItem(
            const HANDLER_CONTEXT<kiapi::common::commands::DeleteLibraryItem>& aCtx );

    /**
     * @return the default plugin type name for a new library of this handler's type (the table's
     *         "type" column), for example "KiCad"
     */
    virtual wxString defaultPluginType() const { return wxS( "KiCad" ); }

    /**
     * @return the file extension (or directory extension) of a new library of this type in the
     *         default plugin format, without the dot
     */
    virtual wxString defaultLibraryExtension() const { return wxEmptyString; }

    LIBRARY_TABLE_TYPE       m_type;
    LIBRARY_MANAGER_ADAPTER* m_adapter;
    PROJECT*                 m_project;
};

#endif // KICAD_API_HANDLER_LIBRARY_H
