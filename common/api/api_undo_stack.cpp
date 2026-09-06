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

#include <api/api_undo_stack.h>
#include <eda_item.h>


API_UNDO_STACK::API_UNDO_STACK( RESTORE_FN aRestore, DELETER_FN aDeleter, size_t aMaxLevels ) :
        m_restore( std::move( aRestore ) ),
        m_deleter( std::move( aDeleter ) ),
        m_maxLevels( aMaxLevels )
{
}


API_UNDO_STACK::~API_UNDO_STACK()
{
    Clear();
}


void API_UNDO_STACK::SaveCopyInUndoList( const PICKED_ITEMS_LIST& aItemsList, bool aAppend )
{
    if( aItemsList.GetCount() == 0 )
        return;

    PICKED_ITEMS_LIST* command = nullptr;

    if( aAppend && !m_undo.m_CommandsList.empty() )
    {
        command = m_undo.m_CommandsList.back();
    }
    else
    {
        command = new PICKED_ITEMS_LIST();
        command->SetDescription( aItemsList.GetDescription() );
        m_undo.PushCommand( command );

        ENTRY entry = m_pending.value_or( ENTRY() );
        entry.Description = aItemsList.GetDescription();
        m_entries[command] = entry;
    }

    m_pending.reset();

    for( unsigned ii = 0; ii < aItemsList.GetCount(); ii++ )
        command->PushItem( aItemsList.GetItemWrapper( ii ) );

    m_entries[command].ItemCount = command->GetCount();

    // A new command makes the redo history unreachable
    clearList( m_redo );

    while( m_undo.m_CommandsList.size() > m_maxLevels )
    {
        PICKED_ITEMS_LIST* oldest = m_undo.m_CommandsList.front();
        m_undo.m_CommandsList.erase( m_undo.m_CommandsList.begin() );
        m_entries.erase( oldest );
        m_deleter( *oldest );
        delete oldest;
    }
}


void API_UNDO_STACK::SetNextAttribution( const std::string& aClientName, const std::optional<KIID>& aCommitId )
{
    ENTRY entry;
    entry.ClientName = aClientName;
    entry.CommitId = aCommitId;
    m_pending = entry;
}


API_UNDO_STACK::ENTRY API_UNDO_STACK::entryFor( const PICKED_ITEMS_LIST* aList ) const
{
    auto it = m_entries.find( aList );

    if( it != m_entries.end() )
        return it->second;

    ENTRY entry;
    entry.Description = aList->GetDescription();
    entry.ItemCount = aList->GetCount();
    return entry;
}


std::vector<API_UNDO_STACK::ENTRY> API_UNDO_STACK::UndoEntries() const
{
    std::vector<ENTRY> entries;

    for( const PICKED_ITEMS_LIST* list : m_undo.m_CommandsList )
        entries.push_back( entryFor( list ) );

    return entries;
}


std::vector<API_UNDO_STACK::ENTRY> API_UNDO_STACK::RedoEntries() const
{
    std::vector<ENTRY> entries;

    for( const PICKED_ITEMS_LIST* list : m_redo.m_CommandsList )
        entries.push_back( entryFor( list ) );

    return entries;
}


std::optional<API_UNDO_STACK::RESULT> API_UNDO_STACK::Undo()
{
    return apply( m_undo, m_redo );
}


std::optional<API_UNDO_STACK::RESULT> API_UNDO_STACK::Redo()
{
    return apply( m_redo, m_undo );
}


std::optional<API_UNDO_STACK::RESULT> API_UNDO_STACK::apply( UNDO_REDO_CONTAINER& aFrom, UNDO_REDO_CONTAINER& aTo )
{
    PICKED_ITEMS_LIST* command = aFrom.PopCommand();

    if( !command )
        return std::nullopt;

    RESULT result;
    result.Entry = entryFor( command );

    // What the document will look like once the recorded state is put back
    for( unsigned ii = 0; ii < command->GetCount(); ii++ )
    {
        EDA_ITEM* item = command->GetPickedItem( ii );

        if( !item )
            continue;

        switch( command->GetPickedItemStatus( ii ) )
        {
        case UNDO_REDO::NEWITEM: result.Deleted.push_back( item->m_Uuid ); break;
        case UNDO_REDO::DELETED: result.Created.push_back( item->m_Uuid ); break;
        case UNDO_REDO::CHANGED:
        case UNDO_REDO::LIBEDIT: result.Updated.push_back( item->m_Uuid ); break;
        default:                                                          break;
        }
    }

    m_restore( *command );

    aTo.PushCommand( command );
    return result;
}


void API_UNDO_STACK::clearList( UNDO_REDO_CONTAINER& aList )
{
    for( PICKED_ITEMS_LIST* command : aList.m_CommandsList )
    {
        m_entries.erase( command );
        m_deleter( *command );
        delete command;
    }

    aList.m_CommandsList.clear();
}


void API_UNDO_STACK::Clear()
{
    clearList( m_undo );
    clearList( m_redo );
    m_pending.reset();
}
