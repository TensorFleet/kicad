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

#include <api/api_undo_sch.h>
#include <api/api_undo_stack.h>
#include <sch_commit.h>
#include <sch_field.h>
#include <sch_group.h>
#include <sch_screen.h>
#include <sch_sheet.h>
#include <sch_sheet_path.h>
#include <sch_sheet_pin.h>
#include <sch_symbol.h>
#include <schematic.h>
#include <undo_redo_container.h>


void RestoreSchematicUndoList( SCHEMATIC& aSchematic, PICKED_ITEMS_LIST& aList, TOOL_MANAGER* aToolManager )
{
    std::vector<SCH_ITEM*> added;
    std::vector<SCH_ITEM*> removed;
    std::vector<SCH_ITEM*> changed;
    bool                   dirtyConnectivity = false;
    bool                   refreshHierarchy = false;
    SCH_CLEANUP_FLAGS      connectivityCleanUp = NO_CLEANUP;
    SCH_SHEET_LIST         sheets = aSchematic.Hierarchy();

    auto propagateConnectivityDamage =
            [&]( SCH_ITEM* aItem, const SCH_SHEET_PATH& aSheet )
            {
                if( aItem->IsConnectable() )
                {
                    aItem->SetConnectivityDirty();

                    if( aItem->Type() == SCH_SYMBOL_T )
                    {
                        for( SCH_PIN* pin : static_cast<SCH_SYMBOL*>( aItem )->GetPins( &aSheet ) )
                            pin->SetConnectivityDirty();
                    }
                    else if( aItem->Type() == SCH_SHEET_T )
                    {
                        for( SCH_SHEET_PIN* pin : static_cast<SCH_SHEET*>( aItem )->GetPins() )
                            pin->SetConnectivityDirty();
                    }

                    dirtyConnectivity = true;

                    if( connectivityCleanUp == NO_CLEANUP )
                        connectivityCleanUp = LOCAL_CLEANUP;

                    if( aItem->Type() == SCH_SHEET_T )
                        connectivityCleanUp = GLOBAL_CLEANUP;
                }
                else if( aItem->Type() == SCH_RULE_AREA_T )
                {
                    dirtyConnectivity = true;
                }
            };

    // Reverse order, so that an item changed and then deleted by one command comes back right
    for( int ii = (int) aList.GetCount() - 1; ii >= 0; ii-- )
    {
        UNDO_REDO      status = aList.GetPickedItemStatus( ii );
        EDA_ITEM*      eda_item = aList.GetPickedItem( ii );
        SCH_SCREEN*    screen = dynamic_cast<SCH_SCREEN*>( aList.GetScreenForItem( ii ) );
        SCH_ITEM*      schItem = dynamic_cast<SCH_ITEM*>( eda_item );
        SCH_SHEET_PATH undoSheet = sheets.FindSheetForScreen( screen );

        if( !schItem || !screen )
            continue;

        eda_item->SetFlags( aList.GetPickerFlags( ii ) );
        eda_item->ClearEditFlags();
        eda_item->ClearTempFlags();

        if( status == UNDO_REDO::NEWITEM )
        {
            propagateConnectivityDamage( schItem, undoSheet );

            if( schItem->Type() == SCH_SHEET_T )
                refreshHierarchy = true;

            if( schItem->Type() != SCH_TABLECELL_T )
                screen->Remove( schItem );

            aList.SetPickedItemStatus( UNDO_REDO::DELETED, ii );
            removed.push_back( schItem );
        }
        else if( status == UNDO_REDO::DELETED )
        {
            if( schItem->Type() == SCH_SHEET_T )
                refreshHierarchy = true;

            propagateConnectivityDamage( schItem, undoSheet );

            if( schItem->Type() != SCH_TABLECELL_T )
                screen->Append( schItem );

            aList.SetPickedItemStatus( UNDO_REDO::NEWITEM, ii );
            added.push_back( schItem );
        }
        else if( status == UNDO_REDO::CHANGED )
        {
            SCH_ITEM* itemCopy = dynamic_cast<SCH_ITEM*>( aList.GetPickedItemLink( ii ) );

            if( !itemCopy )
                continue;

            if( schItem->HasConnectivityChanges( itemCopy, &undoSheet ) )
                propagateConnectivityDamage( schItem, undoSheet );

            // The root sheet owns the root screen but is not on it
            const bool onScreen = schItem != &aSchematic.Root() && schItem->Type() != SCH_TABLECELL_T;

            if( onScreen )
                screen->Remove( schItem );

            if( schItem->Type() == SCH_SHEET_T )
            {
                const SCH_SHEET* origSheet = static_cast<const SCH_SHEET*>( schItem );
                const SCH_SHEET* copySheet = static_cast<const SCH_SHEET*>( itemCopy );

                if( origSheet->GetFileName() != copySheet->GetFileName()
                    || origSheet->HasPageNumberChanges( *copySheet ) )
                {
                    refreshHierarchy = true;
                }
            }

            schItem->SwapItemData( itemCopy );
            changed.push_back( schItem );

            // A reference field carries the placement's reference
            if( schItem->Type() == SCH_FIELD_T && schItem->GetParent() && schItem->GetParent()->Type() == SCH_SYMBOL_T )
            {
                SCH_FIELD*  field = static_cast<SCH_FIELD*>( schItem );
                SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( schItem->GetParent() );

                if( field->GetId() == FIELD_T::REFERENCE )
                    symbol->SetRef( &undoSheet, field->GetText() );

                changed.push_back( symbol );
            }

            if( schItem->Type() == SCH_SYMBOL_T )
                static_cast<SCH_SYMBOL*>( schItem )->UpdatePins();

            if( onScreen )
                screen->Append( schItem );
        }
        else
        {
            // Page settings and repeat-item entries are recorded by the editor frame only
        }
    }

    // Group membership pointers were swapped with the rest of the item data; re-resolve them
    for( int ii = 0; ii < (int) aList.GetCount(); ++ii )
    {
        ITEM_PICKER& wrapper = aList.GetItemWrapper( ii );

        if( wrapper.GetStatus() == UNDO_REDO::DELETED )
            continue;

        SCH_ITEM* parentGroup = aSchematic.ResolveItem( wrapper.GetGroupId(), nullptr, true );
        wrapper.GetItem()->SetParentGroup( dynamic_cast<SCH_GROUP*>( parentGroup ) );
    }

    for( int ii = 0; ii < (int) aList.GetCount(); ++ii )
    {
        ITEM_PICKER& wrapper = aList.GetItemWrapper( ii );

        if( wrapper.GetStatus() == UNDO_REDO::DELETED )
            continue;

        if( EDA_GROUP* group = dynamic_cast<SCH_GROUP*>( wrapper.GetItem() ) )
        {
            group->GetItems().clear();

            for( const KIID& member : wrapper.GetGroupMembers() )
            {
                if( SCH_ITEM* memberItem = aSchematic.ResolveItem( member, nullptr, true ) )
                    group->AddItem( memberItem );
            }
        }

        if( EDA_ITEM* item = wrapper.GetLink() )
            wrapper.SetLink( item );
    }

    if( !added.empty() )
        aSchematic.OnItemsAdded( added );

    if( !removed.empty() )
        aSchematic.OnItemsRemoved( removed );

    if( !changed.empty() )
        aSchematic.OnItemsChanged( changed );

    if( refreshHierarchy )
        aSchematic.RefreshHierarchy();

    if( dirtyConnectivity )
    {
        SCH_COMMIT localCommit( aToolManager );
        aSchematic.RecalculateConnections( &localCommit, connectivityCleanUp, aToolManager );

        if( connectivityCleanUp == GLOBAL_CLEANUP )
            aSchematic.SetSheetNumberAndCount();
    }
}


std::unique_ptr<API_UNDO_STACK> MakeSchematicUndoStack( SCHEMATIC* aSchematic, TOOL_MANAGER* aToolManager )
{
    return std::make_unique<API_UNDO_STACK>(
            [aSchematic, aToolManager]( PICKED_ITEMS_LIST& aList )
            {
                RestoreSchematicUndoList( *aSchematic, aList, aToolManager );
            },
            []( PICKED_ITEMS_LIST& aList )
            {
                aList.ClearListAndDeleteItems(
                        []( EDA_ITEM* aItem )
                        {
                            delete aItem;
                        } );
            } );
}
