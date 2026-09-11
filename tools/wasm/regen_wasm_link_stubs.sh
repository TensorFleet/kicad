#!/usr/bin/env bash
#
# Regenerate host/wasm/wasm_link_stubs.cpp from the current wasm link line.
#
#     tools/wasm/regen_wasm_link_stubs.sh [build dir]        (default build/wasm)
#
# Run this after ANY change that adds or removes a definition -- a stale stub silently shadows a
# real one, which is the single most expensive mistake on this branch (host/STATUS-native-gate.md
# records two cycles lost to it on the native side).
set -euo pipefail

HERE="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
ROOT="$( cd "$HERE/../.." && pwd )"
BUILD="${1:-$ROOT/build/wasm}"
OUT="$ROOT/host/wasm/wasm_link_stubs.cpp"

# Everything the module links: the archives and the one loose object.  Taken from ninja rather
# than hand-listed so it cannot drift.
# macOS ships bash 3.2, which has no mapfile.
FILTERED=()
while IFS= read -r i; do
    case "$i" in
        "" | *wasm_link_stubs.cpp.o ) continue ;;
    esac
    case "$i" in
        /* ) FILTERED+=( "$i" ) ;;
        *  ) FILTERED+=( "$BUILD/$i" ) ;;
    esac
done < <(
    ninja -C "$BUILD" -t commands host/kicad_api.js 2>/dev/null \
        | grep -E 'em\+\+ ' | tail -1 | tr ' ' '\n' \
        | grep -E '\.(a|o)$' | sort -u
)

[ "${#FILTERED[@]}" -gt 0 ] || { echo "no archives on the kicad_api link line" >&2; exit 1; }

# emcc appends libc++, libc++abi, compiler-rt and libc itself; they are not on the ninja
# command line, and without them std::cout / std::nothrow / std::*::id look undefined.
EM_SYSROOT="${EM_SYSROOT:-$( em-config CACHE )/sysroot}"
SYSLIBS=()
while IFS= read -r l; do
    [ -n "$l" ] && SYSLIBS+=( "$l" )
done < <( ls "$EM_SYSROOT"/lib/wasm32-emscripten/*.a 2>/dev/null )

"$HERE/gen_wasm_link_stubs.py" "$OUT" "${FILTERED[@]}" --defined-only "${SYSLIBS[@]}"
