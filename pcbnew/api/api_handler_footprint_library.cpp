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

#include <api/api_handler_footprint_library.h>

#include <api/api_utils.h>
#include <footprint.h>
#include <footprint_library_adapter.h>
#include <footprint_wizard.h>
#include <ki_exception.h>
#include <libraries/library_manager.h>
#include <project_pcb.h>
#include <wildcards_and_files_ext.h>

using namespace kiapi::common::commands;
using kiapi::common::ApiStatusCode;


API_HANDLER_FOOTPRINT_LIBRARY::API_HANDLER_FOOTPRINT_LIBRARY( PROJECT* aProject ) :
        API_HANDLER_LIBRARY( LIBRARY_TABLE_TYPE::FOOTPRINT, PROJECT_PCB::FootprintLibAdapter( aProject ), aProject )
{
    registerHandler<ListWizards, ListWizardsResponse>( &API_HANDLER_FOOTPRINT_LIBRARY::handleListWizards );
    registerHandler<RunWizard, kiapi::common::types::WizardGeneratedContent>(
            &API_HANDLER_FOOTPRINT_LIBRARY::handleRunWizard );
}


API_HANDLER_FOOTPRINT_LIBRARY::~API_HANDLER_FOOTPRINT_LIBRARY()
{
}


FOOTPRINT_LIBRARY_ADAPTER* API_HANDLER_FOOTPRINT_LIBRARY::adapter() const
{
    return static_cast<FOOTPRINT_LIBRARY_ADAPTER*>( m_adapter );
}


wxString API_HANDLER_FOOTPRINT_LIBRARY::defaultLibraryExtension() const
{
    return FILEEXT::KiCadFootprintLibPathExtension;
}


void API_HANDLER_FOOTPRINT_LIBRARY::PackLibraryFootprint( kiapi::board::types::Footprint& aOut,
                                                          const FOOTPRINT& aFootprint )
{
    using namespace kiapi::board::types;

    google::protobuf::Any any;
    aFootprint.Serialize( any );

    FootprintInstance instance;
    any.UnpackTo( &instance );

    aOut = instance.definition();

    // The instance view carries what the library view is missing: the mounting style and
    // flags, the mandatory fields, and the design rule overrides
    std::string description = aOut.attributes().description();
    std::string keywords = aOut.attributes().keywords();
    *aOut.mutable_attributes() = instance.attributes();
    aOut.mutable_attributes()->set_description( description );
    aOut.mutable_attributes()->set_keywords( keywords );

    *aOut.mutable_reference_field() = instance.reference_field();
    *aOut.mutable_value_field() = instance.value_field();
    *aOut.mutable_datasheet_field() = instance.datasheet_field();
    *aOut.mutable_description_field() = instance.description_field();
    *aOut.mutable_overrides() = instance.overrides();
}


std::unique_ptr<FOOTPRINT> API_HANDLER_FOOTPRINT_LIBRARY::UnpackLibraryFootprint(
        const kiapi::board::types::Footprint& aIn )
{
    using namespace kiapi::board::types;

    FootprintInstance instance;
    *instance.mutable_definition() = aIn;
    *instance.mutable_attributes() = aIn.attributes();
    *instance.mutable_reference_field() = aIn.reference_field();
    *instance.mutable_value_field() = aIn.value_field();
    *instance.mutable_datasheet_field() = aIn.datasheet_field();
    *instance.mutable_description_field() = aIn.description_field();
    *instance.mutable_overrides() = aIn.overrides();
    instance.set_layer( BoardLayer::BL_F_Cu );

    google::protobuf::Any any;
    any.PackFrom( instance );

    std::unique_ptr<FOOTPRINT> footprint = std::make_unique<FOOTPRINT>( nullptr );

    if( !footprint->Deserialize( any ) )
        return nullptr;

    return footprint;
}


std::optional<ApiResponseStatus> API_HANDLER_FOOTPRINT_LIBRARY::ensureLibraryLoaded( const wxString& aNickname )
{
    if( aNickname.IsEmpty() )
        return badRequest( "a library nickname is required" );

    // The tables know every library; the adapter's cache only knows the loaded ones
    std::optional<LIBRARY_TABLE_ROW*> row = adapter()->GetRow( aNickname );

    if( !row || ( *row )->Disabled() )
        return badRequest( fmt::format( "no enabled footprint library named '{}'", aNickname.ToUTF8().data() ) );

    std::optional<LIB_STATUS> status = adapter()->LoadLibraryEntry( aNickname );

    if( !status || status->load_status != LOAD_STATUS::LOADED )
    {
        wxString error = status && status->error ? status->error->message : wxString( wxS( "library failed to load" ) );
        return badRequest( fmt::format( "footprint library '{}': {}", aNickname.ToUTF8().data(),
                                        error.ToUTF8().data() ) );
    }

    return std::nullopt;
}


HANDLER_RESULT<ListLibraryEntriesResponse> API_HANDLER_FOOTPRINT_LIBRARY::handleListLibraryEntries(
        const HANDLER_CONTEXT<ListLibraryEntries>& aCtx )
{
    if( std::optional<ApiResponseStatus> e = checkType( aCtx.Request.type() ) )
        return tl::unexpected( *e );

    wxString nickname = wxString::FromUTF8( aCtx.Request.nickname() );

    if( std::optional<ApiResponseStatus> e = ensureLibraryLoaded( nickname ) )
        return tl::unexpected( *e );

    wxString filter = wxString::FromUTF8( aCtx.Request.filter() ).Lower();

    auto matches =
            [&]( const wxString& aText )
            {
                return filter.IsEmpty() || aText.Lower().Contains( filter );
            };

    // The adapter's preloaded cache is filled by the background load of the whole table (editor
    // start-up); headless, the library is enumerated and each footprint loaded on demand instead.
    std::vector<FOOTPRINT*>                 footprints = adapter()->GetFootprints( nickname, true );
    std::vector<std::unique_ptr<FOOTPRINT>> loaded;

    if( footprints.empty() )
    {
        for( const wxString& name : adapter()->GetFootprintNames( nickname, true ) )
        {
            try
            {
                if( FOOTPRINT* footprint = adapter()->LoadFootprint( nickname, name, false ) )
                {
                    footprints.push_back( footprint );
                    loaded.emplace_back( footprint );
                }
            }
            catch( const IO_ERROR& )
            {
                // Skip footprints that fail to load, as the library browser does
            }
        }
    }

    ListLibraryEntriesResponse response;

    for( const FOOTPRINT* footprint : footprints )
    {
        wxString name = footprint->GetFPID().GetUniStringLibItemName();

        if( !matches( name ) && !matches( footprint->GetLibDescription() ) && !matches( footprint->GetKeywords() ) )
            continue;

        LibraryEntry* entry = response.add_entries();
        entry->mutable_id()->set_library_nickname( nickname.ToUTF8() );
        entry->mutable_id()->set_entry_name( name.ToUTF8() );
        entry->set_name( name.ToUTF8() );
        entry->set_description( footprint->GetLibDescription().ToUTF8() );
        entry->set_keywords( footprint->GetKeywords().ToUTF8() );

        FootprintEntryInfo* info = entry->mutable_footprint();
        info->set_pad_count( footprint->GetPadCount() );
        info->set_unique_pad_count( static_cast<uint32_t>( footprint->GetUniquePadNumbers().size() ) );

        if( footprint->GetAttributes() & FP_THROUGH_HOLE )
            info->set_mounting_style( kiapi::board::types::FMS_THROUGH_HOLE );
        else if( footprint->GetAttributes() & FP_SMD )
            info->set_mounting_style( kiapi::board::types::FMS_SMD );
        else
            info->set_mounting_style( kiapi::board::types::FMS_UNSPECIFIED );

        for( const FP_3DMODEL& model : footprint->Models() )
            info->add_models( model.m_Filename.ToUTF8() );
    }

    return response;
}


HANDLER_RESULT<GetLibraryItemResponse> API_HANDLER_FOOTPRINT_LIBRARY::handleGetLibraryItem(
        const HANDLER_CONTEXT<GetLibraryItem>& aCtx )
{
    if( std::optional<ApiResponseStatus> e = checkType( aCtx.Request.type() ) )
        return tl::unexpected( *e );

    LIB_ID id = kiapi::common::UnpackLibId( aCtx.Request.id() );

    if( std::optional<ApiResponseStatus> e = ensureLibraryLoaded( id.GetUniStringLibNickname() ) )
        return tl::unexpected( *e );

    std::unique_ptr<FOOTPRINT> footprint;

    try
    {
        footprint.reset( adapter()->LoadFootprint( id, true ) );
    }
    catch( const IO_ERROR& ioe )
    {
        return tl::unexpected( badRequest( fmt::format( "could not load '{}': {}", id.Format().c_str(),
                                                        ioe.What().ToUTF8().data() ) ) );
    }

    if( !footprint )
        return tl::unexpected( badRequest( fmt::format( "footprint '{}' not found", id.Format().c_str() ) ) );

    GetLibraryItemResponse response;
    kiapi::common::PackLibId( response.mutable_id(), id );
    PackLibraryFootprint( *response.mutable_item()->mutable_footprint(), *footprint );
    return response;
}


HANDLER_RESULT<SaveLibraryItemResponse> API_HANDLER_FOOTPRINT_LIBRARY::handleSaveLibraryItem(
        const HANDLER_CONTEXT<SaveLibraryItem>& aCtx )
{
    if( std::optional<ApiResponseStatus> e = checkType( aCtx.Request.type() ) )
        return tl::unexpected( *e );

    if( !aCtx.Request.item().has_footprint() )
        return tl::unexpected( badRequest( "SaveLibraryItem for LT_FOOTPRINT requires item.footprint" ) );

    wxString nickname = wxString::FromUTF8( aCtx.Request.id().library_nickname() );

    if( std::optional<ApiResponseStatus> e = ensureLibraryLoaded( nickname ) )
        return tl::unexpected( *e );

    if( !adapter()->IsFootprintLibWritable( nickname ) )
        return tl::unexpected( badRequest( fmt::format( "footprint library '{}' is read-only",
                                                        nickname.ToUTF8().data() ) ) );

    std::unique_ptr<FOOTPRINT> footprint = UnpackLibraryFootprint( aCtx.Request.item().footprint() );

    if( !footprint )
        return tl::unexpected( badRequest( "could not unpack the footprint" ) );

    wxString name = wxString::FromUTF8( aCtx.Request.id().entry_name() );

    if( name.IsEmpty() )
        name = footprint->GetFPID().GetUniStringLibItemName();

    if( name.IsEmpty() )
        return tl::unexpected( badRequest( "the footprint has no name; set id.entry_name" ) );

    LIB_ID id( nickname, name );
    footprint->SetFPID( id );

    if( !aCtx.Request.overwrite() && adapter()->FootprintExists( nickname, name ) )
        return tl::unexpected( badRequest( fmt::format( "'{}' already exists and overwrite is not set", id.Format().c_str() ) ) );

    try
    {
        if( adapter()->SaveFootprint( nickname, footprint.get(), true ) != FOOTPRINT_LIBRARY_ADAPTER::SAVE_OK )
            return tl::unexpected( badRequest( fmt::format( "'{}' was not saved", id.Format().c_str() ) ) );
    }
    catch( const IO_ERROR& ioe )
    {
        return tl::unexpected( badRequest( fmt::format( "could not save '{}': {}", id.Format().c_str(),
                                                        ioe.What().ToUTF8().data() ) ) );
    }

    SaveLibraryItemResponse response;
    kiapi::common::PackLibId( response.mutable_id(), id );
    return response;
}


HANDLER_RESULT<google::protobuf::Empty> API_HANDLER_FOOTPRINT_LIBRARY::handleDeleteLibraryItem(
        const HANDLER_CONTEXT<DeleteLibraryItem>& aCtx )
{
    if( std::optional<ApiResponseStatus> e = checkType( aCtx.Request.type() ) )
        return tl::unexpected( *e );

    LIB_ID id = kiapi::common::UnpackLibId( aCtx.Request.id() );
    wxString nickname = id.GetUniStringLibNickname();

    if( std::optional<ApiResponseStatus> e = ensureLibraryLoaded( nickname ) )
        return tl::unexpected( *e );

    if( !adapter()->IsFootprintLibWritable( nickname ) )
        return tl::unexpected( badRequest( fmt::format( "footprint library '{}' is read-only",
                                                        nickname.ToUTF8().data() ) ) );

    if( !adapter()->FootprintExists( nickname, id.GetUniStringLibItemName() ) )
        return tl::unexpected( badRequest( fmt::format( "footprint '{}' not found", id.Format().c_str() ) ) );

    try
    {
        adapter()->DeleteFootprint( nickname, id.GetUniStringLibItemName() );
    }
    catch( const IO_ERROR& ioe )
    {
        return tl::unexpected( badRequest( fmt::format( "could not delete '{}': {}", id.Format().c_str(),
                                                        ioe.What().ToUTF8().data() ) ) );
    }

    return google::protobuf::Empty();
}


HANDLER_RESULT<ListWizardsResponse> API_HANDLER_FOOTPRINT_LIBRARY::handleListWizards(
        const HANDLER_CONTEXT<ListWizards>& aCtx )
{
    ListWizardsResponse response;

#ifdef KICAD_HEADLESS_API
    // Wizards are out-of-process API plugins, and this build has no plugin manager to find
    // them with; the honest answer is that none are installed.
    return response;
#else
    if( !m_wizards )
        m_wizards = std::make_unique<FOOTPRINT_WIZARD_MANAGER>();

    m_wizards->ReloadWizards();

    for( FOOTPRINT_WIZARD* wizard : m_wizards->Wizards() )
    {
        kiapi::common::types::WizardInfo* info = response.add_wizards();
        kiapi::common::types::WizardMetaInfo* meta = info->mutable_meta();
        meta->set_identifier( wizard->Info().meta.identifier.ToUTF8() );
        meta->set_name( wizard->Info().meta.name.ToUTF8() );
        meta->set_description( wizard->Info().meta.description.ToUTF8() );

        for( kiapi::common::types::WizardContentType type : wizard->Info().meta.types_generated )
            meta->add_types_generated( type );

        for( const std::unique_ptr<WIZARD_PARAMETER>& param : wizard->Info().parameters )
            *info->add_parameters() = param->Pack( false );
    }

    return response;
#endif
}


HANDLER_RESULT<kiapi::common::types::WizardGeneratedContent> API_HANDLER_FOOTPRINT_LIBRARY::handleRunWizard(
        const HANDLER_CONTEXT<RunWizard>& aCtx )
{
    using namespace kiapi::common::types;

#ifdef KICAD_HEADLESS_API
    return tl::unexpected( badRequest( fmt::format( "no wizard '{}'; see ListWizards",
                                                    aCtx.Request.identifier() ) ) );
#else
    if( !m_wizards )
    {
        m_wizards = std::make_unique<FOOTPRINT_WIZARD_MANAGER>();
        m_wizards->ReloadWizards();
    }

    std::optional<FOOTPRINT_WIZARD*> wizard = m_wizards->GetWizard( wxString::FromUTF8( aCtx.Request.identifier() ) );

    if( !wizard )
        return tl::unexpected( badRequest( fmt::format( "no wizard '{}'; see ListWizards", aCtx.Request.identifier() ) ) );

    ( *wizard )->ResetParameters();

    for( const WizardParameter& in : aCtx.Request.parameters().parameters() )
    {
        wxString identifier = wxString::FromUTF8( in.identifier() );

        for( const std::unique_ptr<WIZARD_PARAMETER>& param : ( *wizard )->Info().parameters )
        {
            if( param->identifier != identifier )
                continue;

            if( auto* p = dynamic_cast<WIZARD_INT_PARAMETER*>( param.get() ) )
                p->value = in.int_().value();
            else if( auto* p = dynamic_cast<WIZARD_REAL_PARAMETER*>( param.get() ) )
                p->value = in.real().value();
            else if( auto* p = dynamic_cast<WIZARD_BOOL_PARAMETER*>( param.get() ) )
                p->value = in.bool_().value();
            else if( auto* p = dynamic_cast<WIZARD_STRING_PARAMETER*>( param.get() ) )
                p->value = wxString::FromUTF8( in.string().value() );
            else if( auto* p = dynamic_cast<WIZARD_ENUM_PARAMETER*>( param.get() ) )
                p->value = in.enum_().value();
        }
    }

    tl::expected<FOOTPRINT*, wxString> generated = m_wizards->Generate( *wizard );

    WizardGeneratedContent response;

    if( !generated )
    {
        response.set_status( WGS_ERROR );
        response.set_error_message( generated.error().ToUTF8() );
        return response;
    }

    std::unique_ptr<FOOTPRINT> footprint( *generated );
    kiapi::board::types::Footprint packed;
    PackLibraryFootprint( packed, *footprint );

    response.set_status( WGS_OK );
    response.mutable_content()->PackFrom( packed );
    return response;
#endif
}
