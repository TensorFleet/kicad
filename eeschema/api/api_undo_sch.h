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

#ifndef KICAD_API_UNDO_SCH_H
#define KICAD_API_UNDO_SCH_H

#include <memory>

class API_UNDO_STACK;
class PICKED_ITEMS_LIST;
class SCHEMATIC;
class TOOL_MANAGER;

/**
 * Put a schematic back into the state an undo list records, without an editor frame; the list
 * becomes the redo (or undo) list of the same command.  The counterpart of
 * SCH_EDIT_FRAME::PutDataInPreviousState for headless API sessions.  Since 11.0
 */
void RestoreSchematicUndoList( SCHEMATIC& aSchematic, PICKED_ITEMS_LIST& aList, TOOL_MANAGER* aToolManager );

/// An undo stack for a headless schematic.  Since 11.0
std::unique_ptr<API_UNDO_STACK> MakeSchematicUndoStack( SCHEMATIC* aSchematic, TOOL_MANAGER* aToolManager );

#endif // KICAD_API_UNDO_SCH_H
