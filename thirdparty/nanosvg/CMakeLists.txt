set(NANOSVG_SRCS
    nanosvg.cpp
    nanosvgrast.cpp
    )

add_library(nanosvg STATIC ${NANOSVG_SRCS})

target_include_directories( nanosvg
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    )

if( KICAD_HEADLESS_API )
    # No Fontconfig in the headless dependency set: nanosvg still parses SVG, it just
    # cannot resolve a font family, so text in an imported SVG is skipped.
    target_compile_definitions( nanosvg PRIVATE NANOSVG_NO_FONTCONFIG=1 )
    target_link_libraries( nanosvg PRIVATE Freetype::Freetype HarfBuzz::HarfBuzz )
else()
    target_link_libraries( nanosvg PRIVATE Freetype::Freetype Fontconfig::Fontconfig HarfBuzz::HarfBuzz )
endif()
