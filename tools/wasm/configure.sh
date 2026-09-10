#!/usr/bin/env bash
#
# Configure the KiCad headless API core for wasm32-emscripten.
#
#   tools/wasm/configure.sh [extra cmake args...]
#
# Everything that is not obvious is explained in tools/wasm/env.sh and in
# host/STATUS-wasm-build.md.  The two host tools KiCad's build needs (lemon and
# protoc) MUST come from the native side: lemon out of build/headless, protoc
# from brew (36.1, which is exactly the runtime version in the wasm prefix).
set -euo pipefail

HERE="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
export WASM_REPO_ROOT="${WASM_REPO_ROOT:-$( cd "$HERE/../.." && pwd )}"
# shellcheck source=/dev/null
source "$HERE/env.sh"

BUILD="${WASM_BUILD_DIR:-$WASM_REPO_ROOT/build/wasm}"
LEMON="${LEMON_EXE:-$WASM_REPO_ROOT/build/headless/thirdparty/lemon/lemon}"

[ -x "$LEMON" ] || {
    echo "no native lemon at $LEMON -- build it first:" >&2
    echo "    ninja -C build/headless lemon" >&2
    exit 1
}
[ -x "$Protobuf_PROTOC_EXECUTABLE" ] || { echo "no host protoc" >&2; exit 1; }

# The emscripten ports (zlib, freetype, harfbuzz, boost headers) live in the
# toolchain's own sysroot.  We override CMAKE_FIND_ROOT_PATH, so it has to be
# named again or find_package( ZLIB / Freetype / harfbuzz ) misses them.
EM_SYSROOT="${EM_SYSROOT:-$( em-config CACHE )/sysroot}"

# GLM is header-only and architecture independent (PDF_plotter.cpp and the
# easyedapro parser use it), so the host copy is the right one; there is no
# wasm build of it in the prefix.
GLM_PREFIX="${GLM_PREFIX:-$( brew --prefix glm 2>/dev/null || echo /opt/homebrew )}"
GLM_INCLUDE_DIR="$GLM_PREFIX/include"

# FreeType is the one port that comes in setjmp/longjmp variants, because ftgrays.c (the
# rasterizer KiCad's outline font renderer reaches through FT_Render_Glyph) uses setjmp.
# `-sUSE_FREETYPE=1` would make emcc pick the right one, but KiCad's find_package( Freetype )
# puts an explicit path on the link line and that bypasses the choice: plain libfreetype.a is
# the JavaScript-longjmp build, whose invoke_iii / emscripten_longjmp imports make emcc's
# post-link stage assert
#     "invoke_ functions exported but exceptions and longjmp are both disabled"
# because -fwasm-exceptions implies SUPPORT_LONGJMP=wasm.  -legacysjlj is the variant that
# matches -fwasm-exceptions with -sWASM_LEGACY_EXCEPTIONS=1.
FREETYPE_LIBRARY="${FREETYPE_LIBRARY:-$EM_SYSROOT/lib/wasm32-emscripten/libfreetype-legacysjlj.a}"

# Optimisation level for KiCad's own translation units.  env.sh puts -O2 in
# CMAKE_CXX_FLAGS, but CMake appends CMAKE_CXX_FLAGS_RELEASE after it and that is
# "-O3 -DNDEBUG", so the -O2 never took effect: the module was built at -O3.
#
# -O2 is the default here: 36.90 MB -> 35.53 MB against an -O3 control from the
# same tree (-3.7%), for +2.5% on DRC and no change to project open or conformance.
# WASM_OPT_LEVEL=-O3 puts the old behaviour back.
#
# WASM_OPT_LEVEL=-Os gives 30.43 MB (-17.5%) and is conformance-clean, but costs
# ~28% on DRC and ~14% on project open -- worth it only where the download
# dominates.  Note that serving the module Brotli-compressed takes it to 6.65 MB
# on the wire with no build change at all, which is a far bigger win than either.
#
# Measurements, and the six link-flag experiments that were NOT worth adopting,
# are in host/STATUS-module-size.md.
WASM_OPT_LEVEL="${WASM_OPT_LEVEL:--O2}"

emcmake cmake -S "$WASM_REPO_ROOT" -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS_RELEASE="$WASM_OPT_LEVEL -DNDEBUG" \
    -DCMAKE_CXX_FLAGS_RELEASE="$WASM_OPT_LEVEL -DNDEBUG" \
    -DCMAKE_CXX_STANDARD=20 \
    -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
    -DCMAKE_TRY_COMPILE_PLATFORM_VARIABLES=CMAKE_CXX_SCAN_FOR_MODULES \
    -DCMAKE_PREFIX_PATH="$WASM_PREFIX" \
    -DCMAKE_FIND_ROOT_PATH="$WASM_PREFIX;$EM_SYSROOT;$GLM_PREFIX" \
    -DCMAKE_C_FLAGS="$WASM_CFLAGS" \
    -DCMAKE_CXX_FLAGS="$WASM_CXXFLAGS" \
    -DCMAKE_EXE_LINKER_FLAGS="$WASM_LDFLAGS" \
    -DProtobuf_PROTOC_EXECUTABLE="$Protobuf_PROTOC_EXECUTABLE" \
    -Dabsl_DIR="$WASM_PREFIX/lib/cmake/absl" \
    -Dprotobuf_DIR="$WASM_PREFIX/lib/cmake/protobuf" \
    -Dzstd_DIR="$WASM_PREFIX/lib/cmake/zstd" \
    -DwxWidgets_INCLUDE_DIRS="$wxWidgets_INCLUDE_DIRS" \
    -DwxWidgets_LIBRARIES="$wxWidgets_LIBRARIES" \
    -DwxWidgets_DEFINITIONS="$wxWidgets_DEFINITIONS" \
    -DGLM_INCLUDE_DIR="$GLM_INCLUDE_DIR" \
    -DFREETYPE_LIBRARY_RELEASE="$FREETYPE_LIBRARY" \
    -DLEMON_EXE="$LEMON" \
    -DKICAD_HEADLESS_API=ON \
    "$@"
