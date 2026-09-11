#!/usr/bin/env bash
#
# Package the wasm build of KiCad's headless API core as a release artifact.
#
#   tools/wasm/package.sh                       # build/wasm/host -> build/wasm-release/
#   tools/wasm/package.sh --tag fp-pcb/2026-09-09-wasm
#   WASM_BUILD_DIR=... OUT_DIR=... tools/wasm/package.sh
#
# Produces  $OUT_DIR/kicad-wasm-<tag-slug>.tar.gz  containing, flat at the root:
#
#   kicad_api.js        the Emscripten loader (MODULARIZE, EXPORT_ES6)
#   kicad_api.wasm      the module
#   kicad_api.data      only when the build used --preload-file (it does not today)
#   kicad-wasm.json     manifest: fork commit, tag, toolchain + dependency versions, sizes
#   SHA256SUMS          sha256 of every other file in the tarball, `sha256sum -c` format
#
# The tag slug is the tag with '/' replaced by '-', because the alignment tags are
# `fp-pcb/<date>-<name>` and a release asset name cannot contain a slash:
#
#   fp-pcb/2026-09-09-wasm  ->  kicad-wasm-fp-pcb-2026-09-09-wasm.tar.gz
#
# packages/kicad-wasm/scripts/fetch.ts in fab_pcb derives the same name, extracts the
# tarball, and verifies SHA256SUMS.  Keep the two in step.
set -euo pipefail

HERE="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
REPO_ROOT="${WASM_REPO_ROOT:-$( cd "$HERE/../.." && pwd )}"

BUILD_DIR="${WASM_BUILD_DIR:-$REPO_ROOT/build/wasm}"
HOST_DIR=""
OUT_DIR="${OUT_DIR:-$REPO_ROOT/build/wasm-release}"
TAG="${TAG:-}"

while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --host-dir)  HOST_DIR="$2";  shift 2 ;;
        --out)       OUT_DIR="$2";   shift 2 ;;
        --tag)       TAG="$2";       shift 2 ;;
        -h|--help)   sed -n '2,30p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

# The module lands in <build>/host; accept either that or the directory itself.
if [ -z "$HOST_DIR" ]; then
    if [ -f "$BUILD_DIR/host/kicad_api.wasm" ]; then
        HOST_DIR="$BUILD_DIR/host"
    else
        HOST_DIR="$BUILD_DIR"
    fi
fi

die() { printf '\033[31merror: %s\033[0m\n' "$*" >&2; exit 1; }

REQUIRED=( kicad_api.js kicad_api.wasm )
OPTIONAL=( kicad_api.data kicad_api.worker.js )

for f in "${REQUIRED[@]}"; do
    [ -f "$HOST_DIR/$f" ] || die "no $f in $HOST_DIR -- build it first:
     tools/wasm/configure.sh && ninja -C ${BUILD_DIR#"$REPO_ROOT"/} kicad_api"
done

# ---------------------------------------------------------------------------
# identity
# ---------------------------------------------------------------------------
git_out() { git -C "$REPO_ROOT" "$@" 2>/dev/null || true; }

COMMIT="$( git_out rev-parse HEAD )"
COMMIT="${COMMIT:-unknown}"
DIRTY="false"
if [ -n "$( git_out status --porcelain )" ]; then DIRTY="true"; fi

if [ -z "$TAG" ]; then
    # An exact fp-pcb/* tag on HEAD if there is one (the release case), else a description.
    TAG="$( git_out describe --tags --exact-match --match 'fp-pcb/*' HEAD )"
fi
if [ -z "$TAG" ]; then
    TAG="$( git_out describe --tags --always --match 'fp-pcb/*' HEAD )"
fi
TAG="${TAG:-${COMMIT:0:12}}"
TAG_SLUG="${TAG//\//-}"

# ---------------------------------------------------------------------------
# toolchain / dependency versions (best effort: this may run without emcc)
# ---------------------------------------------------------------------------
version_from_deps() { sed -n "s/^$1=\([^ ]*\).*/\1/p" "$HERE/build-deps.sh" | head -1; }

PROTOBUF_VERSION="$( version_from_deps PROTOBUF_VERSION )"
ABSEIL_VERSION="$( version_from_deps ABSEIL_VERSION )"
ZSTD_VERSION="$( version_from_deps ZSTD_VERSION )"
WX_VERSION="$( version_from_deps WX_VERSION )"

EMSCRIPTEN_VERSION="${EMSDK_VERSION:-}"
if [ -z "$EMSCRIPTEN_VERSION" ] && command -v emcc >/dev/null 2>&1; then
    # "emcc (Emscripten gcc/clang-like replacement + linker ...) 6.0.9-git"
    EMSCRIPTEN_VERSION="$( emcc --version | head -1 | sed -n 's/.*) \([0-9][^ ]*\).*/\1/p' )"
fi
EMSCRIPTEN_VERSION="${EMSCRIPTEN_VERSION:-unknown}"

HOST_PROTOC="unknown"
if command -v protoc >/dev/null 2>&1; then HOST_PROTOC="$( protoc --version | awk '{print $2}' )"; fi

BUILD_DATE="$( date -u +%Y-%m-%dT%H:%M:%SZ )"

# ---------------------------------------------------------------------------
# stage
# ---------------------------------------------------------------------------
STAGE="$( mktemp -d "${TMPDIR:-/tmp}/kicad-wasm-pkg.XXXXXX" )"
trap 'rm -rf "$STAGE"' EXIT

FILES=()
for f in "${REQUIRED[@]}" "${OPTIONAL[@]}"; do
    [ -f "$HOST_DIR/$f" ] || continue
    cp "$HOST_DIR/$f" "$STAGE/$f"
    FILES+=( "$f" )
done

file_size()   { wc -c < "$1" | tr -d ' '; }
file_sha256() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}'
    else shasum -a 256 "$1" | awk '{print $1}'; fi
}

# manifest ------------------------------------------------------------------
{
    printf '{\n'
    printf '  "schema": 1,\n'
    printf '  "name": "kicad-wasm",\n'
    printf '  "tag": "%s",\n' "$TAG"
    printf '  "asset": "kicad-wasm-%s.tar.gz",\n' "$TAG_SLUG"
    printf '  "commit": "%s",\n' "$COMMIT"
    printf '  "dirty": %s,\n' "$DIRTY"
    printf '  "buildDate": "%s",\n' "$BUILD_DATE"
    printf '  "buildHost": "%s",\n' "$( uname -s )-$( uname -m )"
    printf '  "toolchain": {\n'
    printf '    "emscripten": "%s",\n' "$EMSCRIPTEN_VERSION"
    printf '    "hostProtoc": "%s"\n' "$HOST_PROTOC"
    printf '  },\n'
    printf '  "dependencies": {\n'
    printf '    "protobuf": "%s",\n' "$PROTOBUF_VERSION"
    printf '    "abseil": "%s",\n' "$ABSEIL_VERSION"
    printf '    "zstd": "%s",\n' "$ZSTD_VERSION"
    printf '    "wxWidgets": "%s"\n' "$WX_VERSION"
    printf '  },\n'
    printf '  "files": [\n'
    n=${#FILES[@]}; i=0
    for f in "${FILES[@]}"; do
        i=$(( i + 1 ))
        printf '    { "name": "%s", "size": %s, "sha256": "%s" }' \
            "$f" "$( file_size "$STAGE/$f" )" "$( file_sha256 "$STAGE/$f" )"
        [ "$i" -lt "$n" ] && printf ','
        printf '\n'
    done
    printf '  ]\n'
    printf '}\n'
} > "$STAGE/kicad-wasm.json"

# checksums -----------------------------------------------------------------
( cd "$STAGE" && for f in "${FILES[@]}" kicad-wasm.json; do
        printf '%s  %s\n' "$( file_sha256 "$f" )" "$f"
  done ) > "$STAGE/SHA256SUMS"

# tarball -------------------------------------------------------------------
mkdir -p "$OUT_DIR"
TARBALL="$OUT_DIR/kicad-wasm-$TAG_SLUG.tar.gz"
rm -f "$TARBALL"
tar -czf "$TARBALL" -C "$STAGE" "${FILES[@]}" kicad-wasm.json SHA256SUMS

cp "$STAGE/kicad-wasm.json" "$OUT_DIR/kicad-wasm.json"

printf '\n\033[1m%s\033[0m\n' "$TARBALL"
printf '  tag        %s\n' "$TAG"
DIRTY_NOTE=""
if [ "$DIRTY" = true ]; then DIRTY_NOTE=" (dirty tree)"; fi
printf '  commit     %s%s\n' "${COMMIT:0:12}" "$DIRTY_NOTE"
printf '  emscripten %s\n' "$EMSCRIPTEN_VERSION"
mib() { awk -v b="$1" 'BEGIN { printf "%.2f", b / 1048576 }'; }
for f in "${FILES[@]}"; do
    printf '  %-20s %8s MiB\n' "$f" "$( mib "$( file_size "$STAGE/$f" )" )"
done
printf '  %-20s %8s MiB\n' "(tarball)" "$( mib "$( file_size "$TARBALL" )" )"

# For the workflow: name and path of what was produced.
if [ -n "${GITHUB_OUTPUT:-}" ]; then
    {
        echo "tarball=$TARBALL"
        echo "asset=kicad-wasm-$TAG_SLUG.tar.gz"
        echo "tag=$TAG"
        echo "tag_slug=$TAG_SLUG"
    } >> "$GITHUB_OUTPUT"
fi
