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

#include <memory>
#include <vector>

#include <boost/test/unit_test.hpp>

#include <wx/filename.h>

#include <qa_utils/wx_utils/unit_test_utils.h>
#include <pcbnew_utils/board_test_utils.h>

#include <api/api_enums.h>
#include <api/api_handler_pcb.h>
#include <api/headless_pcb_context.h>
#include <api/board/board.pb.h>
#include <api/board/board_commands.pb.h>
#include <api/common/commands/cross_probe_commands.pb.h>
#include <api/common/commands/editor_commands.pb.h>
#include <api/common/envelope.pb.h>
#include <api/common/types/base_types.pb.h>

#include <board.h>
#include <drc/drc_item.h>
#include <board_design_settings.h>
#include <board_stackup_manager/board_stackup.h>
#include <connectivity/connectivity_data.h>
#include <lset.h>
#include <settings/settings_manager.h>
#include <zone.h>


namespace
{

/// issue5830 is a four-copper-zone board with stable, human-readable zone UUIDs.
const wxString F_CU_ZONE   = wxS( "00000000-0000-0000-0000-00005c07d704" );
const wxString B_CU_ZONE   = wxS( "00000000-0000-0000-0000-00005c07d701" );
const wxString IN1_CU_ZONE = wxS( "00000000-0000-0000-0000-00005c07d707" );
const wxString IN2_CU_ZONE = wxS( "00000000-0000-0000-0000-00005c07d70a" );


struct API_HANDLER_PCB_FIXTURE
{
    SETTINGS_MANAGER                      m_settingsManager;
    std::unique_ptr<BOARD>                m_board;
    std::shared_ptr<HEADLESS_PCB_CONTEXT> m_context;

    // The context takes ownership of the board; the returned raw pointer lets the test inspect
    // zone state after the handler runs.
    BOARD* loadBoard( const wxString& aRelPath )
    {
        KI_TEST::LoadBoard( m_settingsManager, aRelPath, m_board );

        BOARD* board = m_board.get();
        m_context = std::make_shared<HEADLESS_PCB_CONTEXT>( std::move( m_board ),
                                                            &m_settingsManager.Prj(), nullptr );
        return board;
    }

    kiapi::common::ApiRequest makeRefillRequest( BOARD* aBoard, const std::vector<wxString>& aZoneIds ) const
    {
        kiapi::board::commands::RefillZones command;
        command.mutable_board()->set_type( kiapi::common::types::DocumentType::DOCTYPE_PCB );
        command.mutable_board()->set_board_filename(
                wxFileName( aBoard->GetFileName() ).GetFullName().ToStdString() );

        for( const wxString& id : aZoneIds )
            command.add_zones()->set_value( id.ToStdString() );

        kiapi::common::ApiRequest request;
        request.mutable_header()->set_client_name( "kicad.qa" );
        BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );

        return request;
    }

    ZONE* zoneByUuid( BOARD* aBoard, const wxString& aUuid ) const
    {
        for( ZONE* zone : aBoard->Zones() )
        {
            if( zone->m_Uuid.AsString() == aUuid )
                return zone;
        }

        return nullptr;
    }

    void unfillAll( BOARD* aBoard ) const
    {
        // Start from a clean slate so a positive IsFilled() result can only come from this fill
        for( ZONE* zone : aBoard->Zones() )
        {
            zone->UnFill();
            BOOST_REQUIRE( !zone->IsFilled() );
        }
    }
};


kiapi::common::ApiRequest makeBeginCommitRequest()
{
    // No header, so the pre-11.0 path assumes the PCB editor
    kiapi::common::commands::BeginCommit command;

    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    request.mutable_message()->PackFrom( command );

    return request;
}


template <typename T>
kiapi::common::ApiRequest makeRequest( const T& aCommand )
{
    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    request.mutable_message()->PackFrom( aCommand );

    return request;
}


kiapi::common::types::DocumentSpecifier pcbDocument( BOARD* aBoard )
{
    kiapi::common::types::DocumentSpecifier document;
    document.set_type( kiapi::common::types::DocumentType::DOCTYPE_PCB );
    document.set_board_filename( wxFileName( aBoard->GetFileName() ).GetFullName().ToStdString() );

    return document;
}


kiapi::board::BoardStackupLayer* addStackupLayer( kiapi::board::BoardStackup& aStackup,
                                                  kiapi::board::BoardStackupLayerType aType,
                                                  kiapi::board::types::BoardLayer aLayer, int aThicknessNm )
{
    kiapi::board::BoardStackupLayer* layer = aStackup.add_layers();
    layer->set_type( aType );
    layer->set_layer( aLayer );
    layer->set_enabled( true );
    layer->mutable_thickness()->set_value_nm( aThicknessNm );

    if( aType == kiapi::board::BoardStackupLayerType::BSLT_DIELECTRIC )
    {
        kiapi::board::BoardStackupDielectricProperties* props = layer->mutable_dielectric()->add_layer();
        props->set_epsilon_r( 4.5 );
        props->set_loss_tangent( 0.02 );
        props->set_material_name( "FR4" );
        props->mutable_thickness()->set_value_nm( aThicknessNm );
        layer->mutable_dielectric()->set_type( kiapi::board::BoardStackupDielectricType::BSDT_CORE );
    }

    return layer;
}


/// A minimal, valid physical stackup with the given number of copper layers
kiapi::board::BoardStackup makeCopperStackup( int aCopperLayers )
{
    using namespace kiapi::board;
    using kiapi::board::types::BoardLayer;

    BoardStackup stackup;
    stackup.mutable_finish()->set_type_name( "ENIG" );

    addStackupLayer( stackup, BSLT_SILKSCREEN, BoardLayer::BL_F_SilkS, 0 );
    addStackupLayer( stackup, BSLT_SOLDERMASK, BoardLayer::BL_F_Mask, 10000 );

    LSEQ copper = LSET::AllCuMask( aCopperLayers ).CuStack();

    for( size_t i = 0; i < copper.size(); ++i )
    {
        if( i > 0 )
            addStackupLayer( stackup, BSLT_DIELECTRIC, BoardLayer::BL_UNKNOWN, 200000 );

        addStackupLayer( stackup, BSLT_COPPER, ToProtoEnum<PCB_LAYER_ID, BoardLayer>( copper[i] ), 35000 );
    }

    addStackupLayer( stackup, BSLT_SOLDERMASK, BoardLayer::BL_B_Mask, 10000 );
    addStackupLayer( stackup, BSLT_SILKSCREEN, BoardLayer::BL_B_SilkS, 0 );

    return stackup;
}


kiapi::common::ApiRequest makeRevertRequest( BOARD* aBoard )
{
    kiapi::common::commands::RevertDocument command;
    command.mutable_document()->set_type( kiapi::common::types::DocumentType::DOCTYPE_PCB );
    command.mutable_document()->set_board_filename( wxFileName( aBoard->GetFileName() ).GetFullName().ToStdString() );

    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    request.mutable_message()->PackFrom( command );

    return request;
}

} // namespace


BOOST_FIXTURE_TEST_SUITE( ApiHandlerPcb, API_HANDLER_PCB_FIXTURE )


BOOST_AUTO_TEST_CASE( RefillZonesSubset )
{
    BOARD* board = loadBoard( wxS( "issue5830" ) );

    unfillAll( board );

    API_HANDLER_PCB           handler( m_context );
    kiapi::common::ApiRequest request = makeRefillRequest( board, { F_CU_ZONE, IN1_CU_ZONE } );
    API_RESULT                result = handler.Handle( request );

    if( !result.has_value() )
    {
        BOOST_FAIL( "RefillZones returned status " << result.error().status() << ": "
                                                    << result.error().error_message() );
    }

    BOOST_CHECK_EQUAL( result->status().status(), kiapi::common::ApiStatusCode::AS_OK );

    ZONE* fCu   = zoneByUuid( board, F_CU_ZONE );
    ZONE* bCu   = zoneByUuid( board, B_CU_ZONE );
    ZONE* in1Cu = zoneByUuid( board, IN1_CU_ZONE );
    ZONE* in2Cu = zoneByUuid( board, IN2_CU_ZONE );

    BOOST_REQUIRE( fCu && bCu && in1Cu && in2Cu );

    // Exactly the requested zones must be filled; the others must be untouched.
    BOOST_CHECK( fCu->IsFilled() );
    BOOST_CHECK( in1Cu->IsFilled() );
    BOOST_CHECK( !bCu->IsFilled() );
    BOOST_CHECK( !in2Cu->IsFilled() );
}


BOOST_AUTO_TEST_CASE( RefillZonesSubsetRebuildsConnectivity )
{
    BOARD* board = loadBoard( wxS( "issue5830" ) );

    unfillAll( board );

    // Baseline ratsnest with every zone empty; the GND planes are unfilled so their pads still
    // ratsnest together.
    board->BuildConnectivity();
    const unsigned baseline = board->GetConnectivity()->GetUnconnectedCount( false );
    BOOST_REQUIRE_MESSAGE( baseline > 0, "expected an unconnected baseline with zones empty" );

    API_HANDLER_PCB           handler( m_context );
    kiapi::common::ApiRequest request = makeRefillRequest( board, { F_CU_ZONE, IN1_CU_ZONE } );
    API_RESULT                result = handler.Handle( request );

    if( !result.has_value() )
    {
        BOOST_FAIL( "RefillZones returned status " << result.error().status() << ": "
                                                    << result.error().error_message() );
    }

    const unsigned afterFill = board->GetConnectivity()->GetUnconnectedCount( false );

    // Filling the GND planes bridges GND pads that previously ratsnested, so the unconnected
    // count drops.  Push cleared the ratsnest, so if the handler skipped the connectivity
    // rebuild this would read zero instead of the reduced-but-nonzero count.
    BOOST_CHECK_MESSAGE( afterFill > 0, "connectivity was cleared, not rebuilt, after the fill" );
    BOOST_CHECK_MESSAGE( afterFill < baseline,
                         "filling the GND planes should reduce the unconnected count ("
                                 << afterFill << " vs baseline " << baseline << ")" );
}


BOOST_AUTO_TEST_CASE( RefillZonesAllHeadless )
{
    BOARD* board = loadBoard( wxS( "issue5830" ) );

    unfillAll( board );

    API_HANDLER_PCB           handler( m_context );
    kiapi::common::ApiRequest request = makeRefillRequest( board, {} );
    API_RESULT                result = handler.Handle( request );

    if( !result.has_value() )
    {
        BOOST_FAIL( "RefillZones returned status " << result.error().status() << ": "
                                                    << result.error().error_message() );
    }

    BOOST_CHECK_EQUAL( result->status().status(), kiapi::common::ApiStatusCode::AS_OK );

    // With no frame the empty-zones request must fill everything synchronously
    for( ZONE* zone : board->Zones() )
        BOOST_CHECK_MESSAGE( zone->IsFilled(), "zone " << zone->m_Uuid.AsStdString() << " not filled" );
}


BOOST_AUTO_TEST_CASE( RefillZonesUnknownIdRejected )
{
    BOARD* board = loadBoard( wxS( "issue5830" ) );

    API_HANDLER_PCB           handler( m_context );
    kiapi::common::ApiRequest request =
            makeRefillRequest( board, { wxS( "deadbeef-0000-0000-0000-000000000000" ) } );
    API_RESULT                result = handler.Handle( request );

    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_BAD_REQUEST );
}


// RevertDocument reloads the board, freeing every item an in-flight client commit points at.
// The handler must refuse with AS_BUSY while a commit is open, or the pointers dangle
// (use-after-free). Latent UAF found via #24803.
BOOST_AUTO_TEST_CASE( RevertDocumentRejectedWithOpenCommit )
{
    BOARD* board = loadBoard( wxS( "issue5830" ) );

    API_HANDLER_PCB handler( m_context );

    // Open a client transaction, as a client staging edits would
    kiapi::common::ApiRequest beginRequest = makeBeginCommitRequest();
    BOOST_REQUIRE( handler.Handle( beginRequest ).has_value() );

    kiapi::common::ApiRequest request = makeRevertRequest( board );
    API_RESULT                result = handler.Handle( request );

    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_BUSY );
    BOOST_CHECK( result.error().error_message().find( "commit" ) != std::string::npos );
}


// With no open commit the guard passes; the reload then needs a running editor, so a headless
// handler reports AS_UNIMPLEMENTED. This confirms the guard does not reject the normal path.
BOOST_AUTO_TEST_CASE( RevertDocumentWithoutCommitPassesGuard )
{
    BOARD* board = loadBoard( wxS( "issue5830" ) );

    API_HANDLER_PCB           handler( m_context );
    kiapi::common::ApiRequest request = makeRevertRequest( board );
    API_RESULT                result = handler.Handle( request );

    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_UNIMPLEMENTED );
}


BOOST_AUTO_TEST_CASE( UpdateBoardStackupRoundTrip )
{
    BOARD* board = loadBoard( wxS( "issue5830" ) );

    API_HANDLER_PCB handler( m_context );

    kiapi::board::commands::UpdateBoardStackup command;
    *command.mutable_board() = pcbDocument( board );
    *command.mutable_stackup() = makeCopperStackup( 4 );
    command.mutable_stackup()->mutable_impedance()->set_is_controlled( true );

    kiapi::common::ApiRequest request = makeRequest( command );
    API_RESULT                result = handler.Handle( request );

    if( !result.has_value() )
    {
        BOOST_FAIL( "UpdateBoardStackup returned status " << result.error().status() << ": "
                                                           << result.error().error_message() );
    }

    kiapi::board::commands::BoardStackupResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );

    const BOARD_DESIGN_SETTINGS& bds = board->GetDesignSettings();
    const BOARD_STACKUP&         stackup = bds.GetStackupDescriptor();

    BOOST_CHECK( bds.m_HasStackup );
    BOOST_CHECK_EQUAL( board->GetCopperLayerCount(), 4 );
    BOOST_CHECK_EQUAL( stackup.m_FinishType, wxS( "ENIG" ) );
    BOOST_CHECK( stackup.m_HasDielectricConstrains );

    // 4 copper + 3 dielectric + 2 mask + 2 silk
    BOOST_CHECK_EQUAL( stackup.GetCount(), 11 );
    BOOST_CHECK_EQUAL( response.stackup().layers_size(), 11 );
    BOOST_CHECK_EQUAL( response.stackup().finish().type_name(), "ENIG" );

    // Board thickness follows the stackup: 4 * 35um + 3 * 200um + 2 * 10um
    BOOST_CHECK_EQUAL( bds.GetBoardThickness(), stackup.BuildBoardThicknessFromStackup() );
    BOOST_CHECK_EQUAL( bds.GetBoardThickness(), 4 * 35000 + 3 * 200000 + 2 * 10000 );

    // The response carries the user-visible layer names that Deserialize does not know about
    bool sawFCu = false;

    for( const kiapi::board::BoardStackupLayer& layer : response.stackup().layers() )
    {
        if( layer.layer() == kiapi::board::types::BoardLayer::BL_F_Cu )
        {
            sawFCu = true;
            BOOST_CHECK_EQUAL( layer.user_name(), board->GetLayerName( F_Cu ).ToStdString() );
        }
    }

    BOOST_CHECK( sawFCu );
}


BOOST_AUTO_TEST_CASE( UpdateBoardStackupRejectsMalformed )
{
    BOARD* board = loadBoard( wxS( "issue5830" ) );

    API_HANDLER_PCB handler( m_context );

    const int originalCount = board->GetCopperLayerCount();

    // Odd number of copper layers
    {
        kiapi::board::commands::UpdateBoardStackup command;
        *command.mutable_board() = pcbDocument( board );
        *command.mutable_stackup() = makeCopperStackup( 4 );
        addStackupLayer( *command.mutable_stackup(), kiapi::board::BSLT_COPPER,
                         kiapi::board::types::BoardLayer::BL_In3_Cu, 35000 );

        kiapi::common::ApiRequest request = makeRequest( command );
        API_RESULT                result = handler.Handle( request );

        BOOST_REQUIRE( !result.has_value() );
        BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_BAD_REQUEST );
    }

    // A copper entry that names a non-copper board layer
    {
        kiapi::board::commands::UpdateBoardStackup command;
        *command.mutable_board() = pcbDocument( board );
        addStackupLayer( *command.mutable_stackup(), kiapi::board::BSLT_COPPER,
                         kiapi::board::types::BoardLayer::BL_F_Mask, 35000 );
        addStackupLayer( *command.mutable_stackup(), kiapi::board::BSLT_COPPER,
                         kiapi::board::types::BoardLayer::BL_B_Cu, 35000 );

        kiapi::common::ApiRequest request = makeRequest( command );
        API_RESULT                result = handler.Handle( request );

        BOOST_REQUIRE( !result.has_value() );
        BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_BAD_REQUEST );
    }

    // Copper layers out of stack order
    {
        kiapi::board::commands::UpdateBoardStackup command;
        *command.mutable_board() = pcbDocument( board );
        addStackupLayer( *command.mutable_stackup(), kiapi::board::BSLT_COPPER,
                         kiapi::board::types::BoardLayer::BL_B_Cu, 35000 );
        addStackupLayer( *command.mutable_stackup(), kiapi::board::BSLT_COPPER,
                         kiapi::board::types::BoardLayer::BL_F_Cu, 35000 );

        kiapi::common::ApiRequest request = makeRequest( command );
        API_RESULT                result = handler.Handle( request );

        BOOST_REQUIRE( !result.has_value() );
        BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_BAD_REQUEST );
    }

    BOOST_CHECK_EQUAL( board->GetCopperLayerCount(), originalCount );
}


// Dropping copper layers from the stackup disables them and deletes their content, as the
// message documentation warns
BOOST_AUTO_TEST_CASE( UpdateBoardStackupRemovesLayers )
{
    BOARD* board = loadBoard( wxS( "issue5830" ) );

    BOOST_REQUIRE_EQUAL( board->GetCopperLayerCount(), 4 );
    BOOST_REQUIRE( zoneByUuid( board, IN1_CU_ZONE ) );

    API_HANDLER_PCB handler( m_context );

    kiapi::board::commands::UpdateBoardStackup command;
    *command.mutable_board() = pcbDocument( board );
    *command.mutable_stackup() = makeCopperStackup( 2 );

    kiapi::common::ApiRequest request = makeRequest( command );
    API_RESULT                result = handler.Handle( request );

    if( !result.has_value() )
    {
        BOOST_FAIL( "UpdateBoardStackup returned status " << result.error().status() << ": "
                                                           << result.error().error_message() );
    }

    BOOST_CHECK_EQUAL( board->GetCopperLayerCount(), 2 );
    BOOST_CHECK( !board->IsLayerEnabled( In1_Cu ) );
    BOOST_CHECK( !board->IsLayerEnabled( In2_Cu ) );
    BOOST_CHECK( board->IsLayerEnabled( F_Cu ) );
    BOOST_CHECK( board->IsLayerEnabled( Edge_Cuts ) );

    // Zones on the removed inner layers are gone; the outer ones survive
    BOOST_CHECK( !zoneByUuid( board, IN1_CU_ZONE ) );
    BOOST_CHECK( !zoneByUuid( board, IN2_CU_ZONE ) );
    BOOST_CHECK( zoneByUuid( board, F_CU_ZONE ) );
    BOOST_CHECK( zoneByUuid( board, B_CU_ZONE ) );
}


BOOST_AUTO_TEST_CASE( RefreshEditorHeadlessIsNoOp )
{
    loadBoard( wxS( "issue5830" ) );

    API_HANDLER_PCB handler( m_context );

    kiapi::common::commands::RefreshEditor command;
    command.set_frame( kiapi::common::types::FT_PCB_EDITOR );

    kiapi::common::ApiRequest request = makeRequest( command );
    API_RESULT                result = handler.Handle( request );

    BOOST_REQUIRE( result.has_value() );
    BOOST_CHECK_EQUAL( result->status().status(), kiapi::common::ApiStatusCode::AS_OK );

    // A request for another editor is left for that editor's handler
    command.set_frame( kiapi::common::types::FT_SCHEMATIC_EDITOR );
    request = makeRequest( command );
    result = handler.Handle( request );

    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_UNHANDLED );
}


BOOST_AUTO_TEST_CASE( FocusOnItemHeadlessIsNoOp )
{
    loadBoard( wxS( "issue5830" ) );

    API_HANDLER_PCB handler( m_context );

    kiapi::common::commands::FocusOnItem command;
    command.mutable_focus_item()->mutable_footprint()->set_reference( "R1" );

    kiapi::common::ApiRequest request = makeRequest( command );
    API_RESULT                result = handler.Handle( request );

    BOOST_REQUIRE( result.has_value() );
    BOOST_CHECK_EQUAL( result->status().status(), kiapi::common::ApiStatusCode::AS_OK );

    kiapi::common::commands::FocusOnItemResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );
    BOOST_CHECK_EQUAL( response.status(), kiapi::common::commands::CPS_OK );

    // Sheet paths belong to the schematic handler
    command.mutable_focus_item()->mutable_sheet_path();
    request = makeRequest( command );
    result = handler.Handle( request );

    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_UNHANDLED );
}


BOOST_AUTO_TEST_CASE( SaveItemsToStringHeadless )
{
    BOARD* board = loadBoard( wxS( "issue5830" ) );

    API_HANDLER_PCB handler( m_context );

    kiapi::common::commands::SaveItemsToString command;
    *command.mutable_header()->mutable_document() = pcbDocument( board );
    command.add_items()->set_value( F_CU_ZONE.ToStdString() );
    command.add_items()->set_value( B_CU_ZONE.ToStdString() );

    kiapi::common::ApiRequest request = makeRequest( command );
    API_RESULT                result = handler.Handle( request );

    if( !result.has_value() )
    {
        BOOST_FAIL( "SaveItemsToString returned status " << result.error().status() << ": "
                                                          << result.error().error_message() );
    }

    kiapi::common::commands::SavedSelectionResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );

    BOOST_REQUIRE_EQUAL( response.ids_size(), 2 );
    BOOST_CHECK_EQUAL( response.ids( 0 ).value(), F_CU_ZONE.ToStdString() );
    BOOST_CHECK_EQUAL( response.ids( 1 ).value(), B_CU_ZONE.ToStdString() );

    // Same clipboard format as SaveSelectionToString: a board wrapper with the items inside
    BOOST_CHECK( response.contents().find( "(kicad_pcb" ) != std::string::npos );
    BOOST_CHECK( response.contents().find( "(zone" ) != std::string::npos );
    BOOST_CHECK( response.contents().find( "(footprint" ) == std::string::npos );

    // Unknown items are an error rather than silently skipped
    command.add_items()->set_value( "deadbeef-0000-0000-0000-000000000000" );
    request = makeRequest( command );
    result = handler.Handle( request );

    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_BAD_REQUEST );

    // A header for another document type is passed on to the other handlers
    command.mutable_header()->mutable_document()->set_type( kiapi::common::types::DOCTYPE_SCHEMATIC );
    request = makeRequest( command );
    result = handler.Handle( request );

    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_UNHANDLED );
}


BOOST_AUTO_TEST_CASE( GetDocumentRevisionTracksChanges )
{
    BOARD* board = loadBoard( wxS( "issue5830" ) );

    API_HANDLER_PCB handler( m_context );

    kiapi::common::commands::GetDocumentRevision query;
    *query.mutable_document() = pcbDocument( board );

    auto revision = [&]() -> uint64_t
    {
        kiapi::common::ApiRequest request = makeRequest( query );
        API_RESULT                result = handler.Handle( request );

        BOOST_REQUIRE_MESSAGE( result.has_value(), "GetDocumentRevision failed: " << result.error().error_message() );

        kiapi::common::commands::DocumentRevisionResponse response;
        BOOST_REQUIRE( result->message().UnpackTo( &response ) );
        return response.revision();
    };

    // Freshly opened, and reading does not count as a change
    BOOST_CHECK_EQUAL( revision(), 0u );
    BOOST_CHECK_EQUAL( revision(), 0u );

    // A settings change outside of a commit
    kiapi::board::commands::SetBoardOrigin origin;
    *origin.mutable_board() = pcbDocument( board );
    origin.set_type( kiapi::board::commands::BOT_GRID );
    origin.mutable_origin()->set_x_nm( 1000000 );
    origin.mutable_origin()->set_y_nm( 2000000 );

    kiapi::common::ApiRequest request = makeRequest( origin );
    BOOST_REQUIRE( handler.Handle( request ).has_value() );
    BOOST_CHECK_EQUAL( revision(), 1u );

    // A commit: the revision advances when it is pushed, not when it is opened
    request = makeBeginCommitRequest();
    API_RESULT begin = handler.Handle( request );
    BOOST_REQUIRE( begin.has_value() );

    kiapi::common::commands::BeginCommitResponse beginResponse;
    BOOST_REQUIRE( begin->message().UnpackTo( &beginResponse ) );
    BOOST_CHECK_EQUAL( revision(), 1u );

    kiapi::common::commands::EndCommit end;
    *end.mutable_id() = beginResponse.id();
    end.set_action( kiapi::common::commands::CMA_COMMIT );

    request = makeRequest( end );
    BOOST_REQUIRE( handler.Handle( request ).has_value() );
    BOOST_CHECK_EQUAL( revision(), 2u );

    // Zone refills change the board too
    request = makeRefillRequest( board, {} );
    BOOST_REQUIRE( handler.Handle( request ).has_value() );
    BOOST_CHECK_EQUAL( revision(), 3u );

    // Documents that are not open are an error; other document types belong to other handlers
    query.mutable_document()->set_board_filename( "not_open.kicad_pcb" );
    request = makeRequest( query );
    API_RESULT result = handler.Handle( request );
    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_BAD_REQUEST );

    query.mutable_document()->set_type( kiapi::common::types::DOCTYPE_SCHEMATIC );
    request = makeRequest( query );
    result = handler.Handle( request );
    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_UNHANDLED );
}


BOOST_AUTO_TEST_CASE( RunBoardJobDrcPopulatesMarkers )
{
    BOARD* board = loadBoard( wxS( "api_kitchen_sink" ) );

    API_HANDLER_PCB handler( m_context );

    auto handle = [&]( const auto& aCommand, auto& aResponse )
    {
        kiapi::common::ApiRequest request = makeRequest( aCommand );
        API_RESULT                result = handler.Handle( request );

        BOOST_REQUIRE_MESSAGE( result.has_value(), "request failed: " << result.error().error_message() );
        BOOST_REQUIRE( result->message().UnpackTo( &aResponse ) );
    };

    // Nothing has been checked yet
    kiapi::board::commands::GetDrcMarkers get;
    *get.mutable_board() = pcbDocument( board );

    kiapi::board::commands::DrcResultsResponse before;
    handle( get, before );
    BOOST_CHECK_EQUAL( before.markers_size(), static_cast<int>( board->Markers().size() ) );

    // The kitchen sink has violations; running DRC places markers on the board
    kiapi::board::commands::RunBoardJobDrc run;
    *run.mutable_board() = pcbDocument( board );
    run.set_report_all_track_errors( true );

    kiapi::board::commands::DrcResultsResponse results;
    handle( run, results );

    BOOST_CHECK_GT( results.markers_size(), 0 );
    BOOST_CHECK_EQUAL( results.markers_size(), static_cast<int>( board->Markers().size() ) );
    BOOST_CHECK_EQUAL( results.error_count() + results.warning_count() + results.exclusion_count(),
                       static_cast<uint32_t>( results.markers_size() ) );

    // Every marker carries its identity and effective severity
    for( const kiapi::board::DrcMarker& marker : results.markers() )
    {
        BOOST_CHECK( !marker.id().value().empty() );
        BOOST_CHECK( marker.severity() != kiapi::common::types::RS_UNKNOWN );
        BOOST_CHECK( !marker.description().empty() );
        BOOST_CHECK( !marker.excluded() );
    }

    // GetDrcMarkers reports the same set without re-running
    kiapi::board::commands::DrcResultsResponse after;
    handle( get, after );
    BOOST_CHECK_EQUAL( after.markers_size(), results.markers_size() );
    BOOST_CHECK_EQUAL( after.error_count(), results.error_count() );

    // Excluding a marker moves it into the exclusion count and records the exclusion
    kiapi::board::commands::SetDrcMarkerExcluded exclude;
    *exclude.mutable_board() = pcbDocument( board );
    *exclude.add_markers() = results.markers( 0 ).id();
    exclude.set_excluded( true );
    exclude.set_comment( "known" );

    google::protobuf::Empty empty;
    handle( exclude, empty );
    handle( get, after );

    BOOST_CHECK_EQUAL( after.exclusion_count(), 1u );
    BOOST_CHECK_EQUAL( after.error_count() + after.warning_count(), results.error_count() + results.warning_count() - 1 );
    BOOST_CHECK_EQUAL( board->GetDesignSettings().m_DrcExclusions.size(), 1u );

    bool sawExcluded = false;

    for( const kiapi::board::DrcMarker& marker : after.markers() )
    {
        if( marker.id().value() == results.markers( 0 ).id().value() )
        {
            sawExcluded = true;
            BOOST_CHECK( marker.excluded() );
            BOOST_CHECK_EQUAL( marker.exclusion_comment(), "known" );
            BOOST_CHECK_EQUAL( marker.severity(), kiapi::common::types::RS_EXCLUSION );
        }
    }

    BOOST_CHECK( sawExcluded );

    // Exclusions survive a re-run
    handle( run, results );
    BOOST_CHECK_EQUAL( results.exclusion_count(), 1u );

    // An injected marker is reported until the next run replaces the markers
    kiapi::board::commands::InjectDrcError inject;
    *inject.mutable_board() = pcbDocument( board );
    inject.set_severity( kiapi::board::commands::DRS_ERROR );
    inject.set_message( "injected" );
    inject.mutable_position()->set_x_nm( 1000000 );
    inject.mutable_position()->set_y_nm( 1000000 );

    kiapi::board::commands::InjectDrcErrorResponse injected;
    handle( inject, injected );
    handle( get, after );
    BOOST_CHECK_EQUAL( after.markers_size(), results.markers_size() + 1 );

    // Injecting pushes its own commit, so a run is not blocked afterwards, and an open commit
    // without staged changes does not block it either
    kiapi::common::ApiRequest beginRequest = makeBeginCommitRequest();
    API_RESULT                begin = handler.Handle( beginRequest );
    BOOST_REQUIRE( begin.has_value() );

    handle( run, after );
    BOOST_CHECK_EQUAL( after.markers_size(), results.markers_size() );

    for( const kiapi::board::DrcMarker& marker : after.markers() )
        BOOST_CHECK_NE( marker.description(), "injected" );

    kiapi::common::commands::BeginCommitResponse beginResponse;
    BOOST_REQUIRE( begin->message().UnpackTo( &beginResponse ) );

    kiapi::common::commands::EndCommit end;
    *end.mutable_id() = beginResponse.id();
    end.set_action( kiapi::common::commands::CMA_DROP );

    kiapi::common::commands::EndCommitResponse ended;
    handle( end, ended );

    // Unknown markers are an error
    exclude.mutable_markers( 0 )->set_value( "deadbeef-0000-0000-0000-000000000000" );
    kiapi::common::ApiRequest request = makeRequest( exclude );
    API_RESULT                result = handler.Handle( request );
    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_BAD_REQUEST );
}


BOOST_AUTO_TEST_CASE( DrcSeveritiesRoundTrip )
{
    BOARD* board = loadBoard( wxS( "api_kitchen_sink" ) );

    API_HANDLER_PCB handler( m_context );

    auto handle = [&]( const auto& aCommand, auto& aResponse )
    {
        kiapi::common::ApiRequest request = makeRequest( aCommand );
        API_RESULT                result = handler.Handle( request );

        BOOST_REQUIRE_MESSAGE( result.has_value(), "request failed: " << result.error().error_message() );
        BOOST_REQUIRE( result->message().UnpackTo( &aResponse ) );
    };

    kiapi::board::commands::GetDrcSeverities get;
    *get.mutable_board() = pcbDocument( board );

    kiapi::board::commands::DrcSeveritiesResponse severities;
    handle( get, severities );

    // Every user-settable rule type is listed once
    BOOST_CHECK_EQUAL( severities.severities_size(), static_cast<int>( DRC_ITEM::GetItemsWithSeverities().size() ) );

    std::set<int> seen;

    for( const kiapi::board::DrcSeveritySetting& setting : severities.severities() )
    {
        BOOST_CHECK( setting.rule_type() != kiapi::board::DRCET_UNKNOWN );
        BOOST_CHECK( seen.insert( setting.rule_type() ).second );
    }

    // Change one; the others keep their value
    kiapi::board::commands::SetDrcSeverities set;
    *set.mutable_board() = pcbDocument( board );
    kiapi::board::DrcSeveritySetting* change = set.add_severities();
    change->set_rule_type( kiapi::board::DRCET_CLEARANCE );
    change->set_severity( kiapi::common::types::RS_IGNORE );

    kiapi::board::commands::DrcSeveritiesResponse updated;
    handle( set, updated );
    BOOST_CHECK_EQUAL( updated.severities_size(), severities.severities_size() );
    BOOST_CHECK_EQUAL( board->GetDesignSettings().GetSeverity( DRCE_CLEARANCE ), RPT_SEVERITY_IGNORE );

    for( const kiapi::board::DrcSeveritySetting& setting : updated.severities() )
    {
        if( setting.rule_type() == kiapi::board::DRCET_CLEARANCE )
            BOOST_CHECK_EQUAL( setting.severity(), kiapi::common::types::RS_IGNORE );
    }

    // Exclusion is not a valid severity to set
    change->set_severity( kiapi::common::types::RS_EXCLUSION );
    kiapi::common::ApiRequest request = makeRequest( set );
    API_RESULT                result = handler.Handle( request );
    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_BAD_REQUEST );
}


BOOST_AUTO_TEST_SUITE_END()
