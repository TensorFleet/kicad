#!/usr/bin/env python3
"""Classify the functions the wasm module still imports as throwing JS stubs.

    tools/wasm/audit_undefined.py build/wasm/host/kicad_api.wasm

`-sERROR_ON_UNDEFINED_SYMBOLS=0` lets the module link with every GUI function the
kept sources still reference left undefined; wasm-ld turns each one into an import
that throws when it is reached.  Each import is therefore a command that will kill
an instance if a headless code path ever gets to it, and the goal (STATUS.md
follow-up 6) is to get the list to zero and flip the setting off.

This reads the module's own import section -- so it reports what actually survived
the link, not what the link line asked for -- demangles the names with
llvm-cxxfilt, groups them by owning class, and marks the classes that the headless
API handler sources name, i.e. the ones that plausibly sit on a live code path
rather than behind a wxFrame that a headless build never constructs.

Options:
  --api-dirs a,b,c   directories treated as "headless handler code"
                     (default: common/api, pcbnew/api, eeschema/api, include/api,
                      host)
  --list             print every symbol, not just the per-class summary
  --json             machine-readable output
"""

import argparse
import json
import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

DEFAULT_API_DIRS = ['common/api', 'pcbnew/api', 'eeschema/api', 'include/api',
                    'common/jobs', 'pcbnew/pcbnew_jobs_handler.cpp',
                    'eeschema/eeschema_jobs_handler.cpp', 'host/kicad_api_host.cpp']

# The generated stub lists name every undefined symbol by construction, and the
# status notes quote them, so a hit in one of these says nothing about
# reachability.  Same for the .md files.
EXCLUDE_PARTS = ('_link_stubs.cpp', '/setup.h', '.md')

# Emscripten's own runtime imports: these are supplied by kicad_api.js and are not
# missing KiCad symbols at all.
RUNTIME_PREFIXES = (
    'emscripten_', '_emscripten_', '__syscall', 'fd_', 'proc_exit', 'environ_',
    'clock_time_get', 'random_get', 'args_', 'invoke_', '__wasi_', '_setitimer_js',
    '_abort_js', '_tzset_js', '_localtime_js', '_gmtime_js', '_mktime_js',
    '_munmap_js', '_mmap_js', '_msync_js', '_dlopen_js', '_dlsym_js', '_dlinit',
    '_dlsync_js', '_wasmfs_', 'getentropy', 'segfault', 'alignfault',
    'em_', '__assert_fail', '__cxa_', '__resumeException', 'llvm_',
    '_gmtime_js', 'strftime_l', '__call_sighandler', '_setTempRet0',
)


def uleb(b, i):
    r = s = 0
    while True:
        x = b[i]
        i += 1
        r |= (x & 0x7f) << s
        s += 7
        if not x & 0x80:
            return r, i


def wname(b, i):
    n, i = uleb(b, i)
    return b[i:i + n].decode('utf8', 'replace'), i + n


def read_imports(path):
    """Return [(module, field)] for every imported function."""
    blob = open(path, 'rb').read()
    if blob[:4] != b'\0asm':
        sys.exit('%s is not a wasm module' % path)
    i = 8
    while i < len(blob):
        sid = blob[i]
        i += 1
        size, i = uleb(blob, i)
        if sid != 2:
            i += size
            continue
        count, i = uleb(blob, i)
        out = []
        for _ in range(count):
            mod, i = wname(blob, i)
            fld, i = wname(blob, i)
            kind = blob[i]
            i += 1
            if kind == 0:
                _, i = uleb(blob, i)
                out.append((mod, fld))
            elif kind == 1:
                i += 1
                flags = blob[i]
                i += 1
                _, i = uleb(blob, i)
                if flags & 1:
                    _, i = uleb(blob, i)
            elif kind == 2:
                flags = blob[i]
                i += 1
                _, i = uleb(blob, i)
                if flags & 1:
                    _, i = uleb(blob, i)
            elif kind == 3:
                i += 2
            elif kind == 4:
                i += 1
                _, i = uleb(blob, i)
        return out
    return []


def cxxfilt(names):
    for tool in ('/opt/homebrew/opt/llvm/bin/llvm-cxxfilt', 'llvm-cxxfilt', 'c++filt'):
        try:
            p = subprocess.run([tool], input='\n'.join(names), capture_output=True,
                               text=True, check=True)
            return p.stdout.splitlines()
        except (OSError, subprocess.CalledProcessError):
            continue
    return list(names)


def owner(demangled):
    """The class or namespace a demangled name belongs to, best effort."""
    name = demangled
    # strip the return type of a template instantiation, keep it simple
    if '(' in name:
        name = name[:name.index('(')]
    # drop template arguments so wxVector<int>::foo groups under wxVector
    depth, out = 0, []
    for ch in name:
        if ch == '<':
            depth += 1
        elif ch == '>':
            depth = max(0, depth - 1)
        elif depth == 0:
            out.append(ch)
    name = ''.join(out).strip()
    if '::' in name:
        return name.rsplit('::', 1)[0]
    return '(free function)'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('wasm')
    ap.add_argument('--api-dirs', default=','.join(DEFAULT_API_DIRS))
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--json', action='store_true')
    args = ap.parse_args()

    imports = read_imports(args.wasm)
    raw = [f for m, f in imports if m == 'env']
    other = [(m, f) for m, f in imports if m != 'env']

    kicad = [n for n in raw if not n.startswith(RUNTIME_PREFIXES)]
    runtime = [n for n in raw if n.startswith(RUNTIME_PREFIXES)]

    demangled = cxxfilt(kicad)
    pairs = list(zip(kicad, demangled))

    groups = {}
    for mangled, dem in pairs:
        groups.setdefault(owner(dem), []).append((mangled, dem))

    api_dirs = [os.path.join(REPO, d) for d in args.api_dirs.split(',')
                if os.path.exists(os.path.join(REPO, d))]

    def named_by_api(cls):
        """Does any headless handler source mention this class by name?

        A coarse signal, not a call-graph: a hit means a source file that the
        dispatch path actually compiles into the module writes the class name,
        so the stub could plausibly be reached by a command rather than only by
        a wxFrame the headless build never constructs.
        """
        token = cls.split('::')[-1]
        if not token or token == '(free function)':
            return False, []
        try:
            p = subprocess.run(['grep', '-rlw', '--include=*.cpp', '--include=*.h',
                                token] + api_dirs, capture_output=True, text=True)
        except OSError:
            return False, []
        files = [os.path.relpath(x, REPO) for x in p.stdout.split()
                 if not any(part in x for part in EXCLUDE_PARTS)]
        return bool(files), files[:4]

    rows = []
    for cls, syms in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        reachable, where = named_by_api(cls)
        rows.append({'class': cls, 'count': len(syms), 'api_named': bool(reachable),
                     'api_files': where,
                     'symbols': [d for _, d in syms]})

    if args.json:
        json.dump({'module': args.wasm, 'imports_total': len(imports),
                   'runtime_imports': len(runtime) + len(other),
                   'kicad_stub_imports': len(kicad), 'classes': rows},
                  sys.stdout, indent=2)
        print()
        return

    print('%s' % args.wasm)
    print('  %4d function imports total' % len(imports))
    print('  %4d emscripten/wasi runtime imports (supplied by kicad_api.js)'
          % (len(runtime) + len(other)))
    print('  %4d KiCad symbols left undefined -- each one throws if reached'
          % len(kicad))
    print()
    print('  %-52s %5s  %s' % ('owning class', 'stubs', 'named by headless API code?'))
    flagged = 0
    for r in rows:
        mark = 'YES  ' + ' '.join(r['api_files']) if r['api_named'] else ''
        if r['api_named']:
            flagged += r['count']
        print('  %-52s %5d  %s' % (r['class'][:52], r['count'], mark))
        if args.list:
            for s in r['symbols']:
                print('        %s' % s)
    print()
    print('  %d stubs sit in classes the headless API sources name; the rest are '
          'GUI-only.' % flagged)


if __name__ == '__main__':
    main()
