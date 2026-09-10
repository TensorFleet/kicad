#!/usr/bin/env bash
# Build kicad-cli + the pcbnew and eeschema kifaces on macOS with Homebrew libraries.
# The flags are those of fab_pcb/packages/kicad-patches/build-macos.sh (each one was a
# configure failure on a stock Homebrew setup); see fab_pcb/docs/m0-runbook.md.
#
# Env: KICAD_SRC (default: the tree this script lives in), BUILD_DIR (default build/nightly),
#      BUILD_TYPE (Release), JOBS (hw.ncpu)
set -euo pipefail

SRC="${KICAD_SRC:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}"
BUILD_DIR="${BUILD_DIR:-$SRC/build/nightly}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"
BREW="$(brew --prefix)"

WX_CONFIG=""
for c in "$BREW/opt/wxwidgets@3.2/bin/wx-config" "$BREW/bin/wx-config-3.2" "$BREW/opt/wxwidgets/bin/wx-config" "$BREW/bin/wx-config"; do
  if [ -x "$c" ]; then WX_CONFIG="$c"; break; fi
done
[ -n "$WX_CONFIG" ] || { echo "no wx-config found under $BREW" >&2; exit 1; }

echo "source   : $SRC ($(git -C "$SRC" describe --match '[0-9]*' --always 2>/dev/null || echo unknown))"
echo "build    : $BUILD_DIR ($BUILD_TYPE, $JOBS jobs)"
echo "wxWidgets: $WX_CONFIG ($("$WX_CONFIG" --version))"

launcher=()
if command -v ccache >/dev/null; then
  launcher=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
  ccache --zero-stats >/dev/null || true
fi

cmake -S "$SRC" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  "${launcher[@]}" \
  -DCMAKE_PREFIX_PATH="$BREW" \
  -DwxWidgets_CONFIG_EXECUTABLE="$WX_CONFIG" \
  -DNGSPICE_LIB_NAME=libngspice.0.dylib \
  -DNGSPICE_ROOT_DIR="$BREW/opt/libngspice" \
  -DOCC_INCLUDE_DIR="$BREW/opt/opencascade/include/opencascade" \
  -DOCC_LIBRARY_DIR="$BREW/opt/opencascade/lib" \
  -DKICAD_BUILD_QA_TESTS=OFF \
  -DKICAD_BUILD_I18N=OFF \
  -DKICAD_USE_SENTRY=OFF \
  -DKICAD_UPDATE_CHECK=OFF \
  -DKICAD_INSTALL_DEMOS=OFF \
  -DKICAD_USE_PCH=ON

# cvpcb: eeschema's ERC (and footprint assignment) loads the cvpcb kiface at run time.
ninja -C "$BUILD_DIR" -j"$JOBS" kicad-cli pcbnew_kiface eeschema_kiface cvpcb_kiface

command -v ccache >/dev/null && ccache --show-stats || true
