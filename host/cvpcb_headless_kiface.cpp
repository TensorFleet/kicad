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

/**
 * The cvpcb KIFACE, reduced to what ERC asks of it.
 *
 * ERC_TESTER::TestFootprintLinkIssues and ::TestPinMap reach the footprint libraries through
 * KIWAY::KiFACE( FACE_CVPCB )->IfaceOrAddress(), and that is the only thing on the API path
 * that wants cvpcb at all.  cvpcb/ is a GUI module the headless build does not compile, so its
 * two exported functions are reimplemented here -- they are pure library-adapter calls, the
 * same code cvpcb/cvpcb.cpp runs -- and the face is registered statically like pcbnew's and
 * eeschema's.  Without it, a headless ERC run fails trying to dlopen _cvpcb.kiface.
 */

#include <memory>
#include <optional>
#include <set>

#include <footprint.h>
#include <footprint_library_adapter.h>
#include <kiface_base.h>
#include <kiface_ids.h>
#include <lib_id.h>
#include <libraries/library_manager.h>
#include <pad.h>
#include <pgm_base.h>
#include <project_pcb.h>

#include "static_kifaces.h"


namespace
{

int testFootprintLink( const wxString& aFootprint, PROJECT* aProject )
{
    FOOTPRINT_LIBRARY_ADAPTER* adapter = PROJECT_PCB::FootprintLibAdapter( aProject );
    LIB_ID                     fpID;

    fpID.Parse( aFootprint );

    wxString libName = fpID.GetLibNickname();
    wxString fpName = fpID.GetLibItemName();

    // Libraries may not be present at this point because nothing has triggered a load yet.
    if( !adapter->GetLibraryStatus( libName ) )
        adapter->LoadOne( libName );

    if( !adapter->HasLibrary( libName, false ) )
        return KIFACE_TEST_FOOTPRINT_LINK_NO_LIBRARY;
    else if( !adapter->HasLibrary( libName, true ) )
        return KIFACE_TEST_FOOTPRINT_LINK_LIBRARY_NOT_ENABLED;
    else if( !adapter->FootprintExists( libName, fpName ) )
        return KIFACE_TEST_FOOTPRINT_LINK_NO_FOOTPRINT;

    return 0;
}


void getFootprintPadNumbers( const wxString& aFootprint, PROJECT* aProject,
                             std::set<wxString>& aPadNumbers )
{
    LIB_ID fpID;

    if( fpID.Parse( aFootprint ) >= 0 )
        return;

    if( std::optional<LIBRARY_MANAGER_ADAPTER*> mgr =
                Pgm().GetLibraryManager().Adapter( LIBRARY_TABLE_TYPE::FOOTPRINT ) )
    {
        ( *mgr )->BlockUntilLoaded();
    }

    FOOTPRINT_LIBRARY_ADAPTER* adapter = PROJECT_PCB::FootprintLibAdapter( aProject );

    if( !adapter )
        return;

    adapter->LoadOne( fpID.GetLibNickname() );

    std::unique_ptr<FOOTPRINT> footprint;

    // Let nothing escape into the caller's ERC loop.
    try
    {
        footprint.reset( adapter->LoadFootprint( fpID, false ) );
    }
    catch( ... )
    {
    }

    if( !footprint )
        return;

    for( const PAD* pad : footprint->Pads() )
    {
        if( !pad->GetNumber().IsEmpty() )
            aPadNumbers.insert( pad->GetNumber() );
    }
}


struct CVPCB_HEADLESS_IFACE : public KIFACE_BASE
{
    CVPCB_HEADLESS_IFACE() :
            KIFACE_BASE( "cvpcb", KIWAY::FACE_CVPCB )
    {}

    bool OnKifaceStart( PGM_BASE* aProgram, int aCtlBits, KIWAY* aKiway ) override
    {
        // CVPCB_SETTINGS belongs to the frame this build does not have, and nothing headless
        // reads it; the rest of the face needs no per-process state.
        return start_common( aCtlBits );
    }

    void OnKifaceEnd() override { end_common(); }

    wxWindow* CreateKiWindow( wxWindow* aParent, int aClassId, KIWAY* aKiway,
                              int aCtlBits = 0 ) override
    {
        return nullptr;
    }

    void* IfaceOrAddress( int aDataId ) override
    {
        switch( aDataId )
        {
        case KIFACE_TEST_FOOTPRINT_LINK:   return (void*) testFootprintLink;
        case KIFACE_FOOTPRINT_PAD_NUMBERS: return (void*) getFootprintPadNumbers;
        default:                           return nullptr;
        }
    }
};


CVPCB_HEADLESS_IFACE g_cvpcbKiface;

} // namespace


extern "C" KIFACE* KifaceGetterCvPcb( int* aKIFACEversion, int aKIWAYversion, PGM_BASE* aProgram )
{
    return &g_cvpcbKiface;
}
