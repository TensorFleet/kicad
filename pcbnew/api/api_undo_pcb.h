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

#ifndef KICAD_API_UNDO_PCB_H
#define KICAD_API_UNDO_PCB_H

#include <memory>

class API_UNDO_STACK;
class BOARD;
class PICKED_ITEMS_LIST;

/**
 * Put a board back into the state an undo list records, without an editor frame; the list
 * becomes the redo (or undo) list of the same command.  The item-level counterpart of
 * PCB_BASE_EDIT_FRAME::PutDataInPreviousState for headless API sessions.  Since 11.0
 */
void RestoreBoardUndoList( BOARD* aBoard, PICKED_ITEMS_LIST& aList );

/// An undo stack for a headless board or footprint document.  Since 11.0
std::unique_ptr<API_UNDO_STACK> MakeBoardUndoStack( BOARD* aBoard );

#endif // KICAD_API_UNDO_PCB_H
