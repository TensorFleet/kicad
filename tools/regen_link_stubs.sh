#!/bin/sh
# Relink the native headless host with no stubs, then regenerate them from what is missing.
#
# The stubs are plain object-file definitions, so a stub for a symbol that has since become
# available would silently shadow the real one.  Always regenerate rather than editing by hand.
#
#   tools/regen_link_stubs.sh [build dir] [reference build dir]
set -e
BUILD=${1:-build/headless}
REF=${2:-build/dev}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

: > host/headless_link_stubs.cpp
ninja -C "$BUILD" kicad-api-host-native > "$TMP/link.log" 2>&1 || true
sed -n '/Undefined symbols/,$p' "$TMP/link.log" > "$TMP/undef.txt"

if ! grep -q '^ *"' "$TMP/undef.txt"; then
    echo "nothing undefined: the headless host links without stubs"
    exit 0
fi

# Sequential on purpose: parallel nm writing to one pipe interleaves its "file.o:" headers.
# Reading every object in the reference build takes minutes; DEVSYMS_CACHE=<path> keeps the
# result across runs, which is worth setting while iterating on the headless guards.
if [ -n "$DEVSYMS_CACHE" ] && [ -s "$DEVSYMS_CACHE" ]; then
    cp "$DEVSYMS_CACHE" "$TMP/devsyms.txt"
else
    find "$REF" -name '*.cpp.o' -print0 | xargs -0 -n 200 nm -gU > "$TMP/devsyms.txt" 2>/dev/null

    if [ -n "$DEVSYMS_CACHE" ]; then
        cp "$TMP/devsyms.txt" "$DEVSYMS_CACHE"
    fi
fi

# With KICAD_HEADLESS_WX_BASE_ONLY the undefined set also contains wxCore symbols, which no
# KiCad object defines.  The wx dylibs are where their mangled spelling and their T/D nature
# come from -- exactly the role build/dev plays for KiCad's own symbols.  Harmless when the
# option is off: nothing in the undefined set matches.
WXLIBDIR=${WXLIBDIR:-$(${WXCONFIG:-wx-config} --libs base 2>/dev/null | tr ' ' '\n' | sed -n 's/^-L//p' | head -1)}

if [ -n "$WXLIBDIR" ] && [ -d "$WXLIBDIR" ]; then
    for _wxlib in "$WXLIBDIR"/libwx_*-3.2.dylib; do
        [ -e "$_wxlib" ] || continue
        nm -gU "$_wxlib" >> "$TMP/devsyms.txt" 2>/dev/null
    done
fi
tools/gen_link_stubs.py "$TMP/undef.txt" "$TMP/devsyms.txt" > host/headless_link_stubs.cpp
echo "stubbed $(grep -c '^KI_HEADLESS_STUB' host/headless_link_stubs.cpp) functions"
ninja -C "$BUILD" kicad-api-host-native
