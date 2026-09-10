# The headless API host: KiCad's API core plus a C ABI, and the native binary that speaks the
# stdio framing fab_pcb's StdioTransport expects (docs/08-wasm.md).  Only built under
# KICAD_HEADLESS_API; the top level does not add this directory otherwise.

include_directories( BEFORE ${INC_BEFORE} )
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/pcbnew
    ${CMAKE_SOURCE_DIR}/eeschema
    ${INC_AFTER}
    )

if( KICAD_HEADLESS_WX_BASE_ONLY OR EMSCRIPTEN )
    add_subdirectory( wx_headless )
    set( KICAD_WX_HEADLESS_SHIM wx_headless_shim )
else()
    set( KICAD_WX_HEADLESS_SHIM )
endif()

add_library( kicad_api_host STATIC
    kicad_api_host.cpp
    kicad_api_c.cpp
    cvpcb_headless_kiface.cpp
    )

# The link closure of the two kifaces (pcbnew/CMakeLists.txt's PCBNEW_KIFACE_LIBRARIES and
# eeschema/CMakeLists.txt's eeschema_kiface list) minus the GUI-only members: 3d-viewer and
# idf3 are not built under the option, pcm lives under kicad/, and ngspice is dropped.
target_link_libraries( kicad_api_host
    PUBLIC
        pcbnew_kiface_objects
        eeschema_kiface_objects
        pcbcommon
        connectivity
        pnsrouter
        pads_common
        markdown_lib
        ${PCBNEW_IO_LIBRARIES}
        common
        gal
        kicommon
        kiapi
        kimath
        kiplatform
        core
        sexpr
        nlohmann_json
        Boost::headers
        ${KICAD_WX_HEADLESS_SHIM}
        ${wxWidgets_LIBRARIES}
    )

if( NOT EMSCRIPTEN )
    add_executable( kicad-api-host-native
        main_native.cpp
        )

    target_sources( kicad-api-host-native PRIVATE headless_link_stubs.cpp )

    # kicad_api_host archives the two kiface object sets, and a plain archive link only pulls
    # the members that resolve something.  KiCad registers the DRC/ERC test providers, the
    # property manager types and the IO plugins from file-scope constructors that nothing
    # references, so those members have to come in whether or not they are needed by name.
    if( APPLE )
        target_link_options( kicad-api-host-native PRIVATE
            -Wl,-force_load,$<TARGET_FILE:kicad_api_host> )
    else()
        target_link_options( kicad-api-host-native PRIVATE
            -Wl,--whole-archive $<TARGET_FILE:kicad_api_host> -Wl,--no-whole-archive )
    endif()

    target_link_libraries( kicad-api-host-native
        PRIVATE
            kicad_api_host
        )

    # Bring-up scaffolding: the GUI half of pcbnew/eeschema is not linked, but a number of
    # kept translation units still *reference* it from branches that only run with a frame.
    # Leaving those references undefined lets the headless binary link and abort loudly if one
    # is ever reached; the wasm build gets the same behaviour from
    # -sERROR_ON_UNDEFINED_SYMBOLS=0.  host/STATUS-native-gate.md lists the surface.
    if( KICAD_HEADLESS_ALLOW_UNDEFINED )
        if( APPLE )
            target_link_options( kicad-api-host-native PRIVATE
                -Wl,-undefined,dynamic_lookup )
        else()
            target_link_options( kicad-api-host-native PRIVATE
                -Wl,--unresolved-symbols=ignore-all )
        endif()
    endif()

    # fab_pcb's harness defaults to <kicad>/build/native-host/kicad-api-host-native; keep a link
    # there so the conformance suite needs no environment variable.  A secondary build tree
    # (a side experiment, a second working copy) must not repoint that link at itself, so the
    # link is an option and KICAD_NATIVE_HOST_LINK_DIR can be aimed somewhere else.
    option( KICAD_HEADLESS_HOST_LINK
            "Symlink the built native host into KICAD_NATIVE_HOST_LINK_DIR for the harness" ON )

    set( KICAD_NATIVE_HOST_LINK_DIR "${CMAKE_SOURCE_DIR}/build/native-host"
         CACHE PATH "Where the kicad-api-host-native symlink is created" )

    if( KICAD_HEADLESS_HOST_LINK )
        add_custom_command( TARGET kicad-api-host-native POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${KICAD_NATIVE_HOST_LINK_DIR}"
            COMMAND ${CMAKE_COMMAND} -E create_symlink
                    "$<TARGET_FILE:kicad-api-host-native>"
                    "${KICAD_NATIVE_HOST_LINK_DIR}/kicad-api-host-native"
            COMMENT "Linking ${KICAD_NATIVE_HOST_LINK_DIR}/kicad-api-host-native"
            VERBATIM
            )
    endif()

    if( KICAD_BUILD_QA_TESTS )
        add_test( NAME api.host.selftest
                  COMMAND kicad-api-host-native --selftest
                          "${CMAKE_SOURCE_DIR}/demos/complex_hierarchy/complex_hierarchy.kicad_pcb" )
    endif()
endif()
