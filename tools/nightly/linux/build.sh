#!/usr/bin/env bash
# Build kicad-cli + the pcbnew and eeschema kifaces on Linux with distro packages.
# Same targets and CMake flags as fab_pcb's packages/kicad-patches (build-macos.sh, Dockerfile).
#
# Env: BUILD_DIR (default build/nightly), BUILD_TYPE (Release), JOBS (nproc)
set -euo pipefail

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$SRC/build/nightly}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="${JOBS:-$(nproc)}"

echo "source : $SRC ($(git -C "$SRC" describe --match '[0-9]*' --always 2>/dev/null || echo unknown))"
echo "build  : $BUILD_DIR ($BUILD_TYPE, $JOBS jobs)"

launcher=()
if command -v ccache >/dev/null; then
  launcher=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
  ccache --zero-stats >/dev/null || true
fi

cmake -S "$SRC" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  "${launcher[@]}" \
  -DCMAKE_INSTALL_PREFIX=/opt/kicad \
  -DKICAD_BUILD_QA_TESTS=OFF \
  -DKICAD_BUILD_I18N=OFF \
  -DKICAD_USE_SENTRY=OFF \
  -DKICAD_UPDATE_CHECK=OFF \
  -DKICAD_INSTALL_DEMOS=OFF \
  -DKICAD_USE_PCH=ON

# cvpcb: eeschema's ERC (and footprint assignment) loads the cvpcb kiface at run time.
ninja -C "$BUILD_DIR" -j"$JOBS" kicad-cli pcbnew_kiface eeschema_kiface cvpcb_kiface

command -v ccache >/dev/null && ccache --show-stats || true
