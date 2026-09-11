#!/usr/bin/env bash
#
# Environment for the KiCad / FabPlane PCB WebAssembly build.
#
# Source this before running emcmake on KiCad itself, or before building
# anything that has to link against the libraries produced by
# tools/wasm/build-deps.sh:
#
#     source tools/wasm/env.sh
#     emcmake cmake -S . -B build/wasm $WASM_CMAKE_ARGS ...
#
# Every dependency in $WASM_PREFIX was compiled with exactly $WASM_CXXFLAGS.
# KiCad must use the same flags: mixing exception ABIs (-fexceptions, i.e.
# JavaScript exceptions, vs -fwasm-exceptions, i.e. the native wasm EH
# proposal) produces link errors that look like missing personality/landing-pad
# symbols and give no hint about the real cause.

# ---------------------------------------------------------------------------
# locations
# ---------------------------------------------------------------------------
_WASM_ENV_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
export WASM_REPO_ROOT="${WASM_REPO_ROOT:-$( cd "$_WASM_ENV_DIR/../.." && pwd )}"
export WASM_DEPS_ROOT="${WASM_DEPS_ROOT:-$WASM_REPO_ROOT/build/wasm-deps}"
export WASM_PREFIX="${WASM_PREFIX:-$WASM_DEPS_ROOT/prefix}"

# ---------------------------------------------------------------------------
# compile / link flags  (identical for every library in the prefix)
# ---------------------------------------------------------------------------
# -fwasm-exceptions: native wasm exception handling. Required - KiCad throws.
# No -pthread anywhere: this is a single-threaded build.
#
# -sWASM_LEGACY_EXCEPTIONS=1 is deliberate and must be passed EXPLICITLY:
#   * it is emscripten 6.0.9's default anyway (emcc passes
#     `-mllvm -wasm-use-legacy-eh`), so it is what every library in the prefix
#     was actually compiled with;
#   * the standardised EH encoding (=0) is NOT understood by Node 20's V8 - a
#     module built with it fails to instantiate under `node` while working
#     fine under `bun`;
#   * stating it explicitly also dodges an emscripten 6.0.9 bug: the freetype
#     port does f'-sWASM_LEGACY_EXCEPTIONS={settings.WASM_LEGACY_EXCEPTIONS}',
#     which renders the *default* as the Python literal `True` and makes emcc
#     reject its own port build with
#         "attempt to set `WASM_LEGACY_EXCEPTIONS` to `True`".
#     Passing 1 (or 0) makes the setting an int, and the f-string then works.
export WASM_CFLAGS="-O2 -fwasm-exceptions -sWASM_LEGACY_EXCEPTIONS=1"
# wxDEBUG_LEVEL=0 matches the Homebrew wxWidgets KiCad is built against natively: wxASSERT /
# wxCHECK bodies compile out of KiCad's own TUs, so the module does not spend time formatting
# assertion text (and flooding printErr) for conditions the native build never reports.  wx
# allows the application and the library to use different levels.
export WASM_CXXFLAGS="-O2 -fwasm-exceptions -sWASM_LEGACY_EXCEPTIONS=1 -DwxDEBUG_LEVEL=0"

# Emscripten ports used by KiCad's core. The harfbuzz port pulls freetype in,
# but both are named explicitly so the intent survives a port change.
export WASM_PORT_FLAGS="-sUSE_FREETYPE=1 -sUSE_HARFBUZZ=1 -sUSE_ZLIB=1 -sUSE_BOOST_HEADERS=1"

# Ports have to be on the compile line as well as the link line: that is how
# emcc injects their headers into the include path.
export WASM_CFLAGS="$WASM_CFLAGS $WASM_PORT_FLAGS"
export WASM_CXXFLAGS="$WASM_CXXFLAGS $WASM_PORT_FLAGS"
export WASM_LDFLAGS="-O2 -fwasm-exceptions -sWASM_LEGACY_EXCEPTIONS=1 $WASM_PORT_FLAGS"

# What a final KiCad module wants on top of WASM_LDFLAGS.
export WASM_MODULE_LDFLAGS="-sALLOW_MEMORY_GROWTH=1 -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=node,web,worker"

export CFLAGS="${CFLAGS:-}${CFLAGS:+ }$WASM_CFLAGS"
export CXXFLAGS="${CXXFLAGS:-}${CXXFLAGS:+ }$WASM_CXXFLAGS"
export LDFLAGS="${LDFLAGS:-}${LDFLAGS:+ }$WASM_LDFLAGS"

# ---------------------------------------------------------------------------
# dependency discovery
# ---------------------------------------------------------------------------
# The emscripten toolchain sets CMAKE_FIND_ROOT_PATH_MODE_PACKAGE to ONLY, so
# CMAKE_PREFIX_PATH alone is NOT enough - our prefix must also be a find root,
# or find_package() silently misses it (protobuf then quietly downloads its own
# abseil, which is how you end up with two incompatible abseils).
export CMAKE_PREFIX_PATH="$WASM_PREFIX${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
export PKG_CONFIG_PATH="$WASM_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

# Host protoc. It MUST match the wasm libprotobuf exactly; generated code
# carries a version check that aborts at static-init time otherwise.
export Protobuf_PROTOC_EXECUTABLE="${Protobuf_PROTOC_EXECUTABLE:-$( command -v protoc )}"
export PROTOBUF_PROTOC_EXECUTABLE="$Protobuf_PROTOC_EXECUTABLE"

# ---------------------------------------------------------------------------
# wxWidgets
# ---------------------------------------------------------------------------
# KiCad's CMake uses FindwxWidgets, which cannot run our cross wx-config, so
# hand it the answers directly.
WASM_WX_CONFIG="$WASM_PREFIX/bin/wx-config"

if [ -x "$WASM_WX_CONFIG" ]; then
    export wxWidgets_CONFIG_EXECUTABLE="$WASM_WX_CONFIG"
    # The include path has three entries and the order matters:
    #   1. host/wx_headless/wxgui       -- our wx/setup.h, which forces wxUSE_GUI=1
    #                                      and selects the wxGTK3 port, so that the
    #                                      GUI class DECLARATIONS KiCad's headless
    #                                      TUs name (wxBitmap, wxDC, wxWindow,
    #                                      wxColour, wxImage, wxGridTableBase ...)
    #                                      exist.  Nothing GUI is linked.
    #   2. $WASM_PREFIX/lib/wx/include  -- so that shim setup.h can reach the real
    #                                      one as <base-unicode-static-3.2/wx/setup.h>
    #   3. $WASM_PREFIX/include/wx-3.2  -- the FULL 3.2.11 header set (build-deps
    #                                      rsyncs the source tree's include/wx over
    #                                      the base-only install).
    # tools/wasm/check_wx_abi.sh proves the base classes wxBase actually defines
    # have identical layout under both setup.h variants.
    export wxWidgets_INCLUDE_DIRS="$WASM_REPO_ROOT/host/wx_headless/wxgui;$WASM_PREFIX/lib/wx/include;$WASM_PREFIX/include/wx-3.2"
    export wxWidgets_LIBRARIES="$WASM_PREFIX/lib/libwx_baseu_xml-3.2-Emscripten.a;$WASM_PREFIX/lib/libwx_baseu-3.2-Emscripten.a;$WASM_PREFIX/lib/libwxexpat-3.2.a;$WASM_PREFIX/lib/libwxregexu-3.2.a"
    export wxWidgets_DEFINITIONS="_FILE_OFFSET_BITS=64;WXUSINGDLL=0"
    export wxWidgets_CXX_FLAGS="$( "$WASM_WX_CONFIG" --cxxflags )"
    export wxWidgets_LIBS="$( "$WASM_WX_CONFIG" --libs base,xml )"
    # wx 3.2 also ships a CMake package config in the prefix.
    export wxWidgets_DIR="$WASM_PREFIX/lib/cmake/wxWidgets-3.2"
fi

# ---------------------------------------------------------------------------
# ready-made cmake argument list
# ---------------------------------------------------------------------------
# CMAKE_CXX_SCAN_FOR_MODULES=OFF: brew's emscripten has no working
# clang-scan-deps, so CMake's C++20 module scanning (on by default with Ninja
# for CMAKE_CXX_STANDARD>=20) fails every compile AND every try_compile probe.
# It has to be set on the very first configure, not added later, or the bogus
# probe results are already cached.
export WASM_CMAKE_ARGS="\
-DCMAKE_BUILD_TYPE=Release \
-DCMAKE_CXX_STANDARD=20 \
-DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
-DCMAKE_TRY_COMPILE_PLATFORM_VARIABLES=CMAKE_CXX_SCAN_FOR_MODULES \
-DCMAKE_PREFIX_PATH=$WASM_PREFIX \
-DCMAKE_FIND_ROOT_PATH=$WASM_PREFIX \
-DCMAKE_C_FLAGS=$WASM_CFLAGS \
-DCMAKE_CXX_FLAGS=$WASM_CXXFLAGS \
-DCMAKE_EXE_LINKER_FLAGS=$WASM_LDFLAGS \
-DProtobuf_PROTOC_EXECUTABLE=$Protobuf_PROTOC_EXECUTABLE \
-Dabsl_DIR=$WASM_PREFIX/lib/cmake/absl \
-Dprotobuf_DIR=$WASM_PREFIX/lib/cmake/protobuf \
-Dzstd_DIR=$WASM_PREFIX/lib/cmake/zstd"

unset _WASM_ENV_DIR
