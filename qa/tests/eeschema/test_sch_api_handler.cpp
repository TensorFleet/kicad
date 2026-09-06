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

#include <boost/test/unit_test.hpp>

#include <api/api_handler_sch.h>
#include <api/api_utils.h>
#include <api/common/commands/editor_commands.pb.h>
#include <api/common/envelope.pb.h>
#include <api/api_sch_utils.h>
#include <api/headless_sch_context.h>
#include <api/schematic/schematic_types.pb.h>
#include <schematic_utils/schematic_file_util.h>
#include <settings/settings_manager.h>

#include <core/ignore.h>
#include <schematic.h>
#include <api/schematic/schematic_commands.pb.h>
#include <sch_screen.h>
#include <sch_marker.h>
#include <sch_symbol.h>
#include <sch_pin.h>
#include <erc/erc_settings.h>
#include <erc/erc_item.h>


namespace
{

struct API_HANDLER_SCH_FIXTURE
{
    SETTINGS_MANAGER                      m_settingsManager;
    std::unique_ptr<SCHEMATIC>            m_schematic;
    std::shared_ptr<HEADLESS_SCH_CONTEXT> m_context;

    SCHEMATIC* loadSchematic( const wxString& aRelPath )
    {
        KI_TEST::LoadSchematic( m_settingsManager, aRelPath, m_schematic );
        m_context = std::make_shared<HEADLESS_SCH_CONTEXT>( m_schematic.get(),
                                                            &m_settingsManager.Prj() );
        return m_schematic.get();
    }
};


kiapi::common::types::DocumentSpecifier makeDocument( const SCHEMATIC& aSchematic )
{
    kiapi::common::types::DocumentSpecifier document;
    document.set_type( kiapi::common::types::DocumentType::DOCTYPE_SCHEMATIC );
    kiapi::common::PackProject( *document.mutable_project(), aSchematic.Project() );

    return document;
}


kiapi::common::ApiRequest makeBeginCommitRequest( const SCHEMATIC& aSchematic )
{
    kiapi::common::commands::BeginCommit command;
    *command.mutable_header()->mutable_document() = makeDocument( aSchematic );

    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );

    return request;
}


kiapi::common::ApiRequest makeRevertRequest( const SCHEMATIC& aSchematic )
{
    kiapi::common::commands::RevertDocument command;
    *command.mutable_document() = makeDocument( aSchematic );

    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );

    return request;
}


}


BOOST_FIXTURE_TEST_SUITE( ApiHandlerSch, API_HANDLER_SCH_FIXTURE )


BOOST_AUTO_TEST_CASE( RevertDocumentRejectedWithOpenCommit )
{
    SCHEMATIC* schematic = loadSchematic( wxS( "api_kitchen_sink" ) );

    API_HANDLER_SCH handler( m_context );

    kiapi::common::ApiRequest beginRequest = makeBeginCommitRequest( *schematic );
    BOOST_REQUIRE( handler.Handle( beginRequest ).has_value() );

    kiapi::common::ApiRequest request = makeRevertRequest( *schematic );
    API_RESULT                result = handler.Handle( request );

    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_BUSY );
    BOOST_CHECK( result.error().error_message().find( "commit" ) != std::string::npos );
}



// Since 11.0: title blocks are per sheet, resolved from the document's sheet_path; headless the
// root sheet is the default.  A board document is declined so the board handler can answer.
BOOST_AUTO_TEST_CASE( TitleBlockResolvesSheetHeadless )
{
    SCHEMATIC* schematic = loadSchematic( wxS( "api_kitchen_sink" ) );

    API_HANDLER_SCH handler( m_context );

    SCH_SHEET_LIST hierarchy = schematic->Hierarchy();
    BOOST_REQUIRE_GE( hierarchy.size(), 2 );

    kiapi::common::commands::SetTitleBlockInfo setCommand;
    *setCommand.mutable_document() = makeDocument( *schematic );
    kiapi::common::PackSheetPath( *setCommand.mutable_document()->mutable_sheet_path(), hierarchy.at( 1 ).Path() );
    setCommand.mutable_title_block()->set_title( "sub sheet" );

    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( setCommand ) );

    API_RESULT result = handler.Handle( request );
    BOOST_REQUIRE_MESSAGE( result.has_value(), result.error().error_message() );

    BOOST_CHECK_EQUAL( hierarchy.at( 1 ).LastScreen()->GetTitleBlock().GetTitle().ToStdString(), "sub sheet" );
    BOOST_CHECK_NE( hierarchy.at( 0 ).LastScreen()->GetTitleBlock().GetTitle().ToStdString(), "sub sheet" );

    // No sheet path: the root sheet
    kiapi::common::commands::GetTitleBlockInfo getCommand;
    *getCommand.mutable_document() = makeDocument( *schematic );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( getCommand ) );

    result = handler.Handle( request );
    BOOST_REQUIRE_MESSAGE( result.has_value(), result.error().error_message() );

    kiapi::common::types::TitleBlockInfo info;
    BOOST_REQUIRE( result->message().UnpackTo( &info ) );
    BOOST_CHECK_EQUAL( info.title(), hierarchy.at( 0 ).LastScreen()->GetTitleBlock().GetTitle().ToStdString() );

    // Another editor's document is passed on rather than rejected
    getCommand.mutable_document()->set_type( kiapi::common::types::DocumentType::DOCTYPE_PCB );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( getCommand ) );

    result = handler.Handle( request );
    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_UNHANDLED );
}


// Since 11.0: sending a symbol back unchanged through UpdateItems keeps the library symbol the
// sheet already has (no "<name>_1" entry in lib_symbols, no re-linking) and the pin order.
BOOST_AUTO_TEST_CASE( UnchangedSymbolUpdateKeepsLibSymbols )
{
    SCHEMATIC* schematic = loadSchematic( wxS( "api_kitchen_sink" ) );

    API_HANDLER_SCH handler( m_context );

    SCH_SHEET_PATH rootPath = schematic->Hierarchy().at( 0 );
    SCH_SCREEN*    screen = rootPath.LastScreen();
    SCH_SYMBOL*    symbol = nullptr;

    for( SCH_ITEM* item : screen->Items().OfType( SCH_SYMBOL_T ) )
    {
        symbol = static_cast<SCH_SYMBOL*>( item );

        if( symbol->UseLibIdLookup() && symbol->GetLibSymbolRef() )
            break;
    }

    BOOST_REQUIRE( symbol );

    std::vector<wxString> libNamesBefore;

    for( const auto& [name, unused] : screen->GetLibSymbols() )
        libNamesBefore.push_back( name );

    std::vector<KIID> pinOrderBefore;

    for( const std::unique_ptr<SCH_PIN>& pin : symbol->GetRawPins() )
        pinOrderBefore.push_back( pin->m_Uuid );

    BOOST_REQUIRE_GT( pinOrderBefore.size(), 1 );

    kiapi::schematic::types::SchematicSymbolInstance packed;
    BOOST_REQUIRE( PackSymbol( &packed, symbol, rootPath ) );

    kiapi::common::commands::UpdateItems command;
    *command.mutable_header()->mutable_document() = makeDocument( *schematic );
    command.add_items()->PackFrom( packed );

    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );

    API_RESULT result = handler.Handle( request );
    BOOST_REQUIRE_MESSAGE( result.has_value(), result.error().error_message() );

    std::vector<wxString> libNamesAfter;

    for( const auto& [name, unused] : screen->GetLibSymbols() )
        libNamesAfter.push_back( name );

    BOOST_CHECK( libNamesAfter == libNamesBefore );
    BOOST_CHECK( symbol->UseLibIdLookup() );

    std::vector<KIID> pinOrderAfter;

    for( const std::unique_ptr<SCH_PIN>& pin : symbol->GetRawPins() )
        pinOrderAfter.push_back( pin->m_Uuid );

    BOOST_CHECK( pinOrderAfter == pinOrderBefore );
}


// Since 11.0: the schematic handler serves the clipboard-format commands.  Pasting a copied
// symbol keeps the sheet's lib_symbols cache as it is (the pasted text carries an exact copy of
// the library symbol) and gives the copy new ids.
BOOST_AUTO_TEST_CASE( SaveAndParseItemsAsString )
{
    SCHEMATIC* schematic = loadSchematic( wxS( "api_kitchen_sink" ) );

    API_HANDLER_SCH handler( m_context );

    auto handle = [&]( const auto& aCommand, auto& aResponse )
    {
        kiapi::common::ApiRequest request;
        request.mutable_header()->set_client_name( "kicad.qa" );
        BOOST_REQUIRE( request.mutable_message()->PackFrom( aCommand ) );

        API_RESULT result = handler.Handle( request );
        BOOST_REQUIRE_MESSAGE( result.has_value(), "request failed: " << result.error().error_message() );
        BOOST_REQUIRE( result->message().UnpackTo( &aResponse ) );
    };

    SCH_SHEET_PATH rootPath = schematic->Hierarchy().at( 0 );
    SCH_SCREEN*    screen = rootPath.LastScreen();
    SCH_SYMBOL*    symbol = nullptr;

    for( SCH_ITEM* item : screen->Items().OfType( SCH_SYMBOL_T ) )
    {
        symbol = static_cast<SCH_SYMBOL*>( item );
        break;
    }

    BOOST_REQUIRE( symbol );

    auto countSymbols = [&]()
    {
        size_t count = 0;

        for( SCH_ITEM* item : screen->Items().OfType( SCH_SYMBOL_T ) )
        {
            ignore_unused( item );
            ++count;
        }

        return count;
    };

    size_t symbolsBefore = countSymbols();
    size_t libSymbolsBefore = screen->GetLibSymbols().size();

    // The whole sheet, as it would be written to disk
    kiapi::common::commands::SaveDocumentToString saveDocument;
    *saveDocument.mutable_document() = makeDocument( *schematic );

    kiapi::common::commands::SavedDocumentResponse savedDocument;
    handle( saveDocument, savedDocument );
    BOOST_CHECK( savedDocument.contents().starts_with( "(kicad_sch" ) );
    BOOST_CHECK( savedDocument.contents().find( "(lib_symbols" ) != std::string::npos );

    // One symbol in the clipboard format
    kiapi::common::commands::SaveItemsToString saveItems;
    *saveItems.mutable_header()->mutable_document() = makeDocument( *schematic );
    saveItems.add_items()->set_value( symbol->m_Uuid.AsStdString() );

    kiapi::common::commands::SavedSelectionResponse savedItems;
    handle( saveItems, savedItems );
    BOOST_REQUIRE_EQUAL( savedItems.ids_size(), 1 );
    BOOST_CHECK( savedItems.contents().find( "(lib_symbols" ) != std::string::npos );

    kiapi::common::commands::ParseAndCreateItemsFromString paste;
    *paste.mutable_document() = makeDocument( *schematic );
    paste.set_contents( savedItems.contents() );

    kiapi::common::commands::CreateItemsResponse created;
    handle( paste, created );

    BOOST_REQUIRE_EQUAL( created.created_items_size(), 1 );
    BOOST_CHECK_EQUAL( created.created_items( 0 ).status().code(), kiapi::common::commands::ISC_OK );
    BOOST_CHECK_EQUAL( countSymbols(), symbolsBefore + 1 );
    BOOST_CHECK_EQUAL( screen->GetLibSymbols().size(), libSymbolsBefore );

    kiapi::schematic::types::SchematicSymbolInstance pasted;
    BOOST_REQUIRE( created.created_items( 0 ).item().UnpackTo( &pasted ) );
    BOOST_CHECK_NE( pasted.id().value(), symbol->m_Uuid.AsStdString() );
    BOOST_CHECK_EQUAL( pasted.definition().id().entry_name(), symbol->GetLibId().GetUniStringLibItemName().ToStdString() );

    // The copy has pins of its own
    int pins = 0;

    for( const kiapi::schematic::types::SchematicSymbolChild& child : pasted.definition().items() )
    {
        if( child.item().Is<kiapi::schematic::types::SchematicPin>() )
            ++pins;
    }

    BOOST_CHECK_EQUAL( pins, static_cast<int>( symbol->GetPins( &rootPath ).size() ) );

    // Text that is not a schematic is a bad request
    paste.set_contents( "(nope" );
    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( paste ) );

    API_RESULT result = handler.Handle( request );
    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_BAD_REQUEST );
}


BOOST_AUTO_TEST_CASE( CustomPropertyCannotDuplicateSystemProperty )
{
    SCHEMATIC* schematic = loadSchematic( wxS( "api_kitchen_sink" ) );

    API_HANDLER_SCH handler( m_context );
    ApiRequest beginRequest = makeBeginCommitRequest( *schematic );
    BOOST_REQUIRE( handler.Handle( beginRequest ).has_value() );

    commands::CreateItems command;
    command.mutable_header()->mutable_document()->CopyFrom( makeDocument( *schematic ) );

    schematic::types::SchematicLine line;
    line.mutable_id()->set_value( "83618809-06d6-4cbe-ba0e-ec60fe58921c" );
    line.set_type( schematic::types::SchematicLineType::SLT_WIRE );
    line.add_custom_properties()->set_key( "linE Width" );
    line.add_custom_properties()->set_key( "start X" );

    command.add_items()->PackFrom( line );

    ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );

    API_RESULT result = handler.Handle( request );
    BOOST_REQUIRE( result.has_value() );

    commands::CreateItemsResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );
    BOOST_REQUIRE_EQUAL( response.created_items_size(), 1 );

    const commands::ItemStatus& status = response.created_items( 0 ).status();

    // Should be rejected; duplicates built in property
    BOOST_CHECK_EQUAL( status.code(), kiapi::common::commands::ItemStatusCode::ISC_INVALID_DATA );
    BOOST_CHECK_NE( status.error_message().find( "Invalid custom properties" ), std::string::npos );
}


BOOST_AUTO_TEST_CASE( SymbolFieldTakesPrecedenceOverCustomProperty )
{
    SCHEMATIC* schematic = loadSchematic( wxS( "api_kitchen_sink" ) );

    API_HANDLER_SCH handler( m_context );
    ApiRequest beginRequest = makeBeginCommitRequest( *schematic );
    BOOST_REQUIRE( handler.Handle( beginRequest ).has_value() );

    commands::CreateItems command;
    command.mutable_header()->mutable_document()->CopyFrom( makeDocument( *schematic ) );

    schematic::types::SchematicSymbolInstance symbol;
    symbol.mutable_id()->set_value( "f8688bac-2fcb-4184-ab0e-0cf46a139c43" );
    symbol.mutable_transform()->set_orientation( schematic::types::SchematicSymbolOrientation::SSO_0 );

    schematic::types::SchematicField* field = symbol.add_user_fields();
    field->set_name( "MPN" );
    field->mutable_text()->set_text( "123" );

    types::CustomProperty* prop = symbol.add_custom_properties();
    prop->set_key( "MPN" );
    prop->set_value( "456" );

    command.add_items()->PackFrom( symbol );

    ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( command ) );

    API_RESULT result = handler.Handle( request );
    BOOST_REQUIRE( result.has_value() );

    commands::CreateItemsResponse response;
    BOOST_REQUIRE( result->message().UnpackTo( &response ) );
    BOOST_REQUIRE_EQUAL( response.created_items_size(), 1 );

    const commands::ItemStatus& status = response.created_items( 0 ).status();

    // The field takes precedence: a custom property duplicating a field name is rejected.
    BOOST_CHECK_EQUAL( status.code(), kiapi::common::commands::ItemStatusCode::ISC_INVALID_DATA );
    BOOST_CHECK_NE( status.error_message().find( "MPN" ), std::string::npos );
}


BOOST_AUTO_TEST_CASE( RunSchematicJobErcPopulatesMarkers )
{
    SCHEMATIC* schematic = loadSchematic( wxS( "api_kitchen_sink" ) );
    API_HANDLER_SCH handler( m_context );

    auto handle = [&]( const auto& aCommand, auto& aResponse )
    {
        ApiRequest request;
        request.mutable_header()->set_client_name( "kicad.qa" );
        BOOST_REQUIRE( request.mutable_message()->PackFrom( aCommand ) );

        API_RESULT result = handler.Handle( request );
        BOOST_REQUIRE_MESSAGE( result.has_value(), "request failed: " << result.error().error_message() );
        BOOST_REQUIRE( result->message().UnpackTo( &aResponse ) );
    };

    kiapi::schematic::commands::RunSchematicJobErc run;
    *run.mutable_schematic() = makeDocument( *schematic );

    kiapi::schematic::commands::ErcResultsResponse results;
    handle( run, results );

    // Count the markers the checker left in the hierarchy
    int markersInSchematic = 0;
    SCH_SCREENS screens( schematic->Root() );

    for( SCH_SCREEN* screen = screens.GetFirst(); screen; screen = screens.GetNext() )
    {
        for( SCH_ITEM* item : screen->Items().OfType( SCH_MARKER_T ) )
        {
            ignore_unused( item );
            ++markersInSchematic;
        }
    }

    BOOST_CHECK_GT( results.markers_size(), 0 );
    BOOST_CHECK_EQUAL( results.markers_size(), markersInSchematic );
    BOOST_CHECK_EQUAL( results.error_count() + results.warning_count() + results.exclusion_count(),
                       static_cast<uint32_t>( results.markers_size() ) );

    for( const kiapi::schematic::ErcMarker& marker : results.markers() )
    {
        BOOST_CHECK( !marker.id().value().empty() );
        BOOST_CHECK( marker.severity() != kiapi::common::types::RS_UNKNOWN );
        BOOST_CHECK( !marker.description().empty() );
    }

    // A second run on an unchanged schematic finds the same violations (connectivity is rebuilt
    // before each headless run)
    kiapi::schematic::commands::ErcResultsResponse rerun;
    handle( run, rerun );
    BOOST_CHECK_EQUAL( rerun.markers_size(), results.markers_size() );
    BOOST_CHECK_EQUAL( rerun.error_count(), results.error_count() );
    BOOST_CHECK_EQUAL( rerun.warning_count(), results.warning_count() );
    results = rerun;

    // GetErcMarkers reports the same set; excluding one moves it to the exclusion count
    kiapi::schematic::commands::GetErcMarkers get;
    *get.mutable_schematic() = makeDocument( *schematic );

    kiapi::schematic::commands::ErcResultsResponse again;
    handle( get, again );
    BOOST_CHECK_EQUAL( again.markers_size(), results.markers_size() );

    kiapi::schematic::commands::SetErcMarkerExcluded exclude;
    *exclude.mutable_schematic() = makeDocument( *schematic );
    *exclude.add_markers() = results.markers( 0 ).id();
    exclude.set_excluded( true );
    exclude.set_comment( "known" );

    google::protobuf::Empty empty;
    handle( exclude, empty );
    handle( get, again );

    BOOST_CHECK_EQUAL( again.exclusion_count(), results.exclusion_count() + 1 );
    BOOST_CHECK_EQUAL( schematic->ErcSettings().m_ErcExclusions.size(), static_cast<size_t>( again.exclusion_count() ) );

    // Exclusions survive a re-run
    handle( run, results );
    BOOST_CHECK_EQUAL( results.exclusion_count(), again.exclusion_count() );

    // Severities: every rule type listed once; changing one is reflected in the settings
    kiapi::schematic::commands::GetErcSeverities getSeverities;
    *getSeverities.mutable_schematic() = makeDocument( *schematic );

    kiapi::schematic::commands::ErcSeveritiesResponse severities;
    handle( getSeverities, severities );
    BOOST_CHECK_EQUAL( severities.severities_size(), static_cast<int>( ERC_ITEM::GetItemsWithSeverities().size() ) );

    kiapi::schematic::commands::SetErcSeverities setSeverities;
    *setSeverities.mutable_schematic() = makeDocument( *schematic );
    kiapi::schematic::ErcSeveritySetting* change = setSeverities.add_severities();
    change->set_rule_type( kiapi::schematic::ERCET_PIN_NOT_CONNECTED );
    change->set_severity( kiapi::common::types::RS_IGNORE );

    handle( setSeverities, severities );
    BOOST_CHECK_EQUAL( schematic->ErcSettings().GetSeverity( ERCE_PIN_NOT_CONNECTED ), RPT_SEVERITY_IGNORE );

    change->set_severity( kiapi::common::types::RS_EXCLUSION );
    ApiRequest request;
    request.mutable_header()->set_client_name( "kicad.qa" );
    BOOST_REQUIRE( request.mutable_message()->PackFrom( setSeverities ) );
    API_RESULT result = handler.Handle( request );
    BOOST_REQUIRE( !result.has_value() );
    BOOST_CHECK_EQUAL( result.error().status(), kiapi::common::ApiStatusCode::AS_BAD_REQUEST );
}


BOOST_AUTO_TEST_SUITE_END()
