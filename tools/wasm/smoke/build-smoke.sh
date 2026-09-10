#!/usr/bin/env bash
#
# Build and run the wasm dependency smoke test.
#
# Compiles tools/wasm/smoke/smoke.cpp against everything build-deps.sh put in
# build/wasm-deps/prefix, links it exactly the way the KiCad wasm host will be
# linked, and runs the result under both node and bun.
#
# Usage: tools/wasm/smoke/build-smoke.sh   (or: tools/wasm/build-deps.sh smoke)

set -euo pipefail

HERE="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
REPO_ROOT="$( cd "$HERE/../../.." && pwd )"

# shellcheck source=../env.sh
source "$REPO_ROOT/tools/wasm/env.sh"

OUT="$WASM_DEPS_ROOT/smoke"
mkdir -p "$OUT"

# The font we embed: shipped with KiCad's bundled libwmf, so nothing new lands
# in the repo just for the smoke test.
FONT="$REPO_ROOT/thirdparty/libwmf/fonts/Carlito-Regular.ttf"

if [ ! -f "$FONT" ]; then
    echo "smoke: font not found: $FONT" >&2
    exit 1
fi

# --- 1. generate protobuf sources with the HOST protoc ----------------------
echo "== smoke: generating protobuf sources with host protoc =="
"$Protobuf_PROTOC_EXECUTABLE" --version
"$Protobuf_PROTOC_EXECUTABLE" --cpp_out="$OUT" -I"$HERE" "$HERE/smoke.proto"
ls -la "$OUT/smoke.pb.cc" "$OUT/smoke.pb.h"

# --- 2. compile + link ------------------------------------------------------
echo "== smoke: compiling =="

WX_CXXFLAGS=$( "$WASM_PREFIX/bin/wx-config" --cxxflags )
WX_LIBS=$( "$WASM_PREFIX/bin/wx-config" --libs base,xml )

export PKG_CONFIG_PATH="$WASM_PREFIX/lib/pkgconfig"
PROTOBUF_CFLAGS=$( pkg-config --cflags protobuf )
PROTOBUF_LIBS=$( pkg-config --libs protobuf )
ZSTD_CFLAGS=$( pkg-config --cflags libzstd )
ZSTD_LIBS=$( pkg-config --libs libzstd )

# shellcheck disable=SC2086
em++ \
    -std=c++20 \
    $WASM_CXXFLAGS \
    $WX_CXXFLAGS \
    $PROTOBUF_CFLAGS \
    $ZSTD_CFLAGS \
    -I"$OUT" \
    "$HERE/smoke.cpp" "$OUT/smoke.pb.cc" \
    $WX_LIBS \
    $PROTOBUF_LIBS \
    $ZSTD_LIBS \
    $WASM_LDFLAGS \
    -sALLOW_MEMORY_GROWTH=1 \
    -sMODULARIZE=1 \
    -sEXPORT_ES6=1 \
    -sENVIRONMENT=node,web,worker \
    --embed-file "$FONT@/fonts/smoke.ttf" \
    -o "$OUT/smoke.mjs"

echo "== smoke: artifacts =="
ls -la "$OUT/smoke.mjs" "$OUT/smoke.wasm"
WASM_BYTES=$( wc -c < "$OUT/smoke.wasm" | tr -d ' ' )
echo "smoke.wasm size: $WASM_BYTES bytes ($(( WASM_BYTES / 1024 )) KiB)"

cp "$HERE/run.mjs" "$OUT/run.mjs"

# --- 3. run under node and bun ---------------------------------------------
rc=0

echo "== smoke: running under node =="
if node "$OUT/run.mjs"; then
    echo "smoke: node PASS"
else
    echo "smoke: node FAIL"
    rc=1
fi

echo "== smoke: running under bun =="
if bun "$OUT/run.mjs"; then
    echo "smoke: bun PASS"
else
    echo "smoke: bun FAIL"
    rc=1
fi

exit $rc
