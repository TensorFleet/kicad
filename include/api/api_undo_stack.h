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

#ifndef KICAD_API_UNDO_STACK_H
#define KICAD_API_UNDO_STACK_H

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <kiid.h>
#include <undo_redo_container.h>
#include <wx/string.h>


/**
 * The undo and redo stacks of a headless API document.  Since 11.0
 *
 * Commits pushed without an editor frame hand their undo lists to this sink (through the tool
 * manager); Undo() and Redo() apply them back with the editor-specific restore function and move
 * them to the opposite stack, exactly as the frames do with PutDataInPreviousState.
 */
class API_UNDO_STACK : public UNDO_REDO_SINK
{
public:
    struct ENTRY
    {
        wxString            Description;
        std::string         ClientName;  ///< The API client behind the command, if any
        std::optional<KIID> CommitId;    ///< The API commit behind the command, if any
        size_t              ItemCount = 0;
    };

    /// What an undo or redo step did to the document
    struct RESULT
    {
        ENTRY             Entry;
        std::vector<KIID> Created;
        std::vector<KIID> Updated;
        std::vector<KIID> Deleted;
    };

    /**
     * Applies the previous state recorded in a list to the document and turns the list into its
     * counterpart (an undo list becomes the redo list of the same command), as the frames'
     * PutDataInPreviousState does.
     */
    using RESTORE_FN = std::function<void( PICKED_ITEMS_LIST& aList )>;

    /// Frees what a list owns (its links and transient items)
    using DELETER_FN = std::function<void( PICKED_ITEMS_LIST& aList )>;

    API_UNDO_STACK( RESTORE_FN aRestore, DELETER_FN aDeleter, size_t aMaxLevels = 100 );

    ~API_UNDO_STACK() override;

    void SaveCopyInUndoList( const PICKED_ITEMS_LIST& aItemsList, bool aAppend ) override;

    /**
     * Attribute the next command that arrives through SaveCopyInUndoList to an API client (and
     * commit).  Consumed by that call.
     */
    void SetNextAttribution( const std::string& aClientName, const std::optional<KIID>& aCommitId );

    /// Oldest first
    std::vector<ENTRY> UndoEntries() const;

    /// Oldest first
    std::vector<ENTRY> RedoEntries() const;

    size_t UndoCount() const { return m_undo.m_CommandsList.size(); }

    size_t RedoCount() const { return m_redo.m_CommandsList.size(); }

    /// @return what changed, or std::nullopt when there was nothing to undo
    std::optional<RESULT> Undo();

    std::optional<RESULT> Redo();

    void Clear();

private:
    std::optional<RESULT> apply( UNDO_REDO_CONTAINER& aFrom, UNDO_REDO_CONTAINER& aTo );

    void clearList( UNDO_REDO_CONTAINER& aList );

    ENTRY entryFor( const PICKED_ITEMS_LIST* aList ) const;

    RESTORE_FN m_restore;
    DELETER_FN m_deleter;
    size_t     m_maxLevels;

    UNDO_REDO_CONTAINER m_undo;
    UNDO_REDO_CONTAINER m_redo;

    /// Attribution per command; keyed by the list, which keeps its identity across the stacks
    std::map<const PICKED_ITEMS_LIST*, ENTRY> m_entries;

    std::optional<ENTRY> m_pending;
};

#endif // KICAD_API_UNDO_STACK_H
