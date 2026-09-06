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

#include <api/api_undo_pcb.h>
#include <api/api_undo_stack.h>
#include <board.h>
#include <board_design_settings.h>
#include <component_classes/component_class_manager.h>
#include <footprint.h>
#include <pcb_group.h>
#include <undo_redo_container.h>
#include <zone.h>


void RestoreBoardUndoList( BOARD* aBoard, PICKED_ITEMS_LIST& aList )
{
    bool                     rebuildConnectivity = false;
    std::vector<BOARD_ITEM*> added;
    std::vector<BOARD_ITEM*> removed;
    std::vector<BOARD_ITEM*> changed;

    aBoard->IncrementTimeStamp();   // clear caches

    // Reverse order, so that an item changed and then deleted by one command comes back right
    for( int ii = (int) aList.GetCount() - 1; ii >= 0; ii-- )
    {
        EDA_ITEM* eda_item = aList.GetPickedItem( (unsigned) ii );
        UNDO_REDO status = aList.GetPickedItemStatus( ii );

        // Origin markers and page settings proxies are never on the board
        if( status != UNDO_REDO::DELETED && status != UNDO_REDO::DRILLORIGIN && status != UNDO_REDO::GRIDORIGIN
            && status != UNDO_REDO::PAGESETTINGS )
        {
            if( !aBoard->ResolveItem( eda_item->m_Uuid, true ) )
            {
                aList.RemovePicker( ii );
                continue;
            }
        }

        switch( eda_item->Type() )
        {
        case PCB_FOOTPRINT_T:
        case PCB_ZONE_T:
        case PCB_TRACE_T:
        case PCB_ARC_T:
        case PCB_VIA_T:
        case PCB_PAD_T:
        case PCB_NETINFO_T:
            rebuildConnectivity = true;
            break;

        default:
            break;
        }

        switch( status )
        {
        case UNDO_REDO::CHANGED:
        {
            if( !eda_item->IsBOARD_ITEM() )
                break;

            BOARD_ITEM*           item = static_cast<BOARD_ITEM*>( eda_item );
            BOARD_ITEM*           image = static_cast<BOARD_ITEM*>( aList.GetPickedItemLink( ii ) );
            BOARD_ITEM_CONTAINER* parent = aBoard;

            if( !image )
                break;

            // The stored pointer is stale after a swap (ExchangeFootprint); go by id
            if( BOARD_ITEM* resolved = aBoard->ResolveItem( item->m_Uuid, true ) )
                item = resolved;

            if( item->GetParentFootprint() )
                parent = item->GetParentFootprint();

            parent->Remove( item, REMOVE_MODE::BULK );

            if( item->Type() != PCB_MARKER_T )
                item->SwapItemData( image );

            item->ClearFlags( UR_TRANSIENT );
            image->SetFlags( UR_TRANSIENT );
            parent->Add( item, ADD_MODE::BULK_INSERT );
            changed.push_back( item );
            break;
        }

        case UNDO_REDO::NEWITEM:
        {
            if( !eda_item->IsBOARD_ITEM() )
                break;

            BOARD_ITEM* item = static_cast<BOARD_ITEM*>( eda_item );
            aList.SetPickedItemStatus( UNDO_REDO::DELETED, ii );

            if( FOOTPRINT* parentFP = item->GetParentFootprint() )
                parentFP->Remove( item );
            else
                aBoard->Remove( item, REMOVE_MODE::BULK );

            item->SetFlags( UR_TRANSIENT );
            removed.push_back( item );
            break;
        }

        case UNDO_REDO::DELETED:
        {
            if( !eda_item->IsBOARD_ITEM() )
                break;

            BOARD_ITEM* item = static_cast<BOARD_ITEM*>( eda_item );
            aList.SetPickedItemStatus( UNDO_REDO::NEWITEM, ii );
            item->ClearFlags( UR_TRANSIENT );

            if( FOOTPRINT* parentFP = item->GetParentFootprint() )
                parentFP->Add( item );
            else
                aBoard->Add( item, ADD_MODE::BULK_APPEND );

            added.push_back( item );
            break;
        }

        case UNDO_REDO::DRILLORIGIN:
        case UNDO_REDO::GRIDORIGIN:
        {
            // The picked item carries the current origin and its link the previous one
            EDA_ITEM* image = aList.GetPickedItemLink( ii );

            if( !image )
                break;

            VECTOR2D origin = image->GetPosition();
            image->SetPosition( eda_item->GetPosition() );
            eda_item->SetPosition( origin );

            if( status == UNDO_REDO::DRILLORIGIN )
                aBoard->GetDesignSettings().SetAuxOrigin( VECTOR2I( origin ) );
            else
                aBoard->GetDesignSettings().SetGridOrigin( VECTOR2I( origin ) );

            break;
        }

        default:
            // Page settings are restored through the frame's drawing sheet proxy only
            break;
        }

        if( eda_item->Type() == PCB_FOOTPRINT_T )
        {
            FOOTPRINT* footprint = static_cast<FOOTPRINT*>( eda_item );
            footprint->InvalidateComponentClassCache();
            aBoard->GetComponentClassManager().RebuildRequiredCaches( footprint );
        }
    }

    // Group membership pointers were swapped with the rest of the item data; re-resolve them
    for( int ii = 0; ii < (int) aList.GetCount(); ++ii )
    {
        ITEM_PICKER& wrapper = aList.GetItemWrapper( ii );

        if( wrapper.GetStatus() == UNDO_REDO::DELETED || !wrapper.GetItem()->IsBOARD_ITEM() )
            continue;

        BOARD_ITEM* parentGroup = aBoard->ResolveItem( wrapper.GetGroupId(), true );
        BOARD_ITEM* boardItem = aBoard->ResolveItem( wrapper.GetItem()->m_Uuid, true );

        if( boardItem )
            boardItem->SetParentGroup( dynamic_cast<PCB_GROUP*>( parentGroup ) );

        if( PCB_GROUP* parentPcbGroup = dynamic_cast<PCB_GROUP*>( parentGroup ) )
            parentPcbGroup->GetItems().insert( boardItem );

        if( EDA_GROUP* group = dynamic_cast<PCB_GROUP*>( wrapper.GetItem() ) )
        {
            group->GetItems().clear();

            for( const KIID& member : wrapper.GetGroupMembers() )
            {
                if( BOARD_ITEM* memberItem = aBoard->ResolveItem( member, true ) )
                    group->AddItem( memberItem );
            }
        }

        // Prepare a redo by taking the group info from the current image
        if( EDA_ITEM* item = wrapper.GetLink() )
            wrapper.SetLink( item );
    }

    if( rebuildConnectivity )
    {
        aBoard->BuildConnectivity();
        aBoard->CompileRatsnest();
    }

    aBoard->GetComponentClassManager().InvalidateComponentClasses();
    aBoard->SanitizeNetcodes();

    if( !added.empty() || !removed.empty() || !changed.empty() )
        aBoard->OnItemsCompositeUpdate( added, removed, changed );
}


std::unique_ptr<API_UNDO_STACK> MakeBoardUndoStack( BOARD* aBoard )
{
    return std::make_unique<API_UNDO_STACK>(
            [aBoard]( PICKED_ITEMS_LIST& aList )
            {
                RestoreBoardUndoList( aBoard, aList );
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
