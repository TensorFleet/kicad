/*
 * WebAssembly dependency smoke test for the FabPlane PCB / KiCad wasm port.
 *
 * Proves, inside a single wasm module linked exactly the way the KiCad headless
 * API core will be linked:
 *
 *   (a) wxBase works: wxString / wxFileName / wxFFile round-trip under MEMFS,
 *       plus wxAppConsole + wxEntryStart() + wxTheApp->OnInit(), wxXml,
 *       wxRegEx, wxFileConfig, wxDateTime, wxStringTokenizer, wxDir,
 *       wxStandardPaths and wxMemoryInputStream/wxZipInputStream.
 *   (b) protobuf works: generated code from the HOST protoc compiles and runs
 *       against the wasm libprotobuf (the runtime version check is the point).
 *   (c) FreeType + HarfBuzz work: open a face from an embedded font, shape a
 *       word, read back glyph ids and advances.
 *   (d) zstd works: compress and decompress a buffer.
 *
 * Every "OK" line printed must appear for the test to pass; check_wasm.mjs
 * asserts on them.
 */

#include <clocale>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ---- wxWidgets (base only) -------------------------------------------------
#include <wx/app.h>
#include <wx/init.h>
#include <wx/string.h>
#include <wx/filename.h>
#include <wx/ffile.h>
#include <wx/file.h>
#include <wx/dir.h>
#include <wx/stdpaths.h>
#include <wx/textfile.h>
#include <wx/tokenzr.h>
#include <wx/datetime.h>
#include <wx/intl.h>
#include <wx/translation.h>
#include <wx/xml/xml.h>
#include <wx/sstream.h>
#include <wx/mstream.h>
#include <wx/zipstrm.h>
#include <wx/zstream.h>
#include <wx/regex.h>
#include <wx/fileconf.h>
#include <wx/cmdline.h>
#include <wx/log.h>

// ---- protobuf --------------------------------------------------------------
#include "smoke.pb.h"
#include <google/protobuf/stubs/common.h>

// ---- FreeType + HarfBuzz ---------------------------------------------------
#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>
#include <hb-ft.h>

// ---- zstd ------------------------------------------------------------------
#include <zstd.h>


static int g_failures = 0;

static void check( bool aCond, const char* aWhat )
{
    if( aCond )
    {
        std::printf( "OK   %s\n", aWhat );
    }
    else
    {
        std::printf( "FAIL %s\n", aWhat );
        ++g_failures;
    }
}


/// The console app object KiCad's headless host will need.
class SMOKE_APP : public wxAppConsole
{
public:
    bool OnInit() override
    {
        m_onInitRan = true;
        return true;
    }

    int OnRun() override { return 0; }

    bool m_onInitRan = false;
};

wxIMPLEMENT_APP_NO_MAIN( SMOKE_APP );


// ---------------------------------------------------------------------------
// (a) wxBase
// ---------------------------------------------------------------------------
static void test_wx_app( int argc, char** argv )
{
    wxApp::SetInstance( new SMOKE_APP() );

    bool started = wxEntryStart( argc, argv );
    check( started, "wxEntryStart" );
    check( wxTheApp != nullptr, "wxTheApp non-null" );

    if( wxTheApp )
    {
        bool inited = wxTheApp->CallOnInit();
        check( inited, "wxTheApp->OnInit" );
        check( static_cast<SMOKE_APP*>( wxTheApp )->m_onInitRan, "OnInit body ran" );
    }
}


static void test_wx_string_and_files()
{
    // Non-ASCII here on purpose: this is the case that fails without the
    // C.UTF-8 setlocale() in main().
    wxString s = wxString::Format( wxT( "board-%d-%s" ), 42, wxT( "revÄ" ) );
    check( s == wxT( "board-42-revÄ" ), "wxString::Format + unicode" );
    check( s.Upper().StartsWith( wxT( "BOARD" ) ), "wxString::Upper/StartsWith" );
    check( s.ToStdString().size() > 0, "wxString::ToStdString (utf8)" );

    // wxFileName
    wxFileName fn( wxT( "/work/proj/sub/../board.kicad_pcb" ) );
    fn.Normalize( wxPATH_NORM_DOTS );
    check( fn.GetFullName() == wxT( "board.kicad_pcb" ), "wxFileName::GetFullName" );
    check( fn.GetExt() == wxT( "kicad_pcb" ), "wxFileName::GetExt" );
    check( fn.GetPath() == wxT( "/work/proj" ), "wxFileName::Normalize(dots)" );

    // Make the directory tree under MEMFS.
    check( wxFileName::Mkdir( wxT( "/work/proj" ), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ),
           "wxFileName::Mkdir recursive" );
    check( wxFileName::DirExists( wxT( "/work/proj" ) ), "wxFileName::DirExists" );

    // wxFFile write + read.
    const wxString path = wxT( "/work/proj/board.kicad_pcb" );
    const wxString payload = wxT( "(kicad_pcb (version 20240101) (generator fp_pcb_wasm))\n" );

    {
        wxFFile out( path, wxT( "wb" ) );
        check( out.IsOpened(), "wxFFile open for write" );
        check( out.Write( payload ), "wxFFile::Write" );
        check( out.Close(), "wxFFile::Close" );
    }

    check( wxFileName::FileExists( path ), "wxFileName::FileExists after write" );

    {
        wxFFile in( path, wxT( "rb" ) );
        check( in.IsOpened(), "wxFFile open for read" );
        wxString back;
        check( in.ReadAll( &back ), "wxFFile::ReadAll" );
        check( back == payload, "wxFFile round-trip content matches" );
    }

    // wxFile (low level fd based).
    {
        wxFile f( wxT( "/work/proj/raw.bin" ), wxFile::write );
        check( f.IsOpened(), "wxFile open for write" );
        const char blob[] = "0123456789";
        check( f.Write( blob, 10 ) == 10, "wxFile::Write" );
        f.Close();
        check( wxFile::Exists( wxT( "/work/proj/raw.bin" ) ), "wxFile::Exists" );
    }

    // wxTextFile.
    {
        wxTextFile tf( wxT( "/work/proj/notes.txt" ) );
        check( tf.Create(), "wxTextFile::Create" );
        tf.AddLine( wxT( "line one" ) );
        tf.AddLine( wxT( "line two" ) );
        check( tf.Write(), "wxTextFile::Write" );
        tf.Close();

        wxTextFile rd( wxT( "/work/proj/notes.txt" ) );
        check( rd.Open(), "wxTextFile::Open" );
        check( rd.GetLineCount() == 2, "wxTextFile::GetLineCount" );
        check( rd[1] == wxT( "line two" ), "wxTextFile line content" );
    }

    // wxDir.
    {
        wxDir dir( wxT( "/work/proj" ) );
        check( dir.IsOpened(), "wxDir::IsOpened" );
        wxArrayString files;
        size_t n = wxDir::GetAllFiles( wxT( "/work/proj" ), &files, wxEmptyString, wxDIR_FILES );
        check( n >= 3, "wxDir::GetAllFiles" );
    }

    // wxStandardPaths - KiCad uses this to find config/data dirs.
    {
        wxString cfg = wxStandardPaths::Get().GetUserConfigDir();
        check( !cfg.IsEmpty(), "wxStandardPaths::GetUserConfigDir" );
        wxString tmp = wxStandardPaths::Get().GetTempDir();
        check( !tmp.IsEmpty(), "wxStandardPaths::GetTempDir" );
    }
}


static void test_wx_misc()
{
    // wxStringTokenizer
    wxStringTokenizer tok( wxT( "R1,R2,R3,R4" ), wxT( "," ) );
    int count = 0;

    while( tok.HasMoreTokens() )
    {
        tok.GetNextToken();
        ++count;
    }

    check( count == 4, "wxStringTokenizer" );

    // wxRegEx
    wxRegEx re( wxT( "^([A-Z]+)([0-9]+)$" ) );
    check( re.IsValid(), "wxRegEx::IsValid" );
    check( re.Matches( wxT( "C123" ) ), "wxRegEx::Matches" );
    check( re.GetMatch( wxT( "C123" ), 1 ) == wxT( "C" ), "wxRegEx::GetMatch" );

    // wxDateTime
    wxDateTime dt( 9, wxDateTime::Sep, 2026, 12, 0, 0 );
    check( dt.IsValid(), "wxDateTime::IsValid" );
    check( dt.GetYear() == 2026, "wxDateTime::GetYear" );
    check( !dt.FormatISODate().IsEmpty(), "wxDateTime::FormatISODate" );

    // Translations: the _() macro must resolve even with no catalog loaded.
    {
        wxTranslations* trans = new wxTranslations();
        wxTranslations::Set( trans );
        wxString t = _( "Untranslated" );
        check( t == wxT( "Untranslated" ), "wxTranslations / _() passthrough" );
    }

    // wxFileConfig
    {
        wxStringInputStream in( wxT( "[general]\nlast_board=/work/proj/board.kicad_pcb\nzoom=3\n" ) );
        wxFileConfig cfg( in );
        wxString v;
        check( cfg.Read( wxT( "/general/last_board" ), &v ), "wxFileConfig::Read" );
        check( v == wxT( "/work/proj/board.kicad_pcb" ), "wxFileConfig value" );
        long z = 0;
        check( cfg.Read( wxT( "/general/zoom" ), &z ) && z == 3, "wxFileConfig long value" );
    }

    // wxCmdLineParser
    {
        static const wxCmdLineEntryDesc desc[] = {
            { wxCMD_LINE_SWITCH, "h", "help", "show help", wxCMD_LINE_VAL_NONE, 0 },
            { wxCMD_LINE_OPTION, "b", "board", "board file", wxCMD_LINE_VAL_STRING, 0 },
            { wxCMD_LINE_NONE, nullptr, nullptr, nullptr, wxCMD_LINE_VAL_NONE, 0 }
        };

        wxCmdLineParser parser( desc );
        parser.SetCmdLine( wxT( "--board=/work/proj/board.kicad_pcb" ) );
        check( parser.Parse( false ) == 0, "wxCmdLineParser::Parse" );
        wxString b;
        check( parser.Found( wxT( "board" ), &b ) && b.EndsWith( wxT( "board.kicad_pcb" ) ),
               "wxCmdLineParser::Found" );
    }

    // wxLog must be functional (KiCad logs through it).
    {
        wxLogNull noLog;
        wxLogMessage( wxT( "swallowed" ) );
        check( true, "wxLog available" );
    }
}


static void test_wx_xml()
{
    const wxString xml = wxT( "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                              "<project name=\"demo\">\n"
                              "  <sheet id=\"1\">root</sheet>\n"
                              "</project>\n" );

    wxStringInputStream in( xml );
    wxXmlDocument doc;
    check( doc.Load( in ), "wxXmlDocument::Load" );

    wxXmlNode* root = doc.GetRoot();
    check( root && root->GetName() == wxT( "project" ), "wxXml root node" );
    check( root && root->GetAttribute( wxT( "name" ) ) == wxT( "demo" ), "wxXml attribute" );

    wxXmlNode* child = root ? root->GetChildren() : nullptr;

    while( child && child->GetType() != wxXML_ELEMENT_NODE )
        child = child->GetNext();

    check( child && child->GetNodeContent() == wxT( "root" ), "wxXml child content" );
}


static void test_wx_streams()
{
    // wxZlibOutputStream/wxZlibInputStream exercise the zlib port through wx.
    std::string raw( 4096, 'z' );

    for( size_t i = 0; i < raw.size(); ++i )
        raw[i] = static_cast<char>( 'a' + ( i % 26 ) );

    wxMemoryOutputStream compressed;

    {
        wxZlibOutputStream zout( compressed, 6, wxZLIB_ZLIB );
        zout.Write( raw.data(), raw.size() );
    }

    check( compressed.GetLength() > 0 && compressed.GetLength() < raw.size(),
           "wxZlibOutputStream compresses" );

    std::vector<char> buf( compressed.GetLength() );
    compressed.CopyTo( buf.data(), buf.size() );

    wxMemoryInputStream min( buf.data(), buf.size() );
    check( min.IsOk(), "wxMemoryInputStream" );

    wxZlibInputStream zin( min, wxZLIB_ZLIB );
    std::string back( raw.size(), '\0' );
    zin.Read( &back[0], back.size() );
    check( zin.LastRead() == raw.size() && back == raw, "wxZlibInputStream round-trip" );

    // wxZipInputStream must at least link and report a sane empty archive.
    {
        wxMemoryOutputStream zipMem;

        {
            wxZipOutputStream zip( zipMem );
            zip.PutNextEntry( wxT( "hello.txt" ) );
            const char msg[] = "hello from a zip";
            zip.Write( msg, sizeof( msg ) - 1 );
            zip.Close();
        }

        std::vector<char> zbuf( zipMem.GetLength() );
        zipMem.CopyTo( zbuf.data(), zbuf.size() );

        wxMemoryInputStream zsrc( zbuf.data(), zbuf.size() );
        wxZipInputStream zis( zsrc );
        wxZipEntry* entry = zis.GetNextEntry();
        check( entry != nullptr && entry->GetName() == wxT( "hello.txt" ), "wxZipInputStream entry" );

        char out[32] = { 0 };
        zis.Read( out, sizeof( out ) - 1 );
        check( std::strcmp( out, "hello from a zip" ) == 0, "wxZipInputStream content" );
        delete entry;
    }
}


// ---------------------------------------------------------------------------
// (b) protobuf: host-protoc-generated code against the wasm runtime
// ---------------------------------------------------------------------------
static void test_protobuf()
{
    // This is the check that catches a host/wasm protobuf version mismatch: the
    // generated header expands PROTOBUF_INTERNAL_CHECK_VERSION at static-init
    // time and aborts if the linked runtime disagrees.
    GOOGLE_PROTOBUF_VERIFY_VERSION;
    check( true, "protobuf runtime version check" );

    std::printf( "     protobuf runtime version: %d\n", GOOGLE_PROTOBUF_VERSION );

    fpwasm::smoke::Board board;
    board.set_name( "demo-board" );
    board.set_layers( 4 );
    board.add_nets( "GND" );
    board.add_nets( "VCC" );
    board.add_nets( "SDA" );

    std::string wire;
    check( board.SerializeToString( &wire ), "protobuf SerializeToString" );
    check( wire.size() > 0, "protobuf non-empty wire format" );

    fpwasm::smoke::Board back;
    check( back.ParseFromString( wire ), "protobuf ParseFromString" );
    check( back.name() == "demo-board", "protobuf string field" );
    check( back.layers() == 4, "protobuf int field" );
    check( back.nets_size() == 3 && back.nets( 2 ) == "SDA", "protobuf repeated field" );

    // Reflection / descriptors must be alive too (the API server uses them).
    const google::protobuf::Descriptor* d = fpwasm::smoke::Board::descriptor();
    check( d != nullptr && d->full_name() == "fpwasm.smoke.Board", "protobuf descriptor" );
    check( d->field_count() == 3, "protobuf descriptor field count" );

    // Exercise the C++ exception path once: protobuf and abseil are compiled
    // with -fwasm-exceptions, so a throw/catch across that boundary must work.
    try
    {
        throw std::runtime_error( "wasm-eh" );
    }
    catch( const std::exception& e )
    {
        check( std::string( e.what() ) == "wasm-eh", "C++ exceptions (-fwasm-exceptions)" );
    }
}


// ---------------------------------------------------------------------------
// (c) FreeType + HarfBuzz
// ---------------------------------------------------------------------------
static void test_freetype_harfbuzz()
{
    FT_Library lib = nullptr;
    check( FT_Init_FreeType( &lib ) == 0, "FT_Init_FreeType" );

    FT_Face face = nullptr;
    // Font is preloaded into MEMFS at /fonts/smoke.ttf by --embed-file.
    FT_Error err = FT_New_Face( lib, "/fonts/smoke.ttf", 0, &face );
    check( err == 0 && face != nullptr, "FT_New_Face from MEMFS" );

    if( err != 0 || !face )
        return;

    std::printf( "     face: family=%s style=%s glyphs=%ld\n",
                 face->family_name ? face->family_name : "?",
                 face->style_name ? face->style_name : "?",
                 (long) face->num_glyphs );

    check( face->num_glyphs > 50, "FT face has glyphs" );
    check( FT_Set_Char_Size( face, 0, 16 * 64, 72, 72 ) == 0, "FT_Set_Char_Size" );

    FT_UInt gi = FT_Get_Char_Index( face, 'A' );
    check( gi != 0, "FT_Get_Char_Index('A')" );
    check( FT_Load_Glyph( face, gi, FT_LOAD_DEFAULT ) == 0, "FT_Load_Glyph" );
    check( face->glyph->metrics.horiAdvance > 0, "FT glyph advance > 0" );

    // HarfBuzz shaping over the same face.
    hb_font_t* hbFont = hb_ft_font_create_referenced( face );
    check( hbFont != nullptr, "hb_ft_font_create_referenced" );

    hb_buffer_t* buf = hb_buffer_create();
    check( buf != nullptr, "hb_buffer_create" );

    const char* word = "Netlist";
    hb_buffer_add_utf8( buf, word, -1, 0, -1 );
    hb_buffer_set_direction( buf, HB_DIRECTION_LTR );
    hb_buffer_set_script( buf, HB_SCRIPT_LATIN );
    hb_buffer_set_language( buf, hb_language_from_string( "en", -1 ) );

    hb_shape( hbFont, buf, nullptr, 0 );

    unsigned int             glyphCount = 0;
    hb_glyph_info_t*         info = hb_buffer_get_glyph_infos( buf, &glyphCount );
    hb_glyph_position_t*     pos = hb_buffer_get_glyph_positions( buf, &glyphCount );

    check( glyphCount == std::strlen( word ), "hb_shape glyph count" );

    bool allMapped = glyphCount > 0;
    int  totalAdvance = 0;

    for( unsigned int i = 0; i < glyphCount; ++i )
    {
        if( info[i].codepoint == 0 )
            allMapped = false;

        totalAdvance += pos[i].x_advance;
    }

    check( allMapped, "hb_shape mapped every character to a glyph" );
    check( totalAdvance > 0, "hb_shape produced positive advance" );
    std::printf( "     shaped \"%s\": %u glyphs, advance %d/64px\n", word, glyphCount,
                 totalAdvance );

    hb_buffer_destroy( buf );
    hb_font_destroy( hbFont );
    FT_Done_Face( face );
    FT_Done_FreeType( lib );
}


// ---------------------------------------------------------------------------
// (d) zstd
// ---------------------------------------------------------------------------
static void test_zstd()
{
    std::printf( "     zstd version: %s\n", ZSTD_versionString() );

    std::string raw;
    raw.reserve( 64 * 1024 );

    for( int i = 0; i < 4096; ++i )
        raw += "kicad-wasm-dependency-smoke;";

    size_t              bound = ZSTD_compressBound( raw.size() );
    std::vector<char>   comp( bound );

    size_t compSize = ZSTD_compress( comp.data(), bound, raw.data(), raw.size(), 9 );
    check( !ZSTD_isError( compSize ), "ZSTD_compress" );
    check( compSize < raw.size() / 4, "zstd actually compressed" );

    unsigned long long declared = ZSTD_getFrameContentSize( comp.data(), compSize );
    check( declared == raw.size(), "ZSTD_getFrameContentSize" );

    std::vector<char> back( raw.size() );
    size_t decSize = ZSTD_decompress( back.data(), back.size(), comp.data(), compSize );
    check( !ZSTD_isError( decSize ), "ZSTD_decompress" );
    check( decSize == raw.size() && std::memcmp( back.data(), raw.data(), raw.size() ) == 0,
           "zstd round-trip identical" );

    std::printf( "     zstd: %zu -> %zu bytes\n", raw.size(), compSize );
}


int main( int argc, char** argv )
{
    std::printf( "== fp_pcb wasm dependency smoke test ==\n" );

    // REQUIRED on emscripten, and easy to miss: musl starts in the "C" locale,
    // where wctomb() rejects every non-ASCII character, so swprintf() - which
    // is what wxString::Format() ends up calling - returns -1 and wx hands back
    // an EMPTY string for any format whose arguments contain non-ASCII text.
    // Nothing errors; strings just silently vanish. The KiCad wasm host must do
    // this before it touches wxString.
    std::setlocale( LC_ALL, "C.UTF-8" );

    std::printf( "-- wxWidgets app --\n" );
    test_wx_app( argc, argv );

    std::printf( "-- wxWidgets string/files --\n" );
    test_wx_string_and_files();

    std::printf( "-- wxWidgets misc --\n" );
    test_wx_misc();

    std::printf( "-- wxWidgets xml --\n" );
    test_wx_xml();

    std::printf( "-- wxWidgets streams --\n" );
    test_wx_streams();

    std::printf( "-- protobuf --\n" );
    test_protobuf();

    std::printf( "-- freetype + harfbuzz --\n" );
    test_freetype_harfbuzz();

    std::printf( "-- zstd --\n" );
    test_zstd();

    wxEntryCleanup();

    if( g_failures == 0 )
    {
        std::printf( "ALL OK\n" );
        return 0;
    }

    std::printf( "FAILURES: %d\n", g_failures );
    return 1;
}
