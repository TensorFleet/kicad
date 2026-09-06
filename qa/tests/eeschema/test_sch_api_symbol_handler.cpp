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
 * Tests for API_HANDLER_SYMBOL: the headless library-symbol document.
 */

#include <memory>

#include <boost/test/unit_test.hpp>

#include <api/api_handler_symbol.h>
#include <api/api_utils.h>
#include <api/headless_symbol_context.h>
#include <api/common/commands/editor_commands.pb.h>
#include <api/common/envelope.pb.h>
#include <api/schematic/schematic_types.pb.h>
#include <lib_symbol.h>
#include <sch_pin.h>
#include <sch_shape.h>
#include <settings/settings_manager.h>


namespace
{

struct API_HANDLER_SYMBOL_FIXTURE
{
    API_HANDLER_SYMBOL_FIXTURE()
    {
        // A small two-pin symbol built in memory: no library access is needed for these tests
        std::unique_ptr<LIB_SYMBOL> symbol = std::make_unique<LIB_SYMBOL>( wxS( "R" ) );
        m_libId = LIB_ID( wxS( "Device" ), wxS( "R" ) );
        symbol->SetLibId( m_libId );

        for( int i = 1; i <= 2; ++i )
        {
            SCH_PIN* pin = new SCH_PIN( symbol.get() );
            pin->SetNumber( wxString::Format( wxS( "%d" ), i ) );
            pin->SetName( wxS( "~" ) );
            pin->SetPosition( VECTOR2I( 0, i == 1 ? 1000 : -1000 ) );
            symbol->AddDrawItem( pin );
        }

        m_symbol = symbol.get();
        m_context = std::make_shared<HEADLESS_SYMBOL_CONTEXT>( std::move( symbol ), m_libId,
                                                               &m_settingsManager.Prj() );
        m_handler = std::make_unique<API_HANDLER_SYMBOL>( m_context );
    }

    kiapi::common::types::DocumentSpecifier symbolDocument( const wxString& aNickname = wxS( "Device" ),
                                                            const wxString& aName = wxS( "R" ) )
    {
        kiapi::common::types::DocumentSpecifier doc;
        doc.set_type( kiapi::common::types::DOCTYPE_SYMBOL );
        doc.mutable_lib_id()->set_library_nickname( aNickname.ToUTF8() );
        doc.mutable_lib_id()->set_entry_name( aName.ToUTF8() );
        return doc;
    }

    template <typename T>
    kiapi::common::ApiRequest makeRequest( const T& aCommand )
    {
        kiapi::common::ApiRequest request;
        request.mutable_header()->set_client_name( "kicad.qa.symbol" );
        request.mutable_message()->PackFrom( aCommand );
        return request;
    }

    template <typename Response, typename T>
    Response handle( const T& aCommand )
    {
        kiapi::common::ApiRequest request = makeRequest( aCommand );
        API_RESULT                result = m_handler->Handle( request );

        BOOST_REQUIRE_MESSAGE( result.has_value(), "request failed: " << result.error().error_message() );

        Response response;
        BOOST_REQUIRE( result->message().UnpackTo( &response ) );
        return response;
    }

    size_t drawItemCount( KICAD_T aType ) const
    {
        size_t count = 0;

        for( const SCH_ITEM& item : m_symbol->GetDrawItems() )
        {
            if( item.Type() == aType )
                ++count;
        }

        return count;
    }

    SETTINGS_MANAGER                         m_settingsManager;
    LIB_ID                                   m_libId;
    LIB_SYMBOL*                              m_symbol = nullptr;
    std::shared_ptr<HEADLESS_SYMBOL_CONTEXT> m_context;
    std::unique_ptr<API_HANDLER_SYMBOL>      m_handler;
};

} // namespace


BOOST_FIXTURE_TEST_SUITE( ApiHandlerSymbol, API_HANDLER_SYMBOL_FIXTURE )


BOOST_AUTO_TEST_CASE( DocumentAndOpenDocuments )
{
    std::optional<kiapi::common::types::DocumentSpecifier> doc = m_handler->Document();
    BOOST_REQUIRE( doc.has_value() );
    BOOST_CHECK_EQUAL( doc->type(), kiapi::common::types::DOCTYPE_SYMBOL );
    BOOST_CHECK_EQUAL( doc->lib_id().library_nickname(), "Device" );
    BOOST_CHECK_EQUAL( doc->lib_id().entry_name(), "R" );

    kiapi::common::commands::GetOpenDocuments query;
    query.set_type( kiapi::common::types::DOCTYPE_SYMBOL );

    auto response = handle<kiapi::common::commands::GetOpenDocumentsResponse>( query );
    BOOST_REQUIRE_EQUAL( response.documents_size(), 1 );
    BOOST_CHECK_EQUAL( response.documents( 0 ).lib_id().entry_name(), "R" );

    // Other document types are left to other handlers
    query.set_type( kiapi::common::types::DOCTYPE_SCHEMATIC );
    kiapi::common::ApiRequest request = makeRequest( query );
    API_RESULT                result = m_handler->Handle( request );
    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_UNHANDLED );
}


BOOST_AUTO_TEST_CASE( GetItemsReturnsChildrenAndDefinition )
{
    kiapi::common::commands::GetItems query;
    *query.mutable_header()->mutable_document() = symbolDocument();
    query.add_types( kiapi::common::types::KOT_SCH_PIN );
    query.add_types( kiapi::common::types::KOT_SCH_FIELD );

    auto response = handle<kiapi::common::commands::GetItemsResponse>( query );
    BOOST_CHECK_EQUAL( response.status(), kiapi::common::types::IRS_OK );

    int pins = 0;
    int fields = 0;

    for( const google::protobuf::Any& any : response.items() )
    {
        if( any.Is<kiapi::schematic::types::SchematicPin>() )
            ++pins;
        else if( any.Is<kiapi::schematic::types::SchematicField>() )
            ++fields;
    }

    BOOST_CHECK_EQUAL( pins, 2 );
    BOOST_CHECK_EQUAL( fields, static_cast<int>( drawItemCount( SCH_FIELD_T ) ) );

    // The library definition is served as a SchematicSymbol carrying the pins as children
    query.clear_types();
    query.add_types( kiapi::common::types::KOT_LIB_SYMBOL );
    response = handle<kiapi::common::commands::GetItemsResponse>( query );
    BOOST_REQUIRE_EQUAL( response.items_size(), 1 );

    kiapi::schematic::types::SchematicSymbol definition;
    BOOST_REQUIRE( response.items( 0 ).UnpackTo( &definition ) );
    BOOST_CHECK_EQUAL( definition.id().entry_name(), "R" );
    BOOST_CHECK_EQUAL( definition.unit_count(), 1u );

    int definitionPins = 0;

    for( const kiapi::schematic::types::SchematicSymbolChild& child : definition.items() )
    {
        if( child.item().Is<kiapi::schematic::types::SchematicPin>() )
            ++definitionPins;
    }

    BOOST_CHECK_EQUAL( definitionPins, 2 );

    // A wrong symbol name is an error, and another document type is not ours
    *query.mutable_header()->mutable_document() = symbolDocument( wxS( "Device" ), wxS( "C" ) );
    kiapi::common::ApiRequest request = makeRequest( query );
    API_RESULT                result = m_handler->Handle( request );
    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_BAD_REQUEST );

    query.mutable_header()->mutable_document()->set_type( kiapi::common::types::DOCTYPE_FOOTPRINT );
    request = makeRequest( query );
    result = m_handler->Handle( request );
    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_UNHANDLED );
}


BOOST_AUTO_TEST_CASE( CreateUpdateDeleteChildren )
{
    size_t shapesBefore = drawItemCount( SCH_SHAPE_T );

    // Create a rectangle body
    kiapi::schematic::types::SchematicGraphicShape shape;
    shape.mutable_id()->set_value( "0d3a2c3e-0000-4000-8000-000000000001" );
    shape.mutable_shape()->mutable_rectangle()->mutable_top_left()->set_x_nm( -1000000 );
    shape.mutable_shape()->mutable_rectangle()->mutable_top_left()->set_y_nm( -2000000 );
    shape.mutable_shape()->mutable_rectangle()->mutable_bottom_right()->set_x_nm( 1000000 );
    shape.mutable_shape()->mutable_rectangle()->mutable_bottom_right()->set_y_nm( 2000000 );

    kiapi::common::commands::CreateItems create;
    *create.mutable_header()->mutable_document() = symbolDocument();
    create.add_items()->PackFrom( shape );

    auto created = handle<kiapi::common::commands::CreateItemsResponse>( create );
    BOOST_REQUIRE_EQUAL( created.created_items_size(), 1 );
    BOOST_CHECK_EQUAL( created.created_items( 0 ).status().code(), kiapi::common::commands::ISC_OK );
    BOOST_CHECK_EQUAL( drawItemCount( SCH_SHAPE_T ), shapesBefore + 1 );

    // Creating it again is refused as existing
    created = handle<kiapi::common::commands::CreateItemsResponse>( create );
    BOOST_REQUIRE_EQUAL( created.created_items_size(), 1 );
    BOOST_CHECK_EQUAL( created.created_items( 0 ).status().code(), kiapi::common::commands::ISC_EXISTING );

    // Update a pin's number through UpdateItems
    kiapi::common::commands::GetItems query;
    *query.mutable_header()->mutable_document() = symbolDocument();
    query.add_types( kiapi::common::types::KOT_SCH_PIN );
    auto items = handle<kiapi::common::commands::GetItemsResponse>( query );
    BOOST_REQUIRE_EQUAL( items.items_size(), 2 );

    kiapi::schematic::types::SchematicPin pin;
    BOOST_REQUIRE( items.items( 0 ).UnpackTo( &pin ) );
    pin.set_number( "42" );

    kiapi::common::commands::UpdateItems update;
    *update.mutable_header()->mutable_document() = symbolDocument();
    update.add_items()->PackFrom( pin );

    auto updated = handle<kiapi::common::commands::UpdateItemsResponse>( update );
    BOOST_REQUIRE_EQUAL( updated.updated_items_size(), 1 );
    BOOST_CHECK_EQUAL( updated.updated_items( 0 ).status().code(), kiapi::common::commands::ISC_OK );

    bool found = false;

    for( const SCH_PIN* symbolPin : m_symbol->GetPins() )
    {
        if( symbolPin->GetNumber() == wxS( "42" ) )
            found = true;
    }

    BOOST_CHECK( found );

    // Delete the shape; mandatory fields cannot be deleted
    kiapi::common::commands::DeleteItems del;
    *del.mutable_header()->mutable_document() = symbolDocument();
    del.add_item_ids()->set_value( shape.id().value() );
    del.add_item_ids()->set_value( m_symbol->GetField( FIELD_T::REFERENCE )->m_Uuid.AsStdString() );

    auto deleted = handle<kiapi::common::commands::DeleteItemsResponse>( del );
    BOOST_REQUIRE_EQUAL( deleted.deleted_items_size(), 2 );

    for( const kiapi::common::commands::ItemDeletionResult& result : deleted.deleted_items() )
    {
        if( result.id().value() == shape.id().value() )
            BOOST_CHECK_EQUAL( result.status(), kiapi::common::commands::IDS_OK );
        else
            BOOST_CHECK_EQUAL( result.status(), kiapi::common::commands::IDS_IMMUTABLE );
    }

    BOOST_CHECK_EQUAL( drawItemCount( SCH_SHAPE_T ), shapesBefore );
    BOOST_CHECK( m_symbol->GetField( FIELD_T::REFERENCE ) != nullptr );

    // Each implicit commit advanced the revision
    kiapi::common::commands::GetDocumentRevision revisionQuery;
    *revisionQuery.mutable_document() = symbolDocument();
    auto revision = handle<kiapi::common::commands::DocumentRevisionResponse>( revisionQuery );
    BOOST_CHECK_EQUAL( revision.revision(), 3u );
}


BOOST_AUTO_TEST_CASE( DroppedCommitRestoresSymbol )
{
    size_t shapesBefore = drawItemCount( SCH_SHAPE_T );

    kiapi::common::commands::BeginCommit begin;
    *begin.mutable_header()->mutable_document() = symbolDocument();
    auto beginResponse = handle<kiapi::common::commands::BeginCommitResponse>( begin );

    kiapi::schematic::types::SchematicGraphicShape shape;
    shape.mutable_id()->set_value( "0d3a2c3e-0000-4000-8000-000000000002" );
    shape.mutable_shape()->mutable_circle()->mutable_center()->set_x_nm( 0 );
    shape.mutable_shape()->mutable_circle()->mutable_center()->set_y_nm( 0 );
    shape.mutable_shape()->mutable_circle()->mutable_radius_point()->set_x_nm( 500000 );
    shape.mutable_shape()->mutable_circle()->mutable_radius_point()->set_y_nm( 0 );

    kiapi::common::commands::CreateItems create;
    *create.mutable_header()->mutable_document() = symbolDocument();
    create.add_items()->PackFrom( shape );
    handle<kiapi::common::commands::CreateItemsResponse>( create );

    // Staged, not yet applied to the symbol
    BOOST_CHECK_EQUAL( drawItemCount( SCH_SHAPE_T ), shapesBefore );

    kiapi::common::commands::EndCommit end;
    *end.mutable_header()->mutable_document() = symbolDocument();
    *end.mutable_id() = beginResponse.id();
    end.set_action( kiapi::common::commands::CMA_DROP );
    handle<kiapi::common::commands::EndCommitResponse>( end );

    BOOST_CHECK_EQUAL( drawItemCount( SCH_SHAPE_T ), shapesBefore );

    // And a committed one is applied
    beginResponse = handle<kiapi::common::commands::BeginCommitResponse>( begin );
    handle<kiapi::common::commands::CreateItemsResponse>( create );
    *end.mutable_id() = beginResponse.id();
    end.set_action( kiapi::common::commands::CMA_COMMIT );
    handle<kiapi::common::commands::EndCommitResponse>( end );

    BOOST_CHECK_EQUAL( drawItemCount( SCH_SHAPE_T ), shapesBefore + 1 );
}


BOOST_AUTO_TEST_SUITE_END()
