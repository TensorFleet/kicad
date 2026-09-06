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


BOOST_AUTO_TEST_SUITE_END()
