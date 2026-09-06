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

#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include <boost/test/unit_test.hpp>
#include <wx/filename.h>

#include "api_e2e_utils.h"

#include <api/board/board_jobs.pb.h>
#include <api/common/commands/base_commands.pb.h>
#include <api/common/commands/editor_commands.pb.h>
#include <wx/utils.h>
#include <api/board/board_types.pb.h>
#include <api/schematic/schematic_jobs.pb.h>


/**
 * Compare two text files line-by-line, optionally skipping the first @a aSkipLines lines of each
 * file (to ignore timestamps / version headers).  Returns true if files are identical after the
 * skipped prefix, false otherwise.  On mismatch the first differing line pair is logged via
 * BOOST_TEST_MESSAGE.
 */
bool textFilesMatch( const wxString& aGoldenPath, const wxString& aGeneratedPath, int aSkipLines )
{
    std::ifstream goldenStream( aGoldenPath.ToStdString() );
    std::ifstream generatedStream( aGeneratedPath.ToStdString() );

    if( !goldenStream.is_open() )
    {
        BOOST_TEST_MESSAGE( "Cannot open golden file: " + aGoldenPath );
        return false;
    }

    if( !generatedStream.is_open() )
    {
        BOOST_TEST_MESSAGE( "Cannot open generated file: " + aGeneratedPath );
        return false;
    }

    std::string goldenLine, generatedLine;

    for( int i = 0; i < aSkipLines; ++i )
    {
        std::getline( goldenStream, goldenLine );
        std::getline( generatedStream, generatedLine );
    }

    int lineNum = aSkipLines + 1;

    while( std::getline( goldenStream, goldenLine ) )
    {
        if( !std::getline( generatedStream, generatedLine ) )
        {
            BOOST_TEST_MESSAGE( "Generated file is shorter than golden at line " << lineNum );
            return false;
        }

        if( goldenLine != generatedLine )
        {
            BOOST_TEST_MESSAGE( "Mismatch at line " << lineNum << ":\n"
                                                    << "  golden:    " << goldenLine << "\n"
                                                    << "  generated: " << generatedLine );
            return false;
        }

        ++lineNum;
    }

    if( std::getline( generatedStream, generatedLine ) )
    {
        BOOST_TEST_MESSAGE( "Generated file is longer than golden after line " << lineNum );
        return false;
    }

    return true;
}


/**
 * Read @a aPath into a string, or return an empty string if it cannot be opened.
 */
static std::string readFile( const wxString& aPath )
{
    std::ifstream stream( aPath.ToStdString(), std::ios::binary );

    if( !stream.is_open() )
        return {};

    std::ostringstream contents;
    contents << stream.rdbuf();
    return contents.str();
}


static size_t countOccurrences( const std::string& aHaystack, const std::string& aNeedle )
{
    size_t count = 0;

    for( size_t pos = aHaystack.find( aNeedle ); pos != std::string::npos;
         pos = aHaystack.find( aNeedle, pos + aNeedle.size() ) )
    {
        ++count;
    }

    return count;
}


/**
 * Return every distinct #RRGGBB literal appearing in @a aSvg.
 */
static std::set<std::string> collectColours( const std::string& aSvg )
{
    std::set<std::string> colours;

    for( size_t pos = aSvg.find( '#' ); pos != std::string::npos; pos = aSvg.find( '#', pos + 1 ) )
    {
        std::string colour = aSvg.substr( pos, 7 );

        if( colour.size() < 7 )
            continue;

        if( colour.find_first_not_of( "0123456789ABCDEFabcdef", 1 ) != std::string::npos )
            continue;

        colours.insert( colour );
    }

    return colours;
}


BOOST_AUTO_TEST_SUITE( ApiJobs )


BOOST_FIXTURE_TEST_CASE( ExportBoardSvg, API_SERVER_E2E_FIXTURE )
{
    BOOST_REQUIRE_MESSAGE( Start(), LastError() );

    wxString testDataDir =
            wxString::FromUTF8( KI_TEST::GetTestDataRootDir() ) + wxS( "cli/artwork_generation_regressions/" );

    wxFileName boardPath( testDataDir, wxS( "ZoneFill-4.0.7.kicad_pcb" ) );

    kiapi::common::types::DocumentSpecifier document;

    BOOST_REQUIRE_MESSAGE( Client().OpenDocument( boardPath.GetFullPath(), &document ),
                           "OpenDocument failed: " + Client().LastError() );

    wxString   tempFile = wxFileName::CreateTempFileName( wxS( "api_job_svg_" ) );
    wxFileName outputPath( tempFile );
    outputPath.SetExt( wxS( "svg" ) );

    kiapi::board::jobs::RunBoardJobExportSvg request;
    *request.mutable_job_settings()->mutable_document() = document;
    request.mutable_job_settings()->set_output_path( outputPath.GetFullPath().ToUTF8().data() );

    request.mutable_plot_settings()->add_layers( kiapi::board::types::BL_F_Cu );
    request.mutable_plot_settings()->set_black_and_white( true );
    request.mutable_plot_settings()->set_plot_drawing_sheet( false );
    request.mutable_plot_settings()->set_drill_marks( kiapi::board::jobs::PDM_FULL );
    request.set_page_mode( kiapi::board::jobs::BJPM_EACH_LAYER_OWN_FILE );
    request.set_precision( 4 );

    kiapi::common::types::RunJobResponse response;
    BOOST_REQUIRE_MESSAGE( Client().RunJob( request, &response ), "RunJob failed: " + Client().LastError() );

    BOOST_REQUIRE_MESSAGE( response.status() == kiapi::common::types::JS_SUCCESS,
                           "Job failed: " + wxString::FromUTF8( response.message() ) );

    BOOST_REQUIRE_MESSAGE( response.output_path_size() > 0, "Job returned no output paths" );

    wxString generatedPath = wxString::FromUTF8( response.output_path( 0 ) );
    BOOST_REQUIRE_MESSAGE( wxFileName::FileExists( generatedPath ), "Generated SVG does not exist: " + generatedPath );

    // Plot fidelity is covered by the kicad-cli SVG regression tests, which rasterize and compare
    // against golden artwork.  All this test has to establish is that the job honoured the request.
    const std::string svg = readFile( generatedPath );

    BOOST_REQUIRE_MESSAGE( !svg.empty(), "Generated SVG is empty or unreadable: " + generatedPath );
    BOOST_CHECK( svg.find( "<svg" ) != std::string::npos );
    BOOST_CHECK( svg.find( "</svg>" ) != std::string::npos );

    // The board page is A4, and the four decimals are the requested precision
    BOOST_CHECK( svg.find( "width=\"297.0022mm\" height=\"210.0072mm\"" ) != std::string::npos );

    std::set<std::string> colours = collectColours( svg );
    BOOST_REQUIRE_MESSAGE( !colours.empty(), "Plot declares no colours at all" );

    for( const std::string& colour : colours )
    {
        BOOST_CHECK_MESSAGE( colour == "#000000" || colour == "#FFFFFF",
                             "black_and_white was requested but the plot contains " + colour );
    }

    // F.Cu of this board is dense; a near-empty plot means the layer selection was dropped
    BOOST_CHECK_GT( countOccurrences( svg, "<path" ), 100u );

    if( wxFileName::FileExists( generatedPath ) )
        wxRemoveFile( generatedPath );

    // A per-layer export turns the requested output path into a directory
    if( wxFileName::DirExists( outputPath.GetFullPath() ) )
        wxFileName::Rmdir( outputPath.GetFullPath(), wxPATH_RMDIR_RECURSIVE );
    else if( wxFileName::FileExists( outputPath.GetFullPath() ) )
        wxRemoveFile( outputPath.GetFullPath() );

    wxRemoveFile( tempFile );
}


BOOST_FIXTURE_TEST_CASE( ExportBoardDrill, API_SERVER_E2E_FIXTURE )
{
    BOOST_REQUIRE_MESSAGE( Start(), LastError() );

    wxString testDataDir = wxString::FromUTF8( KI_TEST::GetTestDataRootDir() ) + wxS( "cli/basic_test/" );

    wxFileName boardPath( testDataDir, wxS( "basic_test.kicad_pcb" ) );

    kiapi::common::types::DocumentSpecifier document;

    BOOST_REQUIRE_MESSAGE( Client().OpenDocument( boardPath.GetFullPath(), &document ),
                           "OpenDocument failed: " + Client().LastError() );

    wxString outputDir = wxFileName::GetTempDir() + wxFileName::GetPathSeparator() + wxS( "api_job_drill_" )
                         + wxString::Format( "%ld", wxGetProcessId() ) + wxFileName::GetPathSeparator();
    wxFileName::Mkdir( outputDir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL );

    kiapi::board::jobs::RunBoardJobExportDrill request;
    *request.mutable_job_settings()->mutable_document() = document;
    request.mutable_job_settings()->set_output_path( outputDir.ToUTF8().data() );
    request.set_format( kiapi::board::jobs::DF_EXCELLON );

    kiapi::common::types::RunJobResponse response;
    BOOST_REQUIRE_MESSAGE( Client().RunJob( request, &response ), "RunJob failed: " + Client().LastError() );

    BOOST_REQUIRE_MESSAGE( response.status() == kiapi::common::types::JS_SUCCESS,
                           "Job failed: " + wxString::FromUTF8( response.message() ) );

    BOOST_REQUIRE_MESSAGE( response.output_path_size() > 0, "Job returned no output paths" );
    wxString generatedDrillPath;

    for( int i = 0; i < response.output_path_size(); ++i )
    {
        wxString outputPath = wxString::FromUTF8( response.output_path( i ) );

        if( outputPath.EndsWith( wxS( ".drl" ) ) )
        {
            generatedDrillPath = outputPath;
            break;
        }
    }

    BOOST_REQUIRE_MESSAGE( !generatedDrillPath.IsEmpty(), "No .drl file found in job output" );
    BOOST_REQUIRE_MESSAGE( wxFileName::FileExists( generatedDrillPath ),
                           "Generated drill file does not exist: " + generatedDrillPath );

    wxString goldenPath = testDataDir + wxS( "basic_test_excellon_inches.drl" );
    BOOST_CHECK_MESSAGE( textFilesMatch( goldenPath, generatedDrillPath, 5 ),
                         "Drill output does not match golden file" );

    wxFileName::Rmdir( outputDir, wxPATH_RMDIR_RECURSIVE );
}


BOOST_FIXTURE_TEST_CASE( ExportSchematicNetlist, API_SERVER_E2E_FIXTURE )
{
    BOOST_REQUIRE_MESSAGE( Start(), LastError() );

    wxString testDataDir = wxString::FromUTF8( KI_TEST::GetTestDataRootDir() ) + wxS( "cli/basic_test/" );

    wxFileName schPath( testDataDir, wxS( "basic_test.kicad_sch" ) );

    kiapi::common::types::DocumentSpecifier document;

    BOOST_REQUIRE_MESSAGE(
            Client().OpenDocument( schPath.GetFullPath(), kiapi::common::types::DOCTYPE_SCHEMATIC, &document ),
            "OpenDocument failed: " + Client().LastError() );

    wxFileName outputPath = wxFileName::CreateTempFileName( wxS( "api_job_netlist_" ) );
    outputPath.SetExt( wxS( "cadstar" ) );

    kiapi::schematic::jobs::RunSchematicJobExportNetlist request;
    *request.mutable_job_settings()->mutable_document() = document;
    request.mutable_job_settings()->set_output_path( outputPath.GetFullPath().ToUTF8().data() );
    request.set_format( kiapi::schematic::jobs::SNF_CADSTAR );

    kiapi::common::types::RunJobResponse response;
    BOOST_REQUIRE_MESSAGE( Client().RunJob( request, &response ), "RunJob failed: " + Client().LastError() );

    BOOST_REQUIRE_MESSAGE( response.status() == kiapi::common::types::JS_SUCCESS,
                           "Job failed: " + wxString::FromUTF8( response.message() ) );

    BOOST_REQUIRE_MESSAGE( response.output_path_size() > 0, "Job returned no output paths" );

    wxString generatedPath = wxString::FromUTF8( response.output_path( 0 ) );
    BOOST_REQUIRE_MESSAGE( wxFileName::FileExists( generatedPath ),
                           "Generated netlist does not exist: " + generatedPath );

    // 3 header lines to skip (contain timestamp/version)
    wxString goldenPath = testDataDir + wxS( "basic_test.netlist.cadstar" );
    BOOST_CHECK_MESSAGE( textFilesMatch( goldenPath, generatedPath, 3 ), "Netlist output does not match golden file" );

    // Clean up
    if( wxFileName::FileExists( generatedPath ) )
        wxRemoveFile( generatedPath );

    if( wxFileName::FileExists( outputPath.GetFullPath() ) )
        wxRemoveFile( outputPath.GetFullPath() );
}


BOOST_FIXTURE_TEST_CASE( ExportSchematicBom, API_SERVER_E2E_FIXTURE )
{
    BOOST_REQUIRE_MESSAGE( Start(), LastError() );

    wxString testDataDir = wxString::FromUTF8( KI_TEST::GetTestDataRootDir() ) + wxS( "cli/variants/" );

    wxFileName schPath( testDataDir, wxS( "variants.kicad_sch" ) );

    kiapi::common::types::DocumentSpecifier document;

    BOOST_REQUIRE_MESSAGE(
            Client().OpenDocument( schPath.GetFullPath(), kiapi::common::types::DOCTYPE_SCHEMATIC, &document ),
            "OpenDocument failed: " + Client().LastError() );

    wxFileName outputPath = wxFileName::CreateTempFileName( wxS( "api_job_bom_" ) );
    outputPath.SetExt( wxS( "csv" ) );

    kiapi::schematic::jobs::RunSchematicJobExportBOM request;
    *request.mutable_job_settings()->mutable_document() = document;
    request.mutable_job_settings()->set_output_path( outputPath.GetFullPath().ToUTF8().data() );
    request.set_exclude_dnp( true );

    request.mutable_format()->set_preset_name( "CSV" );

    auto* refField = request.mutable_fields()->add_fields();
    refField->set_name( "Reference" );
    refField->set_label( "Refs" );
    refField->set_group_by( false );

    auto* valField = request.mutable_fields()->add_fields();
    valField->set_name( "Value" );
    valField->set_label( "Value" );
    valField->set_group_by( false );

    kiapi::common::types::RunJobResponse response;
    BOOST_REQUIRE_MESSAGE( Client().RunJob( request, &response ), "RunJob failed: " + Client().LastError() );

    BOOST_REQUIRE_MESSAGE( response.status() == kiapi::common::types::JS_SUCCESS,
                           "Job failed: " + wxString::FromUTF8( response.message() ) );

    BOOST_REQUIRE_MESSAGE( response.output_path_size() > 0, "Job returned no output paths" );

    wxString generatedPath = wxString::FromUTF8( response.output_path( 0 ) );
    BOOST_REQUIRE_MESSAGE( wxFileName::FileExists( generatedPath ), "Generated BOM does not exist: " + generatedPath );

    wxString goldenPath = testDataDir + wxS( "variants_default.bom.csv" );
    BOOST_CHECK_MESSAGE( textFilesMatch( goldenPath, generatedPath, 0 ), "BOM output does not match golden file" );

    request.mutable_format()->clear_preset_name();
    request.mutable_format()->set_field_delimiter( "," );
    request.mutable_format()->set_string_delimiter( "\"" );
    request.mutable_format()->set_ref_delimiter( "," );
    request.mutable_format()->set_ref_range_delimiter( "" );
    request.mutable_format()->set_include_byte_order_mark( true );
    response.Clear();

    BOOST_REQUIRE_MESSAGE( Client().RunJob( request, &response ),
                           "BOM job with byte order mark failed: " + Client().LastError() );

    BOOST_REQUIRE_MESSAGE( response.status() == kiapi::common::types::JS_SUCCESS,
                           "BOM job with byte order mark failed: " + wxString::FromUTF8( response.message() ) );
    BOOST_REQUIRE_MESSAGE( response.output_path_size() > 0, "BOM job with byte order mark returned no output paths" );

    generatedPath = wxString::FromUTF8( response.output_path( 0 ) );
    std::ifstream generatedStream( generatedPath.ToStdString(), std::ios::binary );
    std::ifstream goldenStream( goldenPath.ToStdString(), std::ios::binary );
    BOOST_REQUIRE( generatedStream.is_open() );
    BOOST_REQUIRE( goldenStream.is_open() );

    std::ostringstream generatedContents;
    std::ostringstream goldenContents;
    generatedContents << generatedStream.rdbuf();
    goldenContents << goldenStream.rdbuf();

    const std::string utf8ByteOrderMark( "\xEF\xBB\xBF", 3 );
    const std::string bom = generatedContents.str();
    BOOST_REQUIRE_GE( bom.size(), utf8ByteOrderMark.size() );
    BOOST_CHECK_EQUAL( bom.substr( 0, utf8ByteOrderMark.size() ), utf8ByteOrderMark );
    BOOST_CHECK_EQUAL( bom.substr( utf8ByteOrderMark.size() ), goldenContents.str() );

    generatedStream.close();
    goldenStream.close();

    if( wxFileName::FileExists( generatedPath ) )
        wxRemoveFile( generatedPath );

    if( wxFileName::FileExists( outputPath.GetFullPath() ) )
        wxRemoveFile( outputPath.GetFullPath() );
}


// Since 11.0: a request that names neither a preset nor any field exports the columns
// `kicad-cli sch export bom` defaults to, rather than a BOM with no columns at all
BOOST_FIXTURE_TEST_CASE( ExportSchematicBomWithoutFieldSettings, API_SERVER_E2E_FIXTURE )
{
    BOOST_REQUIRE_MESSAGE( Start(), LastError() );

    wxString testDataDir = wxString::FromUTF8( KI_TEST::GetTestDataRootDir() ) + wxS( "cli/variants/" );

    wxFileName schPath( testDataDir, wxS( "variants.kicad_sch" ) );

    kiapi::common::types::DocumentSpecifier document;

    BOOST_REQUIRE_MESSAGE(
            Client().OpenDocument( schPath.GetFullPath(), kiapi::common::types::DOCTYPE_SCHEMATIC, &document ),
            "OpenDocument failed: " + Client().LastError() );

    wxFileName outputPath = wxFileName::CreateTempFileName( wxS( "api_job_bom_defaults_" ) );
    outputPath.SetExt( wxS( "csv" ) );

    kiapi::schematic::jobs::RunSchematicJobExportBOM request;
    *request.mutable_job_settings()->mutable_document() = document;
    request.mutable_job_settings()->set_output_path( outputPath.GetFullPath().ToUTF8().data() );

    kiapi::common::types::RunJobResponse response;
    BOOST_REQUIRE_MESSAGE( Client().RunJob( request, &response ), "RunJob failed: " + Client().LastError() );

    BOOST_REQUIRE_MESSAGE( response.status() == kiapi::common::types::JS_SUCCESS,
                           "Job failed: " + wxString::FromUTF8( response.message() ) );
    BOOST_REQUIRE_MESSAGE( response.output_path_size() > 0, "Job returned no output paths" );

    wxString      generatedPath = wxString::FromUTF8( response.output_path( 0 ) );
    std::ifstream generatedStream( generatedPath.ToStdString() );
    BOOST_REQUIRE_MESSAGE( generatedStream.is_open(), "Generated BOM does not exist: " + generatedPath );

    std::string header;
    BOOST_REQUIRE( std::getline( generatedStream, header ) );

    if( !header.empty() && header.back() == '\r' )
        header.pop_back();

    BOOST_CHECK_EQUAL( header, "\"Refs\",\"Value\",\"Footprint\",\"Qty\",\"DNP\"" );

    size_t      rows = 0;
    std::string line;

    while( std::getline( generatedStream, line ) )
    {
        if( !line.empty() && line != "\r" )
            rows++;
    }

    // The same symbols the golden file of ExportSchematicBom lists
    BOOST_CHECK_EQUAL( rows, 56 );

    generatedStream.close();

    if( wxFileName::FileExists( generatedPath ) )
        wxRemoveFile( generatedPath );

    if( wxFileName::FileExists( outputPath.GetFullPath() ) )
        wxRemoveFile( outputPath.GetFullPath() );
}


// Since 11.0: RunJobSettings.async queues the job and answers JS_RUNNING with a job id;
// GetJobStatus reports it finished with the result, and return_inline carries the output bytes.
BOOST_FIXTURE_TEST_CASE( ExportBoardSvgAsyncWithInlineOutput, API_SERVER_E2E_FIXTURE )
{
    BOOST_REQUIRE_MESSAGE( Start(), LastError() );

    wxString testDataDir =
            wxString::FromUTF8( KI_TEST::GetTestDataRootDir() ) + wxS( "cli/artwork_generation_regressions/" );

    wxFileName boardPath( testDataDir, wxS( "ZoneFill-4.0.7.kicad_pcb" ) );

    kiapi::common::types::DocumentSpecifier document;

    BOOST_REQUIRE_MESSAGE( Client().OpenDocument( boardPath.GetFullPath(), &document ),
                           "OpenDocument failed: " + Client().LastError() );

    wxString   tempFile = wxFileName::CreateTempFileName( wxS( "api_job_async_svg_" ) );
    wxFileName outputPath( tempFile );
    outputPath.SetExt( wxS( "svg" ) );

    kiapi::board::jobs::RunBoardJobExportSvg request;
    *request.mutable_job_settings()->mutable_document() = document;
    request.mutable_job_settings()->set_output_path( outputPath.GetFullPath().ToUTF8().data() );
    request.mutable_job_settings()->set_async( true );
    request.mutable_job_settings()->set_return_inline( true );
    request.mutable_plot_settings()->add_layers( kiapi::board::types::BL_F_Cu );
    request.set_page_mode( kiapi::board::jobs::BJPM_ALL_LAYERS_ONE_PAGE );

    kiapi::common::types::RunJobResponse started;
    BOOST_REQUIRE_MESSAGE( Client().RunJob( request, &started ), "RunJob failed: " + Client().LastError() );
    BOOST_CHECK_EQUAL( started.status(), kiapi::common::types::JS_RUNNING );
    BOOST_REQUIRE( !started.job_id().empty() );

    kiapi::common::commands::GetJobStatus     statusRequest;
    kiapi::common::commands::GetJobStatusResponse status;
    statusRequest.set_job_id( started.job_id() );

    // The export takes well under a second; give a loaded machine much longer
    for( int attempt = 0; attempt < 600; ++attempt )
    {
        kiapi::common::ApiResponse response;
        BOOST_REQUIRE( Client().SendCommand( statusRequest, &response ) );
        BOOST_REQUIRE_MESSAGE( response.status().status() == kiapi::common::AS_OK, response.status().error_message() );
        BOOST_REQUIRE( response.message().UnpackTo( &status ) );

        if( status.state() == kiapi::common::commands::JOB_STATE_FINISHED )
            break;

        wxMilliSleep( 50 );
    }

    BOOST_REQUIRE_EQUAL( status.state(), kiapi::common::commands::JOB_STATE_FINISHED );
    BOOST_CHECK_EQUAL( status.percent(), 100 );
    BOOST_CHECK_EQUAL( status.job_id(), started.job_id() );
    BOOST_REQUIRE_MESSAGE( status.result().status() == kiapi::common::types::JS_SUCCESS,
                           "Job failed: " + wxString::FromUTF8( status.result().message() ) );
    BOOST_REQUIRE_GT( status.result().output_path_size(), 0 );
    BOOST_REQUIRE_EQUAL( status.result().inline_outputs_size(), 1 );

    const std::string& svg = status.result().inline_outputs( 0 ).data();
    BOOST_CHECK_EQUAL( status.result().inline_outputs( 0 ).path(), status.result().output_path( 0 ) );
    BOOST_CHECK( svg.find( "<svg" ) != std::string::npos );
    BOOST_CHECK_EQUAL( svg, readFile( wxString::FromUTF8( status.result().output_path( 0 ) ) ) );

    // An unknown id is a bad request
    statusRequest.set_job_id( "not-a-job" );
    kiapi::common::ApiResponse response;
    BOOST_REQUIRE( Client().SendCommand( statusRequest, &response ) );
    BOOST_CHECK_EQUAL( response.status().status(), kiapi::common::AS_BAD_REQUEST );

    for( const std::string& path : status.result().output_path() )
    {
        if( wxFileName::FileExists( wxString::FromUTF8( path ) ) )
            wxRemoveFile( wxString::FromUTF8( path ) );
    }

    if( wxFileName::DirExists( outputPath.GetFullPath() ) )
        wxFileName::Rmdir( outputPath.GetFullPath(), wxPATH_RMDIR_RECURSIVE );
    else if( wxFileName::FileExists( outputPath.GetFullPath() ) )
        wxRemoveFile( outputPath.GetFullPath() );

    wxRemoveFile( tempFile );
}


/// RunBoardJobExportSpecctra writes a DSN design file for an external autorouter, and
/// return_inline hands its text back to the client.
BOOST_FIXTURE_TEST_CASE( ExportBoardSpecctra, API_SERVER_E2E_FIXTURE )
{
    BOOST_REQUIRE_MESSAGE( Start(), LastError() );

    wxFileName boardPath( wxString::FromUTF8( KI_TEST::GetPcbnewTestDataDir() ), wxS( "api_kitchen_sink.kicad_pcb" ) );

    kiapi::common::types::DocumentSpecifier document;

    BOOST_REQUIRE_MESSAGE( Client().OpenDocument( boardPath.GetFullPath(), &document ),
                           "OpenDocument failed: " + Client().LastError() );

    auto revision =
            [&]() -> uint64_t
            {
                kiapi::common::commands::GetDocumentRevision request;
                *request.mutable_document() = document;

                kiapi::common::ApiResponse                        response;
                kiapi::common::commands::DocumentRevisionResponse result;

                if( !Client().SendCommand( request, &response ) || !response.message().UnpackTo( &result ) )
                    return 0;

                return result.revision();
            };

    const uint64_t revisionBefore = revision();

    wxString   tempFile = wxFileName::CreateTempFileName( wxS( "api_job_specctra_" ) );
    wxFileName outputPath( tempFile );
    outputPath.SetExt( wxS( "dsn" ) );

    kiapi::board::jobs::RunBoardJobExportSpecctra request;
    *request.mutable_job_settings()->mutable_document() = document;
    request.mutable_job_settings()->set_output_path( outputPath.GetFullPath().ToUTF8().data() );
    request.mutable_job_settings()->set_return_inline( true );

    kiapi::common::types::RunJobResponse response;
    BOOST_REQUIRE_MESSAGE( Client().RunJob( request, &response ), "RunJob failed: " + Client().LastError() );
    BOOST_REQUIRE_MESSAGE( response.status() == kiapi::common::types::JS_SUCCESS,
                           "Job failed: " + wxString::FromUTF8( response.message() ) );
    BOOST_REQUIRE_EQUAL( response.output_path_size(), 1 );
    BOOST_CHECK_EQUAL( response.output_path( 0 ), outputPath.GetFullPath().ToUTF8().data() );
    BOOST_REQUIRE( wxFileName::FileExists( outputPath.GetFullPath() ) );

    BOOST_REQUIRE_EQUAL( response.inline_outputs_size(), 1 );
    const std::string& dsn = response.inline_outputs( 0 ).data();
    BOOST_CHECK_EQUAL( response.inline_outputs( 0 ).path(), response.output_path( 0 ) );
    BOOST_CHECK_EQUAL( dsn, readFile( outputPath.GetFullPath() ) );

    // The design carries the board's copper layers, outline, footprints and nets, in micrometers
    BOOST_CHECK_EQUAL( dsn.rfind( "(pcb ", 0 ), 0u );
    BOOST_CHECK( dsn.find( "(resolution um 10)" ) != std::string::npos );
    BOOST_CHECK( dsn.find( "(layer F.Cu" ) != std::string::npos );
    BOOST_CHECK( dsn.find( "(layer B.Cu" ) != std::string::npos );
    BOOST_CHECK( dsn.find( "(boundary" ) != std::string::npos );
    BOOST_CHECK( dsn.find( "(placement" ) != std::string::npos );
    BOOST_CHECK( dsn.find( "(network" ) != std::string::npos );
    BOOST_CHECK( dsn.find( "(net A" ) != std::string::npos );
    BOOST_CHECK( dsn.find( "(wiring" ) != std::string::npos );

    // The export flips the back-side footprints while it writes and flips them back; it is not
    // a change to the document
    BOOST_CHECK_EQUAL( revision(), revisionBefore );

    if( wxFileName::FileExists( outputPath.GetFullPath() ) )
        wxRemoveFile( outputPath.GetFullPath() );

    wxRemoveFile( tempFile );
}


BOOST_AUTO_TEST_SUITE_END()
