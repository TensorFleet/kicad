#!/usr/bin/env bash
# Pack a staged kicad-cli tree into the release archive.
#
# Usage: archive.sh <staging dir> <platform> <tag> <out dir>
#   staging dir   the tree produced by */bundle.* (its basename becomes the top-level
#                 directory of the archive, i.e. `kicad-cli/`)
#   platform      linux-x86_64 | macos-arm64 | macos-x86_64 | windows-x86_64
#   tag           nightly-<YYYYMMDD>-<sha10>
#
# Produces <out dir>/kicad-cli-<tag>-<platform>.tar.gz (zip on Windows; the name is what
# tools/nightly/manifest.py parses).
set -euo pipefail

STAGE="$1"
PLATFORM="$2"
TAG="$3"
OUT="$4"

mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
PARENT="$(cd "$(dirname "$STAGE")" && pwd)"
NAME="$(basename "$STAGE")"

case "$PLATFORM" in
  windows-*)
    ARCHIVE="$OUT/kicad-cli-$TAG-$PLATFORM.zip"
    (cd "$PARENT" && 7z a -tzip -mx=5 -bso0 -bsp0 "$ARCHIVE" "$NAME")
    ;;
  *)
    ARCHIVE="$OUT/kicad-cli-$TAG-$PLATFORM.tar.gz"
    # COPYFILE_DISABLE keeps macOS tar from adding ._* resource-fork entries.
    (cd "$PARENT" && COPYFILE_DISABLE=1 tar -czf "$ARCHIVE" "$NAME")
    ;;
esac

ls -l "$ARCHIVE"
