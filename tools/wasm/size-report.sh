#!/usr/bin/env bash
#
# Size report for a built KiCad wasm module.
#
#     tools/wasm/size-report.sh build/wasm/host/kicad_api.wasm [more.wasm ...]
#
# Prints, per module: the raw size, the gzip -9 and brotli -q9 wire sizes, every
# wasm section with its byte count, and the import / export counts.  With a
# symbol map beside the module (kicad_api.js.symbols, produced by relinking with
# `--emit-symbol-map`) it also prints the largest functions by code size, which
# is the only per-symbol attribution available: emcc strips the name section at
# -O1 and above, so wasm-objdump / twiggy have nothing to report.
#
# Nothing here writes into the build tree, so it is safe to run against a build
# another process owns.
set -euo pipefail

[ $# -ge 1 ] || { echo "usage: $0 <module.wasm> [...]" >&2; exit 2; }

TOP_FUNCS="${TOP_FUNCS:-40}"

for WASM in "$@"; do
    [ -f "$WASM" ] || { echo "no such module: $WASM" >&2; exit 1; }

    SYMBOLS="${WASM%.wasm}.js.symbols"
    [ -f "$SYMBOLS" ] || SYMBOLS=""

    JS="${WASM%.wasm}.js"
    [ -f "$JS" ] || JS=""

    echo "=============================================================="
    echo "$WASM"
    echo "=============================================================="

    raw=$( wc -c < "$WASM" | tr -d ' ' )
    gz=$( gzip -9 -c "$WASM" | wc -c | tr -d ' ' )
    if command -v brotli >/dev/null 2>&1; then
        br=$( brotli -9 -c "$WASM" | wc -c | tr -d ' ' )
    else
        br=0
    fi

    printf 'wasm   raw   %12s bytes  (%6.2f MB)\n' "$raw" "$( echo "$raw" | awk '{print $1/1048576}' )"
    printf 'wasm   gzip9 %12s bytes  (%6.2f MB)\n' "$gz"  "$( echo "$gz"  | awk '{print $1/1048576}' )"
    if [ "$br" -gt 0 ]; then
        printf 'wasm   br9   %12s bytes  (%6.2f MB)\n' "$br" "$( echo "$br" | awk '{print $1/1048576}' )"
    fi
    if [ -n "$JS" ]; then
        jraw=$( wc -c < "$JS" | tr -d ' ' )
        jgz=$( gzip -9 -c "$JS" | wc -c | tr -d ' ' )
        printf 'js     raw   %12s bytes   gzip9 %s\n' "$jraw" "$jgz"
    fi
    echo

    TOP_FUNCS="$TOP_FUNCS" SYMBOLS="$SYMBOLS" python3 - "$WASM" <<'PY'
import os, sys

SECTIONS = { 0:'CUSTOM', 1:'TYPE', 2:'IMPORT', 3:'FUNCTION', 4:'TABLE', 5:'MEMORY',
             6:'GLOBAL', 7:'EXPORT', 8:'START', 9:'ELEM', 10:'CODE', 11:'DATA',
             12:'DATACOUNT', 13:'TAG' }

blob = open(sys.argv[1], 'rb').read()
if blob[:4] != b'\0asm':
    sys.exit('not a wasm module')

def uleb(b, i):
    r = s = 0
    while True:
        x = b[i]; i += 1
        r |= (x & 0x7f) << s; s += 7
        if not x & 0x80:
            return r, i

def name(b, i):
    n, i = uleb(b, i)
    return b[i:i+n].decode('utf8', 'replace'), i + n

sections, code_span, import_span, export_span = [], None, None, None
i = 8
while i < len(blob):
    sid = blob[i]; i += 1
    size, i = uleb(blob, i)
    label = SECTIONS.get(sid, str(sid))
    if sid == 0:
        label = 'CUSTOM:' + name(blob, i)[0]
    sections.append((label, size))
    if sid == 10: code_span = (i, size)
    if sid == 2:  import_span = (i, size)
    if sid == 7:  export_span = (i, size)
    i += size

total = sum(s for _, s in sections)
print('sections')
for label, size in sections:
    print('  %-22s %12d  %5.1f%%' % (label, size, 100.0 * size / total))
print('  %-22s %12d' % ('(total, no headers)', total))
print()

# ---- imports: how many functions are still JS stubs, and from where ----------
if import_span:
    i, _ = import_span
    count, i = uleb(blob, i)
    kinds = {0: 'func', 1: 'table', 2: 'mem', 3: 'global', 4: 'tag'}
    tally, funcs = {}, []
    for _ in range(count):
        mod, i = name(blob, i)
        fld, i = name(blob, i)
        kind = blob[i]; i += 1
        if kind == 0:
            _, i = uleb(blob, i); funcs.append((mod, fld))
        elif kind == 1:
            i += 1; flags = blob[i]; i += 1
            _, i = uleb(blob, i)
            if flags & 1: _, i = uleb(blob, i)
        elif kind == 2:
            flags = blob[i]; i += 1
            _, i = uleb(blob, i)
            if flags & 1: _, i = uleb(blob, i)
        elif kind == 3:
            i += 2
        elif kind == 4:
            i += 1; _, i = uleb(blob, i)
        tally[kinds.get(kind, kind)] = tally.get(kinds.get(kind, kind), 0) + 1
    print('imports  %d total  %s' % (count, ', '.join('%s=%d' % kv for kv in sorted(tally.items()))))
    bymod = {}
    for mod, _ in funcs:
        bymod[mod] = bymod.get(mod, 0) + 1
    for mod, n in sorted(bymod.items(), key=lambda kv: -kv[1]):
        print('    %-16s %d function imports' % (mod, n))

if export_span:
    i, _ = export_span
    count, i = uleb(blob, i)
    print('exports  %d' % count)
print()

# ---- per-function code size, when a symbol map exists -----------------------
symfile = os.environ.get('SYMBOLS', '')
if code_span:
    i, _ = code_span
    n, i = uleb(blob, i)
    sizes = []
    for idx in range(n):
        body, j = uleb(blob, i)
        sizes.append((idx, body + (j - i)))
        i = j + body
    print('code     %d function bodies' % n)
    if symfile and os.path.exists(symfile):
        # emcc's symbol map is "<wasm function index>:<name>" per line, and the
        # index is the *function index*, i.e. imported functions come first.
        nimports = sum(1 for lbl, _ in sections if False)  # placeholder, computed below
        names = {}
        for line in open(symfile):
            line = line.strip()
            if ':' not in line: continue
            k, v = line.split(':', 1)
            try: names[int(k)] = v
            except ValueError: pass
        # number of imported functions offsets the defined-function indices
        nimp = 0
        if import_span:
            i2, _ = import_span
            c2, i2 = uleb(blob, i2)
            for _ in range(c2):
                _, i2 = name(blob, i2)
                _, i2 = name(blob, i2)
                kind = blob[i2]; i2 += 1
                if kind == 0:
                    _, i2 = uleb(blob, i2); nimp += 1
                elif kind == 1:
                    i2 += 1; flags = blob[i2]; i2 += 1
                    _, i2 = uleb(blob, i2)
                    if flags & 1: _, i2 = uleb(blob, i2)
                elif kind == 2:
                    flags = blob[i2]; i2 += 1
                    _, i2 = uleb(blob, i2)
                    if flags & 1: _, i2 = uleb(blob, i2)
                elif kind == 3:
                    i2 += 2
                elif kind == 4:
                    i2 += 1; _, i2 = uleb(blob, i2)
        top = int(os.environ.get('TOP_FUNCS', '40'))
        sizes.sort(key=lambda t: -t[1])
        print('largest %d functions (symbol map: %s)' % (top, symfile))
        for idx, sz in sizes[:top]:
            print('  %10d  %s' % (sz, names.get(idx + nimp, '<index %d>' % (idx + nimp))))
    else:
        big = sorted(sizes, key=lambda t: -t[1])[:5]
        print('  no symbol map beside the module; relink with --emit-symbol-map for names')
        print('  largest bodies: ' + ', '.join('%d' % s for _, s in big))
PY
    echo
done
