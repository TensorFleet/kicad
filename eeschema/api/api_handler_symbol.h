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

#ifndef KICAD_API_HANDLER_SYMBOL_H
#define KICAD_API_HANDLER_SYMBOL_H

#include <memory>

#include <api/api_handler_editor.h>
#include <api/api_undo_stack.h>
#include <api/symbol_context.h>
#include <api/common/commands/project_commands.pb.h>

class LIB_SYMBOL;
class SCH_ITEM;


/**
 * API handler for a library symbol document (DOCTYPE_SYMBOL).  The items of the document are the
 * symbol's children: pins, shapes, text, text boxes and fields.  Serves the generic editor
 * commands (commits, Create/Update/DeleteItems, HitTest, GetDocumentRevision) plus
 * GetOpenDocuments, GetItems, SaveDocument and SaveCopyOfDocument.
 */
class API_HANDLER_SYMBOL : public API_HANDLER_EDITOR
{
public:
    API_HANDLER_SYMBOL( std::shared_ptr<SYMBOL_CONTEXT> aContext );

    std::optional<DocumentSpecifier> Document() const override;

protected:
    std::unique_ptr<COMMIT> createCommit() override;

    kiapi::common::types::DocumentType thisDocumentType() const override
    {
        return kiapi::common::types::DOCTYPE_SYMBOL;
    }

    tl::expected<bool, ApiResponseStatus> validateDocumentInternal( const DocumentSpecifier& aDocument ) const override;

    const EDA_IU_SCALE& getIuScale() const override { return schIUScale; }

    HANDLER_RESULT<ItemRequestStatus> handleCreateUpdateItemsInternal( bool aCreate,
            const std::string& aClientName,
            const types::ItemHeader& aHeader,
            const google::protobuf::RepeatedPtrField<google::protobuf::Any>& aItems,
            std::function<void( commands::ItemStatus, google::protobuf::Any )> aItemHandler ) override;

    void deleteItemsInternal( std::map<KIID, ItemDeletionStatus>& aItemsToDelete,
                              const std::string& aClientName ) override;

    std::optional<EDA_ITEM*> getItemFromDocument( const DocumentSpecifier& aDocument, const KIID& aId ) override;

private:
    HANDLER_RESULT<commands::GetOpenDocumentsResponse> handleGetOpenDocuments(
            const HANDLER_CONTEXT<commands::GetOpenDocuments>& aCtx );

    HANDLER_RESULT<google::protobuf::Empty> handleSaveDocument(
            const HANDLER_CONTEXT<commands::SaveDocument>& aCtx );

    HANDLER_RESULT<google::protobuf::Empty> handleSaveCopyOfDocument(
            const HANDLER_CONTEXT<commands::SaveCopyOfDocument>& aCtx );

    HANDLER_RESULT<commands::GetItemsResponse> handleGetItems( const HANDLER_CONTEXT<commands::GetItems>& aCtx );

    std::map<KICAD_T, uint32_t> countItems( const DocumentSpecifier& aDocument ) override;

    HANDLER_RESULT<commands::GetItemsResponse> handleGetItemsById(
            const HANDLER_CONTEXT<commands::GetItemsById>& aCtx );

    LIB_SYMBOL* symbol() const { return m_context->GetSymbol(); }

    API_UNDO_STACK* apiUndoStack() const override { return m_undoStack.get(); }

    /// Put the symbol back into the state an undo list records; see API_UNDO_STACK::RESTORE_FN
    void restoreUndoList( PICKED_ITEMS_LIST& aList );

    /// @return the child of the symbol with the given id, or nullptr
    SCH_ITEM* findItem( const KIID& aId ) const;

    std::shared_ptr<SYMBOL_CONTEXT> m_context;

    /// The document's undo history (headless only; symbol documents have no editor window here)
    std::unique_ptr<API_UNDO_STACK> m_undoStack;
};

#endif // KICAD_API_HANDLER_SYMBOL_H
