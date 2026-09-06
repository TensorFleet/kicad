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
#include <algorithm>
#include <deque>
#include <eda_item.h>
#include <map>
#include <set>
#include <vector>

using namespace kiapi::common;
using kiapi::common::types::DocumentSpecifier;
using kiapi::common::types::ItemRequestStatus;
using kiapi::common::commands::ItemDeletionStatus;

class EDA_BASE_FRAME;
class TITLE_BLOCK;
class TOOL_MANAGER;

/**
 * Base class for API handlers related to editor frames
 */
class API_HANDLER_EDITOR : public API_HANDLER
{
public:
    API_HANDLER_EDITOR( EDA_BASE_FRAME* aFrame = nullptr );

    /**
     * Publish a DocumentOpened event for the current document.  Editor frames call this after
     * loading a file into a frame whose handler was registered before the file existed.
     */
    void NotifyDocumentOpened();

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

    virtual HANDLER_RESULT<commands::GetDocumentModifiedStateResponse>
    handleGetDocumentModifiedState( const HANDLER_CONTEXT<commands::GetDocumentModifiedState>& aCtx );

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

    HANDLER_RESULT<commands::GetItemCountsResponse> handleGetItemCounts(
            const HANDLER_CONTEXT<commands::GetItemCounts>& aCtx );

    /**
     * @return the number of items of each type in the (validated) document, as GetItems would
     *         return them.  Types with no items may be omitted.
     */
    virtual std::map<KICAD_T, uint32_t> countItems( const DocumentSpecifier& aDocument ) { return {}; }

    /// What changed in one revision step; see changesSince
    struct REVISION_CHANGES
    {
        uint64_t          Revision;    ///< The revision these changes produced
        bool              Complete;    ///< false if the change could not be attributed to items
        std::vector<KIID> Changed;     ///< Created or modified items
        std::vector<KIID> Deleted;
    };

    /**
     * Collect what changed after revision aRevision from the bounded change log.
     * @return std::nullopt if the log does not cover the range or a change in it could not be
     *         attributed to items (the caller must assume everything changed)
     */
    std::optional<REVISION_CHANGES> changesSince( uint64_t aRevision ) const;

    /**
     * Apply GetItems.since_revision and GetItems.page to a list of items about to be packed,
     * and fill the response's total / revision / deleted_ids.
     *
     * @param aItems is the full list of matching items, reduced in place to the window
     * @param aGetItem maps an entry of aItems to its EDA_ITEM (items are matched through their
     *                 ancestors too, so a changed footprint yields its pads)
     */
    template <typename T, typename GETTER>
    void windowItems( const commands::GetItems& aRequest, std::vector<T>& aItems,
                      commands::GetItemsResponse& aResponse, GETTER aGetItem )
    {
        if( aRequest.has_since_revision() )
        {
            if( std::optional<REVISION_CHANGES> changes = changesSince( aRequest.since_revision() ) )
            {
                std::set<KIID> changed( changes->Changed.begin(), changes->Changed.end() );

                std::erase_if( aItems,
                               [&]( const T& aEntry )
                               {
                                   for( const EDA_ITEM* item = aGetItem( aEntry ); item; item = item->GetParent() )
                                   {
                                       if( changed.contains( item->m_Uuid ) )
                                           return false;
                                   }

                                   return true;
                               } );

                for( const KIID& id : changes->Deleted )
                    aResponse.add_deleted_ids()->set_value( id.AsStdString() );
            }
        }

        aResponse.set_total( static_cast<uint32_t>( aItems.size() ) );
        aResponse.set_revision( m_revision );

        if( aRequest.has_page() )
        {
            size_t offset = std::min<size_t>( aRequest.page().offset(), aItems.size() );
            size_t limit = aRequest.page().limit() == 0 ? aItems.size() - offset
                                                         : std::min<size_t>( aRequest.page().limit(), aItems.size() - offset );

            aItems.erase( aItems.begin() + offset + limit, aItems.end() );
            aItems.erase( aItems.begin(), aItems.begin() + offset );
        }
    }

    /**
     * Record that the document was changed (or reverted) so that GetDocumentRevision reports a
     * new value, and publish a DocumentChanged event without item details.  Called by
     * pushCurrentCommit and onModified; handlers that change the document without going through
     * either must call it themselves.
     */
    void bumpRevision();

    /**
     * Record that the document was written to disk: advances the revision and publishes a
     * DocumentSaved event.
     * @param aPath is the absolute path (or library id) the document was written to
     */
    void notifyDocumentSaved( const wxString& aPath );

    HANDLER_RESULT<google::protobuf::Empty> handleRefreshEditor(
            const HANDLER_CONTEXT<commands::RefreshEditor>& aCtx );

    HANDLER_RESULT<commands::FocusOnItemResponse> handleFocusOnItem(
            const HANDLER_CONTEXT<commands::FocusOnItem>& aCtx );

    HANDLER_RESULT<commands::RunActionResponse> handleRunAction( const HANDLER_CONTEXT<commands::RunAction>& aCtx );

    HANDLER_RESULT<commands::GetActionsResponse> handleGetActions(
            const HANDLER_CONTEXT<commands::GetActions>& aCtx );

    /// @return the tool manager actions are run on, or nullptr if the editor has none
    virtual TOOL_MANAGER* editorToolManager() const { return nullptr; }

    /**
     * @return the action name prefixes this editor answers RunAction / GetActions for, e.g.
     *         "pcbnew." and "common." for the board editor.  Actions with other prefixes are
     *         passed on to the next handler.
     */
    virtual std::vector<std::string> actionPrefixes() const { return {}; }

    /**
     * @return the names of the actions that can run without an editor window.  Everything else
     *         is refused headless (the tools behind them open dialogs or need a canvas).
     */
    virtual const std::set<std::string>& headlessActions() const;

    /**
     * Register the tools that serve headlessActions() on the headless tool manager, if not done
     * yet.  Editor frames register every tool themselves; a headless context starts with a bare
     * tool manager and adds the non-interactive tools on first use.
     */
    virtual void ensureHeadlessTools() {}

    /// @return true if aAction starts with one of actionPrefixes()
    bool ownsAction( const std::string& aAction ) const;

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
     * Advance the revision and publish a DocumentChanged event.
     * @param aClientName is the API client that made the change, if any
     * @param aMessage is the commit message, if any
     * @param aCommitId is the id of the API commit, if the change came from one
     * @param aCommit is the commit about to be pushed; its staged entries are reported as
     *                created/updated/deleted ids
     */
    void publishDocumentChanged( const std::string& aClientName, const wxString& aMessage,
                                 const KIID* aCommitId = nullptr, const COMMIT* aCommit = nullptr );

    void fillDocumentChanged( events::DocumentChanged& aEvent, const std::string& aClientName,
                              const wxString& aMessage, const KIID* aCommitId, const COMMIT* aCommit ) const;

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

    /**
     * @return the title block of the given (already validated) document, or std::nullopt if the
     *         editor has no title block.  Editors with several title blocks per document (the
     *         schematic editor has one per sheet) resolve it from aDocument.
     */
    virtual std::optional<TITLE_BLOCK*> getTitleBlock( const DocumentSpecifier& aDocument )
    {
        return std::nullopt;
    }

    virtual std::optional<PAGE_INFO> getPageSettings( const DocumentSpecifier& aDocument )
    {
        return std::nullopt;
    }

    virtual bool setPageSettings( const DocumentSpecifier& aDocument, const PAGE_INFO& aPageInfo )
    {
        return false;
    }

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

private:
    /// Advance m_revision and log what changed; every revision step goes through here
    void advanceRevision( bool aComplete, const COMMIT* aCommit );

    /// The most recent revision steps, oldest first; see changesSince
    std::deque<REVISION_CHANGES> m_revisionChanges;

    static constexpr size_t MAX_REVISION_CHANGES = 256;
};

#endif //KICAD_API_HANDLER_EDITOR_H
