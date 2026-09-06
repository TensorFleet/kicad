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

#ifndef KICAD_API_HANDLER_EDITOR_H
#define KICAD_API_HANDLER_EDITOR_H

#include <api/api_handler.h>
#include <api/common/commands/cross_probe_commands.pb.h>
#include <api/common/commands/editor_commands.pb.h>
#include <base_units.h>
#include <commit.h>
#include <cstdint>
#include <google/protobuf/empty.pb.h>
#include <kiid.h>
#include <page_info.h>

using namespace kiapi::common;
using kiapi::common::types::DocumentSpecifier;
using kiapi::common::types::ItemRequestStatus;
using kiapi::common::commands::ItemDeletionStatus;

class EDA_BASE_FRAME;
class TITLE_BLOCK;

/**
 * Base class for API handlers related to editor frames
 */
class API_HANDLER_EDITOR : public API_HANDLER
{
public:
    API_HANDLER_EDITOR( EDA_BASE_FRAME* aFrame = nullptr );

protected:
    /// If the header is valid, returns the item container
    HANDLER_RESULT<std::optional<KIID>> validateItemHeaderDocument(
            const kiapi::common::types::ItemHeader& aHeader );

    HANDLER_RESULT<bool> validateDocument( const DocumentSpecifier& aDocument );

    /**
     * Checks if the editor can accept commands
     * @return an error status if busy, std::nullopt if not busy
     */
    virtual std::optional<ApiResponseStatus> checkForBusy();

    HANDLER_RESULT<commands::BeginCommitResponse> handleBeginCommit(
        const HANDLER_CONTEXT<commands::BeginCommit>& aCtx );

    HANDLER_RESULT<commands::EndCommitResponse> handleEndCommit(
        const HANDLER_CONTEXT<commands::EndCommit>& aCtx );

    COMMIT* getCurrentCommit( const std::string& aClientName );

    virtual void pushCurrentCommit( const std::string& aClientName, const wxString& aMessage );

    HANDLER_RESULT<commands::CreateItemsResponse> handleCreateItems(
        const HANDLER_CONTEXT<commands::CreateItems>& aCtx );

    HANDLER_RESULT<commands::UpdateItemsResponse> handleUpdateItems(
        const HANDLER_CONTEXT<commands::UpdateItems>& aCtx );

    HANDLER_RESULT<commands::DeleteItemsResponse> handleDeleteItems(
        const HANDLER_CONTEXT<commands::DeleteItems>& aCtx );

    HANDLER_RESULT<commands::HitTestResponse> handleHitTest(
        const HANDLER_CONTEXT<commands::HitTest>& aCtx );

    HANDLER_RESULT<types::TitleBlockInfo> handleGetTitleBlockInfo(
            const HANDLER_CONTEXT<commands::GetTitleBlockInfo>& aCtx );

    HANDLER_RESULT<google::protobuf::Empty> handleSetTitleBlockInfo(
            const HANDLER_CONTEXT<commands::SetTitleBlockInfo>& aCtx );

    HANDLER_RESULT<types::PageSettings> handleGetPageSettings(
            const HANDLER_CONTEXT<commands::GetPageSettings>& aCtx );

    HANDLER_RESULT<types::PageSettings> handleSetPageSettings(
            const HANDLER_CONTEXT<commands::SetPageSettings>& aCtx );

    HANDLER_RESULT<commands::DocumentRevisionResponse> handleGetDocumentRevision(
            const HANDLER_CONTEXT<commands::GetDocumentRevision>& aCtx );

    /**
     * Record that the document was changed (or saved/reverted) so that GetDocumentRevision
     * reports a new value.  Called by pushCurrentCommit and onModified; handlers that change
     * the document without going through either must call it themselves.
     */
    void bumpRevision() { ++m_revision; }

    HANDLER_RESULT<google::protobuf::Empty> handleRefreshEditor(
            const HANDLER_CONTEXT<commands::RefreshEditor>& aCtx );

    HANDLER_RESULT<commands::FocusOnItemResponse> handleFocusOnItem(
            const HANDLER_CONTEXT<commands::FocusOnItem>& aCtx );

    /**
     * Focus the editor window on the item described by aSpec.  Only called when a frame is
     * attached; the default does nothing and reports CPS_OK.
     */
    virtual void focusOnItem( const commands::SelectionSpec& aSpec, commands::FocusOnItemResponse& aResponse )
    {
        aResponse.set_status( commands::CrossProbeStatus::CPS_OK );
    }

    /// @return the editor frame type that serves thisDocumentType()
    types::FrameType thisFrameType() const;

    /**
     * Override this to create an appropriate COMMIT subclass for the frame in question
     * @return a new COMMIT, bound to the editor frame
     */
    virtual std::unique_ptr<COMMIT> createCommit() = 0;

    /**
     * Override this to specify which document type this editor handles
     */
    virtual types::DocumentType thisDocumentType() const = 0;

    /**
     * @return true if the given document is valid for this editor and is currently open
     */
    virtual tl::expected<bool, ApiResponseStatus> validateDocumentInternal( const DocumentSpecifier& aDocument ) const = 0;

    /**
     * Returns the internal-unit scale that the concrete editor uses. API wire coordinates
     * are always in nanometers, so this scale drives conversion to the editor's native IU.
     * Defaults to pcbIUScale; schematic-like editors must override.
     */
    virtual const EDA_IU_SCALE& getIuScale() const { return pcbIUScale; }

    virtual HANDLER_RESULT<ItemRequestStatus> handleCreateUpdateItemsInternal( bool aCreate,
        const std::string& aClientName,
        const types::ItemHeader &aHeader,
        const google::protobuf::RepeatedPtrField<google::protobuf::Any>& aItems,
        std::function<void( commands::ItemStatus, google::protobuf::Any )> aItemHandler ) = 0;

    virtual void deleteItemsInternal( std::map<KIID, ItemDeletionStatus>& aItemsToDelete,
                                      const std::string& aClientName ) = 0;

    virtual std::optional<EDA_ITEM*> getItemFromDocument( const DocumentSpecifier& aDocument,
                                                          const KIID& aId ) = 0;

    static std::vector<KICAD_T> parseRequestedItemTypes( const google::protobuf::RepeatedField<int>& aTypes );

    virtual std::optional<TITLE_BLOCK*> getTitleBlock() { return std::nullopt; }

    virtual std::optional<PAGE_INFO> getPageSettings() { return std::nullopt; }

    virtual bool setPageSettings( const PAGE_INFO& aPageInfo ) { return false; }

    virtual wxString getDrawingSheetFileName() { return wxEmptyString; }

    virtual void setDrawingSheetFileName( const wxString& aFileName ) {}

    /**
     * Called after the document was changed outside of a commit.  Overrides must call the base
     * implementation, which advances the document revision.
     */
    virtual void onModified() { bumpRevision(); }

protected:
    std::map<std::string, std::pair<KIID, std::unique_ptr<COMMIT>>> m_commits;

    std::set<std::string> m_activeClients;

    EDA_BASE_FRAME* m_frame;

    /// Document revision counter reported by GetDocumentRevision; see bumpRevision
    uint64_t m_revision;
};

#endif //KICAD_API_HANDLER_EDITOR_H
