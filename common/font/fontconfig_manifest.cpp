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

/*
 * Headless replacement for common/font/fontconfig.cpp.
 *
 * There is no system font database in the headless API core (and none at all inside a wasm
 * module), so the font list is whatever the host mounted: KICAD_FONTS_DIR names one or more
 * directories, each of which either carries a manifest.json
 *
 *     { "default": "Carlito",
 *       "fonts": [ { "family": "Carlito", "style": "Regular",
 *                    "bold": false, "italic": false, "file": "Carlito-Regular.ttf" }, ... ] }
 *
 * or is just a folder of *.ttf / *.otf, in which case the family and style names are read out
 * of the files with FreeType.  A manifest keeps the module from opening every file on the first
 * lookup; the scan exists so that pointing KICAD_FONTS_DIR at an ordinary font folder works.
 *
 * Matching mirrors common/font/fontconfig.cpp closely enough that callers cannot tell the two
 * apart: the same FF_RESULT codes, the same "the name says bold, so it is bold" rule, the same
 * synthetic-style reporting, and the same substitution warnings through the REPORTER.  That
 * matters because OUTLINE_FONT::LoadFont() drives the stroke-font fallback and the fake
 * bold/italic flags off those codes.
 *
 * Only the public surface of fontconfig::FONTCONFIG is defined; the private helpers exist
 * solely to talk to libfontconfig and have no callers outside fontconfig.cpp.
 */

#include <font/fontconfig.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#ifdef _MSC_VER
#include <ft2build.h>
#else
#include <freetype2/ft2build.h>
#endif
#include FT_FREETYPE_H

#include <nlohmann/json.hpp>

#include <reporter.h>
#include <wx/filename.h>
#include <wx/log.h>
#include <wx/translation.h>

#include <trace_helpers.h>

using namespace fontconfig;


REPORTER* FONTCONFIG::s_reporter = nullptr;

static std::mutex g_fontConfigMutex;


namespace
{

/// One face of one font file: what a fontconfig pattern would have carried.
struct FACE_ENTRY
{
    std::string family;
    std::string style;
    std::string file;
    int         faceIndex = 0;

    /// "Can serve as bold/italic", used to pick between the faces of a family.  The FF_RESULT
    /// is recomputed from the style string exactly the way fontconfig.cpp does it.
    bool bold = false;
    bool italic = false;
};


/// The font database, built once from KICAD_FONTS_DIR and grown by embedded files.
std::vector<FACE_ENTRY> g_faces;

/// Family the manifest asks for when nothing matches, or the first family found.
std::string g_defaultFamily;

/// Font files already probed, so an embedded file is only read once.
std::set<std::string> g_scannedFiles;

bool g_scanned = false;


bool containsAny( const wxString& aLowerHaystack, std::initializer_list<const char*> aNeedles )
{
    for( const char* needle : aNeedles )
    {
        if( aLowerHaystack.Contains( wxString::FromUTF8( needle ) ) )
            return true;
    }

    return false;
}


/**
 * fontconfig.cpp's reading of a style string, kept verbatim so that the FF_RESULT this file
 * returns is the one the real wrapper would have returned.
 *
 * @param aLowerStyle is the lower-cased style name ("bold italic", "semibold", ...)
 * @param aWantBold is what the caller asked for; a medium/semibold face answers "yes" to both
 *                  questions, which is what fontconfig does
 */
bool styleIsBold( const wxString& aLowerStyle, bool aWantBold )
{
    if( containsAny( aLowerStyle, { "thin", "light", "regular", "roman", "book" } ) )
        return false;
    else if( containsAny( aLowerStyle, { "medium", "semibold", "demibold" } ) )
        return aWantBold;
    else if( containsAny( aLowerStyle, { "bold", "heavy", "black", "thick", "dark" } ) )
        return true;

    return false;
}


bool styleIsItalic( const wxString& aLowerStyle )
{
    return containsAny( aLowerStyle, { "italic", "oblique", "slant" } );
}


/// A font name may be qualified ("Times:Bold:Italic"); the family is what precedes the colon.
wxString familyPart( const wxString& aFontName )
{
    return aFontName.BeforeFirst( ':' );
}


/**
 * Read the family and style of every face in one font file.
 *
 * @param aPath is the font file
 * @param aOut receives one entry per face
 * @return false if FreeType could not open the file at all
 */
bool probeFontFile( const std::string& aPath, std::vector<FACE_ENTRY>& aOut )
{
    FT_Library library = nullptr;

    if( FT_Init_FreeType( &library ) != 0 )
        return false;

    FT_Face face = nullptr;

    if( FT_New_Face( library, aPath.c_str(), 0, &face ) != 0 )
    {
        FT_Done_FreeType( library );
        return false;
    }

    const long faceCount = face->num_faces > 0 ? face->num_faces : 1;

    for( long i = 0; i < faceCount; ++i )
    {
        FT_Face thisFace = face;

        if( i != 0 && FT_New_Face( library, aPath.c_str(), i, &thisFace ) != 0 )
            continue;

        // A bitmap-only face has no outline for us to plot; fontconfig's ListFonts skips
        // these on FC_OUTLINE and FindFont would only ever hand back something unusable.
        if( thisFace->face_flags & FT_FACE_FLAG_SCALABLE )
        {
            FACE_ENTRY entry;
            entry.family = thisFace->family_name ? thisFace->family_name : "";
            entry.style = thisFace->style_name ? thisFace->style_name : "";
            entry.file = aPath;
            entry.faceIndex = static_cast<int>( i );

            wxString lowerStyle = wxString::FromUTF8( entry.style.c_str() ).Lower();

            entry.bold = styleIsBold( lowerStyle, true )
                         || ( thisFace->style_flags & FT_STYLE_FLAG_BOLD ) != 0;
            entry.italic = styleIsItalic( lowerStyle )
                           || ( thisFace->style_flags & FT_STYLE_FLAG_ITALIC ) != 0;

            if( !entry.family.empty() )
                aOut.push_back( std::move( entry ) );
        }

        if( thisFace != face )
            FT_Done_Face( thisFace );
    }

    FT_Done_Face( face );
    FT_Done_FreeType( library );

    return true;
}


void addFace( FACE_ENTRY aEntry )
{
    if( aEntry.family.empty() || aEntry.file.empty() )
        return;

    if( g_defaultFamily.empty() )
        g_defaultFamily = aEntry.family;

    g_faces.push_back( std::move( aEntry ) );
}


/// Pull the fonts out of <aDir>/manifest.json.  @return false if there is no manifest.
bool readManifest( const std::filesystem::path& aDir )
{
    std::filesystem::path manifestPath = aDir / "manifest.json";
    std::error_code       ec;

    if( !std::filesystem::is_regular_file( manifestPath, ec ) )
        return false;

    try
    {
        std::ifstream   stream( manifestPath );
        nlohmann::json  js = nlohmann::json::parse( stream );

        if( js.contains( "default" ) && js.at( "default" ).is_string() )
            g_defaultFamily = js.at( "default" ).get<std::string>();

        if( !js.contains( "fonts" ) || !js.at( "fonts" ).is_array() )
        {
            wxLogTrace( traceFonts, wxS( "Font manifest '%s' has no fonts array." ),
                        manifestPath.string() );
            return true;
        }

        for( const nlohmann::json& item : js.at( "fonts" ) )
        {
            if( !item.is_object() || !item.contains( "file" ) || !item.at( "file" ).is_string() )
                continue;

            FACE_ENTRY entry;
            std::filesystem::path file = item.at( "file" ).get<std::string>();

            entry.file = ( file.is_absolute() ? file : aDir / file ).string();

            if( item.contains( "family" ) && item.at( "family" ).is_string() )
                entry.family = item.at( "family" ).get<std::string>();

            if( item.contains( "style" ) && item.at( "style" ).is_string() )
                entry.style = item.at( "style" ).get<std::string>();

            if( item.contains( "faceIndex" ) && item.at( "faceIndex" ).is_number_integer() )
                entry.faceIndex = item.at( "faceIndex" ).get<int>();

            wxString lowerStyle = wxString::FromUTF8( entry.style.c_str() ).Lower();

            entry.bold = styleIsBold( lowerStyle, true );
            entry.italic = styleIsItalic( lowerStyle );

            if( item.contains( "bold" ) && item.at( "bold" ).is_boolean() )
                entry.bold = item.at( "bold" ).get<bool>();

            if( item.contains( "italic" ) && item.at( "italic" ).is_boolean() )
                entry.italic = item.at( "italic" ).get<bool>();

            // A manifest that names only the file still has to say what family it is; ask
            // FreeType rather than guessing from the file name.
            if( entry.family.empty() )
            {
                std::vector<FACE_ENTRY> probed;

                if( probeFontFile( entry.file, probed ) )
                {
                    for( FACE_ENTRY& one : probed )
                        addFace( std::move( one ) );

                    g_scannedFiles.insert( entry.file );
                }

                continue;
            }

            g_scannedFiles.insert( entry.file );
            addFace( std::move( entry ) );
        }
    }
    catch( const std::exception& e )
    {
        wxLogTrace( traceFonts, wxS( "Could not read font manifest '%s': %s" ),
                    manifestPath.string(), e.what() );
    }

    return true;
}


/// No manifest: read every font file in the directory.
void scanDirectory( const std::filesystem::path& aDir )
{
    std::error_code ec;

    for( const std::filesystem::directory_entry& item :
         std::filesystem::directory_iterator( aDir, ec ) )
    {
        if( !item.is_regular_file( ec ) )
            continue;

        std::string ext = item.path().extension().string();
        std::transform( ext.begin(), ext.end(), ext.begin(),
                        []( unsigned char c ) { return std::tolower( c ); } );

        if( ext != ".ttf" && ext != ".otf" && ext != ".ttc" && ext != ".otc" )
            continue;

        std::string path = item.path().string();

        if( !g_scannedFiles.insert( path ).second )
            continue;

        std::vector<FACE_ENTRY> probed;

        if( probeFontFile( path, probed ) )
        {
            for( FACE_ENTRY& one : probed )
                addFace( std::move( one ) );
        }
        else
        {
            wxLogTrace( traceFonts, wxS( "FreeType could not read font file '%s'." ), path );
        }
    }
}


/// Build the database from KICAD_FONTS_DIR.  Call with g_fontConfigMutex held.
void ensureScanned()
{
    if( g_scanned )
        return;

    g_scanned = true;

    const char* dirs = std::getenv( "KICAD_FONTS_DIR" );

    if( !dirs || !*dirs )
        return;

#ifdef __WINDOWS__
    const char separator = ';';
#else
    const char separator = ':';
#endif

    std::string       list( dirs );
    std::string::size_type start = 0;

    while( start <= list.size() )
    {
        std::string::size_type end = list.find( separator, start );
        std::string            one = list.substr( start, end == std::string::npos
                                                                 ? std::string::npos
                                                                 : end - start );

        if( !one.empty() )
        {
            std::filesystem::path dir( one );
            std::error_code       ec;

            if( std::filesystem::is_directory( dir, ec ) )
            {
                if( !readManifest( dir ) )
                    scanDirectory( dir );
            }
            else
            {
                wxLogTrace( traceFonts, wxS( "KICAD_FONTS_DIR entry '%s' is not a directory." ),
                            one );
            }
        }

        if( end == std::string::npos )
            break;

        start = end + 1;
    }
}


/// Fold a document's embedded font files into the database, once each.
void addEmbeddedFiles( const std::vector<wxString>* aEmbeddedFiles )
{
    if( !aEmbeddedFiles )
        return;

    for( const wxString& file : *aEmbeddedFiles )
    {
        std::string path( file.utf8_string() );

        if( path.empty() || !g_scannedFiles.insert( path ).second )
            continue;

        std::vector<FACE_ENTRY> probed;

        if( probeFontFile( path, probed ) )
        {
            for( FACE_ENTRY& one : probed )
                addFace( std::move( one ) );
        }
    }
}


/// How well a face answers a bold/italic request; higher is better.
int styleScore( const FACE_ENTRY& aEntry, bool aBold, bool aItalic )
{
    int score = 0;

    if( aEntry.bold == aBold )
        score += 2;

    if( aEntry.italic == aItalic )
        score += 2;

    // Break a tie towards the plainest face rather than an arbitrary one
    if( !aEntry.bold && !aEntry.italic )
        score += 1;

    return score;
}


const FACE_ENTRY* bestOf( const std::vector<const FACE_ENTRY*>& aCandidates, bool aBold,
                          bool aItalic )
{
    const FACE_ENTRY* best = nullptr;
    int               bestScore = -1;

    for( const FACE_ENTRY* entry : aCandidates )
    {
        int score = styleScore( *entry, aBold, aItalic );

        if( score > bestScore )
        {
            bestScore = score;
            best = entry;
        }
    }

    return best;
}


/**
 * The family half of the match: an exact family name, else a family the requested name is a
 * prefix of (which is what fontconfig.cpp's familyMatched test accepts), else the default
 * family, else anything at all.  fontconfig always answers with *some* font, and callers rely
 * on that -- FF_ERROR means "the font system is broken", not "no such family".
 */
const FACE_ENTRY* matchFamily( const wxString& aFontName, bool aBold, bool aItalic,
                               bool* aFamilyMatched )
{
    wxString wanted = familyPart( aFontName ).Lower();

    std::vector<const FACE_ENTRY*> exact;
    std::vector<const FACE_ENTRY*> prefix;
    std::vector<const FACE_ENTRY*> fallback;

    for( const FACE_ENTRY& entry : g_faces )
    {
        wxString family = wxString::FromUTF8( entry.family.c_str() ).Lower();

        if( !wanted.empty() && family == wanted )
            exact.push_back( &entry );
        else if( !wanted.empty() && family.StartsWith( wanted ) )
            prefix.push_back( &entry );

        if( !g_defaultFamily.empty()
            && family == wxString::FromUTF8( g_defaultFamily.c_str() ).Lower() )
        {
            fallback.push_back( &entry );
        }
    }

    *aFamilyMatched = !exact.empty() || !prefix.empty();

    if( !exact.empty() )
        return bestOf( exact, aBold, aItalic );

    if( !prefix.empty() )
        return bestOf( prefix, aBold, aItalic );

    if( !fallback.empty() )
        return bestOf( fallback, aBold, aItalic );

    if( g_faces.empty() )
        return nullptr;

    for( const FACE_ENTRY& entry : g_faces )
        fallback.push_back( &entry );

    return bestOf( fallback, aBold, aItalic );
}

} // namespace


FONTCONFIG::FONTCONFIG()
{
}


wxString FONTCONFIG::Version()
{
    return wxS( "manifest" );
}


void fontconfig::FONTCONFIG::SetReporter( REPORTER* aReporter )
{
    std::lock_guard lock( g_fontConfigMutex );
    s_reporter = aReporter;
}


REPORTER& fontconfig::FONTCONFIG::GetReporter()
{
    std::lock_guard lock( g_fontConfigMutex );
    return s_reporter ? *s_reporter : NULL_REPORTER::GetInstance();
}


FONTCONFIG::FF_RESULT FONTCONFIG::FindFont( const wxString& aFontName, wxString& aFontFile,
                                            int& aFaceIndex, bool aBold, bool aItalic,
                                            const std::vector<wxString>* aEmbeddedFiles )
{
    std::lock_guard lock( g_fontConfigMutex );

    FF_RESULT retval = FF_RESULT::FF_ERROR;

    aFontFile = wxEmptyString;
    aFaceIndex = 0;

    ensureScanned();
    addEmbeddedFiles( aEmbeddedFiles );

    // If the original font name contains any of these, then it is bold, regardless
    // of whether we are looking for bold or not
    if( containsAny( aFontName.Lower(), { "bold",     // also catches ultrabold
                                          "heavy",
                                          "black",    // also catches extrablack
                                          "thick",
                                          "dark" } ) )
    {
        aBold = true;
    }

    const wxString&   qualifiedFontName = aFontName;
    bool              familyMatched = false;
    const FACE_ENTRY* match = matchFamily( aFontName, aBold, aItalic, &familyMatched );
    wxString          fontName;

    if( match )
    {
        aFontFile = wxString::FromUTF8( match->file.c_str() );
        aFaceIndex = match->faceIndex;

        retval = FF_RESULT::FF_SUBSTITUTE;

        wxString styleStr = wxString::FromUTF8( match->style.c_str() );

        fontName = wxString::FromUTF8( match->family.c_str() );

        if( !styleStr.IsEmpty() )
        {
            wxString qualifier = styleStr;
            qualifier.Replace( ' ', ':' );
            fontName += ':' + qualifier;
        }

        wxString lowerStyle = styleStr.Lower();
        bool     has_bold = styleIsBold( lowerStyle, aBold );
        bool     has_ital = styleIsItalic( lowerStyle );

        if( familyMatched )
        {
            if( ( aBold != has_bold ) || ( aItalic != has_ital ) )
                retval = FF_RESULT::FF_SUBSTITUTE;
            else
                retval = FF_RESULT::FF_OK;
        }

        // Fallback families need synthetic styles just as exact family matches do
        if( ( aBold && !has_bold ) && ( aItalic && !has_ital ) )
            retval = FF_RESULT::FF_MISSING_BOLD_ITAL;
        else if( aBold && !has_bold )
            retval = FF_RESULT::FF_MISSING_BOLD;
        else if( aItalic && !has_ital )
            retval = FF_RESULT::FF_MISSING_ITAL;
    }

    if( retval == FF_RESULT::FF_ERROR )
    {
        // No font database at all, or nothing readable in it.  OUTLINE_FONT::LoadFont() reads
        // this as "use the stroke font", which is the right answer for a bare module.
        if( s_reporter )
        {
            s_reporter->Report( wxString::Format( _( "Error loading font '%s'." ),
                                                  qualifiedFontName ) );
        }
    }
    else if( retval == FF_RESULT::FF_SUBSTITUTE || !familyMatched )
    {
        fontName.Replace( ':', ' ' );

        // If we missed a case but the matching found the original font name, then we are
        // not substituting
        if( fontName.CmpNoCase( qualifiedFontName ) == 0 )
        {
            if( retval == FF_RESULT::FF_SUBSTITUTE )
                retval = FF_RESULT::FF_OK;
        }
        else if( s_reporter )
        {
            s_reporter->Report( wxString::Format( _( "Font '%s' not found; substituting '%s'." ),
                                                  qualifiedFontName,
                                                  fontName ) );
        }
    }

    return retval;
}


void FONTCONFIG::ListFonts( std::vector<std::string>& aFonts, const std::string& aDesiredLang,
                            const std::vector<wxString>* aEmbeddedFiles, bool aForce )
{
    std::lock_guard lock( g_fontConfigMutex );

    ensureScanned();
    addEmbeddedFiles( aEmbeddedFiles );

    // The manifest carries no per-language family names, so the cache can never go stale on a
    // language change the way the fontconfig one does; aDesiredLang is recorded and ignored.
    if( m_fontInfoCache.empty() || m_fontCacheLastLang != aDesiredLang || aForce )
    {
        m_fontInfoCache.clear();

        for( const FACE_ENTRY& entry : g_faces )
        {
            if( entry.family.empty() || entry.family.front() == '.' )
                continue;

            FONTINFO fontInfo( entry.file, entry.style, entry.family );

            std::map<std::string, FONTINFO>::iterator it = m_fontInfoCache.find( entry.family );

            if( it == m_fontInfoCache.end() )
                m_fontInfoCache.emplace( entry.family, fontInfo );
            else
                it->second.Children().push_back( fontInfo );
        }

        m_fontCacheLastLang = aDesiredLang;
    }

    for( const std::pair<const std::string, FONTINFO>& entry : m_fontInfoCache )
        aFonts.push_back( entry.second.Family() );
}


FONTCONFIG* Fontconfig()
{
    static FONTCONFIG* s_config = nullptr;

    if( !s_config )
        s_config = new FONTCONFIG();

    return s_config;
}
