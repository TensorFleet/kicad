# The Emscripten module: host/wasm/main_wasm.cpp linked against kicad_api_host, emitted as
# build/wasm/host/kicad_api.js + kicad_api.wasm -- exactly where fab_pcb's
# packages/kicad-wasm/scripts/fetch.ts looks (KICAD_WASM_DIR).
#
# Everything the JavaScript side needs is in fab_pcb docs/08-wasm.md and
# packages/kicad-wasm/src/index.ts; the flags below are the contract:
#   MODULARIZE + EXPORT_ES6 + EXPORT_NAME  -> `import createKicadApi from "kicad_api.js"`
#   EXPORTED_FUNCTIONS                     -> Module._kiapi_* / _malloc / _free
#   EXPORTED_RUNTIME_METHODS               -> HEAPU8, FS, UTF8ToString, stringToNewUTF8
#   FORCE_FILESYSTEM                       -> MEMFS is how projects get in and out

option( KICAD_WASM_ALLOW_UNDEFINED
    "Let the wasm module link with the GUI symbols the kept sources still reference undefined"
    ON )

add_executable( kicad_api
    main_wasm.cpp
    wasm_missing_globals.cpp
    wasm_link_stubs.cpp
    )

target_link_libraries( kicad_api
    PRIVATE
        kicad_api_host
    )

# The same reason the native host needs -force_load (host/STATUS-native-gate.md): KiCad
# registers the DRC/ERC test providers, the property manager types and the IO plugins from
# file-scope constructors that nothing references by name, and a plain archive link drops
# those members.  That was the "DRC finds no errors" bug.
target_link_options( kicad_api PRIVATE
    "SHELL:-Wl,--whole-archive $<TARGET_FILE:kicad_api_host> -Wl,--no-whole-archive"
    )

target_link_options( kicad_api PRIVATE
    -fwasm-exceptions
    "SHELL:-sWASM_LEGACY_EXCEPTIONS=1"
    "SHELL:-sMODULARIZE=1"
    "SHELL:-sEXPORT_ES6=1"
    "SHELL:-sEXPORT_NAME=createKicadApi"
    "SHELL:-sENVIRONMENT=web,worker,node"
    "SHELL:-sALLOW_MEMORY_GROWTH=1"
    "SHELL:-sINITIAL_MEMORY=256MB"
    "SHELL:-sSTACK_SIZE=16MB"
    "SHELL:-sFORCE_FILESYSTEM=1"
    "SHELL:-sEXPORTED_FUNCTIONS=_main,_kiapi_init,_kiapi_dispatch,_kiapi_free,_kiapi_shutdown,_kiapi_last_error,_malloc,_free"
    "SHELL:-sEXPORTED_RUNTIME_METHODS=HEAPU8,FS,UTF8ToString,stringToNewUTF8,ccall,cwrap"
    )

if( KICAD_WASM_ALLOW_UNDEFINED )
    # Bring-up: the wasm equivalent of the native host's generated abort stubs.  wasm-ld turns
    # every remaining GUI reference into a JavaScript stub that throws when it is reached, and
    # -sWARN_ON_UNDEFINED_SYMBOLS prints the list -- which is the list of headless guards that
    # are still missing.  Flip this off to make them hard errors.
    target_link_options( kicad_api PRIVATE
        "SHELL:-sERROR_ON_UNDEFINED_SYMBOLS=0"
        "SHELL:-sWARN_ON_UNDEFINED_SYMBOLS=1"
        )
else()
    target_link_options( kicad_api PRIVATE
        "SHELL:-sERROR_ON_UNDEFINED_SYMBOLS=1"
        )
endif()

set_target_properties( kicad_api PROPERTIES
    OUTPUT_NAME              "kicad_api"
    SUFFIX                   ".js"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/host"
    )

# ------------------------------------------------------------------------------------------
# Default outline fonts, preloaded at /kicad/fonts
#
# common/font/fontconfig_manifest.cpp reads KICAD_FONTS_DIR (the host config's `fonts`, which
# defaults to /kicad/fonts in a wasm build) and resolves outline text against the manifest it
# finds there.  With nothing mounted every board that names a font falls back to the stroke
# font, so the module ships with the two Carlito faces KiCad already carries for libwmf --
# metric-compatible with Calibri, SIL OFL (thirdparty/libwmf/fonts/LICENSE-Carlito.txt).
#
# The bundle lands in kicad_api.data next to kicad_api.js, which fab_pcb's fetch.ts already
# copies when it is present.  Turn this off for a module that mounts its own fonts at runtime.

option( KICAD_WASM_PRELOAD_FONTS "Preload the default outline fonts at /kicad/fonts" ON )

if( KICAD_WASM_PRELOAD_FONTS )
    set( KICAD_WASM_FONT_FILES
        Carlito-Regular.ttf
        Carlito-Bold.ttf
        )

    set( KICAD_WASM_FONT_STAGE "${CMAKE_CURRENT_BINARY_DIR}/fonts" )
    set( KICAD_WASM_FONT_MANIFEST "{\n  \"default\": \"Carlito\",\n  \"fonts\": [\n" )
    set( _first TRUE )

    foreach( _font ${KICAD_WASM_FONT_FILES} )
        configure_file( "${CMAKE_SOURCE_DIR}/thirdparty/libwmf/fonts/${_font}"
                        "${KICAD_WASM_FONT_STAGE}/${_font}" COPYONLY )

        # Carlito-Bold.ttf -> family "Carlito", style "Bold"
        string( REGEX REPLACE "^([^-]+)-([^.]+)\\..*$" "\\1" _family "${_font}" )
        string( REGEX REPLACE "^([^-]+)-([^.]+)\\..*$" "\\2" _style "${_font}" )
        string( TOLOWER "${_style}" _lower_style )

        if( _lower_style MATCHES "bold" )
            set( _bold "true" )
        else()
            set( _bold "false" )
        endif()

        if( _lower_style MATCHES "italic|oblique" )
            set( _italic "true" )
        else()
            set( _italic "false" )
        endif()

        if( NOT _first )
            string( APPEND KICAD_WASM_FONT_MANIFEST ",\n" )
        endif()

        set( _first FALSE )
        string( APPEND KICAD_WASM_FONT_MANIFEST
                "    { \"family\": \"${_family}\", \"style\": \"${_style}\", \"bold\": ${_bold}, \"italic\": ${_italic}, \"file\": \"${_font}\" }" )
    endforeach()

    string( APPEND KICAD_WASM_FONT_MANIFEST "\n  ]\n}\n" )
    file( WRITE "${KICAD_WASM_FONT_STAGE}/manifest.json" "${KICAD_WASM_FONT_MANIFEST}" )

    target_link_options( kicad_api PRIVATE
        "SHELL:--preload-file ${KICAD_WASM_FONT_STAGE}@/kicad/fonts"
        )
endif()
