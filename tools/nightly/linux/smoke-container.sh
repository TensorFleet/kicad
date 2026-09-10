#!/usr/bin/env bash
# Run tools/nightly/smoke.sh on the staged Linux tree inside a clean ubuntu:24.04
# container that only has the packages from runtime-packages.txt, so a library the bundle
# forgot (or a host package the list forgot) fails here and not on a user's machine.
#
# Usage: smoke-container.sh <staging dir>     (KICAD_SRC: the source tree, default: this one)
set -euo pipefail

STAGE="$(cd "$1" && pwd)"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${KICAD_SRC:-$(cd "$HERE/../../.." && pwd)}"
PKGS="$(grep -v '^#' "$HERE/runtime-packages.txt" | tr '\n' ' ')"

docker run --rm \
  -v "$STAGE:/kicad-cli:ro" \
  -v "$SRC/qa/data:/qa/data:ro" \
  -v "$HERE/../smoke.sh:/smoke.sh:ro" \
  -e DEBIAN_FRONTEND=noninteractive \
  ubuntu:24.04 bash -euo pipefail -c "
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends $PKGS > /dev/null
    echo '== unresolved libraries (must be none)'
    LD_LIBRARY_PATH= ldd /kicad-cli/bin/kicad-cli /kicad-cli/bin/*.kiface /kicad-cli/lib/*.so* \
      | grep 'not found' && exit 1 || true
    bash /smoke.sh /kicad-cli/kicad-cli /
  "
