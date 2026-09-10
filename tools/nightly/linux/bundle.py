#!/usr/bin/env python3
"""Stage a relocatable Linux tree of the headless kicad-cli from a build directory.

    kicad-cli/
      kicad-cli              launcher: sets KICAD_STOCK_DATA_HOME, execs bin/kicad-cli
      bin/kicad-cli          the executable (RUNPATH $ORIGIN/../lib)
      bin/_pcbnew.kiface     kifaces; KIWAY loads them from the executable's directory
      bin/_eeschema.kiface
      lib/*.so*              libkicommon/libkigal/libkiapi and every non-system dependency
      share/kicad/schemas    api/schemas from the source tree
      share/kicad/template   resources/project_template
      KICAD_COMMIT VERSION

Dependencies are walked through DT_NEEDED (readelf) and resolved with ldd, stopping at
the libraries the host must provide (see EXCLUDE and runtime-packages.txt): glibc, the
C++ runtime, the GTK/GLib/font stack, OpenGL and the windowing system.  Everything else
is copied to lib/ and given RUNPATH $ORIGIN with patchelf.
"""

import argparse
import os
import re
import shutil
import stat
import subprocess
import sys

# Sonames (prefix match) left to the host.  Mirrors the usual AppImage exclude list: these
# either are the platform (glibc, libstdc++, OpenGL drivers, X/Wayland clients) or have to be
# the same copy the host's GTK uses (GLib, cairo, pango, fontconfig, freetype, harfbuzz), and
# excluding libwebkit2gtk here is what keeps its enormous dependency tree out of the bundle.
EXCLUDE = (
    "ld-linux", "linux-vdso", "libc.so", "libm.so", "libdl.so", "libpthread.so", "librt.so",
    "libresolv.so", "libutil.so", "libnsl.so", "libanl.so", "libmvec.so", "libBrokenLocale.so",
    "libcrypt.so", "libgcc_s.so", "libstdc++.so",
    "libglib-2.0", "libgobject-2.0", "libgio-2.0", "libgmodule-2.0", "libgthread-2.0",
    "libgtk-3", "libgdk-3", "libgdk_pixbuf", "libpango", "libatk", "libcairo", "libepoxy",
    "libfontconfig", "libfreetype", "libharfbuzz", "libfribidi", "libthai", "libdatrie", "libgraphite2",
    "libwebkit2gtk", "libjavascriptcoregtk", "libsoup",
    "libGL.so", "libGLX", "libGLdispatch", "libEGL", "libOpenGL", "libGLESv", "libgbm", "libdrm",
    "libX11", "libXext", "libXrender", "libXi.so", "libXrandr", "libXcursor", "libXfixes", "libXinerama",
    "libXcomposite", "libXdamage", "libXxf86vm", "libXau", "libXdmcp", "libxcb", "libxkbcommon",
    "libwayland", "libdbus-1", "libsecret-1", "libudev", "libsystemd", "libselinux",
    "libffi", "libmount", "libblkid",
    # not libpcre2: wxWidgets links libpcre2-32 itself (wxRegEx), and libpcre2-32-0 is not
    # part of a stock desktop the way GLib's libpcre2-8 is
)


def run(*cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw).stdout


def excluded(soname):
    return any(soname.startswith(p) for p in EXCLUDE)


def needed(path):
    out = run("readelf", "-d", path)
    return re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]", out)


def ldd_map(path):
    """soname -> resolved path for the whole closure of `path` (ldd honours RUNPATH)."""
    m = {}
    for line in run("ldd", path).splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[1] == "=>" and parts[2].startswith("/"):
            m[parts[0]] = parts[2]
    return m


def find_one(build_dir, name):
    hits = []
    for root, dirs, files in os.walk(build_dir):
        dirs[:] = [d for d in dirs if d != "CMakeFiles" and d != "vcpkg_installed"]
        if name in files:
            hits.append(os.path.join(root, name))
    if len(hits) != 1:
        sys.exit(f"expected exactly one {name} under {build_dir}, found: {hits}")
    return hits[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--out", required=True, help="staging directory to create (e.g. dist/kicad-cli)")
    ap.add_argument("--src", default=None, help="source tree (default: derived from this script)")
    args = ap.parse_args()

    src = args.src or os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
    build = os.path.abspath(args.build_dir)
    out = os.path.abspath(args.out)
    bin_dir, lib_dir, share = (os.path.join(out, d) for d in ("bin", "lib", "share/kicad"))

    if os.path.exists(out):
        shutil.rmtree(out)
    for d in (bin_dir, lib_dir, share):
        os.makedirs(d)

    # ---- our own binaries
    roots = []
    for name in ("kicad-cli", "_pcbnew.kiface", "_eeschema.kiface"):
        dst = os.path.join(bin_dir, name)
        shutil.copy2(find_one(build, name), dst)
        roots.append(dst)
    for root, dirs, files in os.walk(build):
        dirs[:] = [d for d in dirs if d != "CMakeFiles" and d != "vcpkg_installed"]
        for f in files:
            if re.match(r"^libki\w+\.so(\.\d+)*$", f):
                p = os.path.join(root, f)
                dst = os.path.join(lib_dir, f)
                if os.path.islink(p):
                    os.symlink(os.readlink(p), dst)
                else:
                    shutil.copy2(p, dst)
                    roots.append(dst)

    # ---- dependency closure, stopping at EXCLUDE
    bundled = {}                       # soname -> path in lib/
    resolver = {}                      # soname -> path on the build machine (from ldd)

    def copy_lib(path, soname):
        dst = os.path.join(lib_dir, soname)
        shutil.copy2(os.path.realpath(path), dst)
        os.chmod(dst, os.stat(dst).st_mode | stat.S_IWUSR)
        bundled[soname] = dst
        return dst

    def walk(queue):
        while queue:
            f = queue.pop()
            resolver.update(ldd_map(f))
            for soname in needed(f):
                if excluded(soname) or soname in bundled:
                    continue
                if os.path.exists(os.path.join(lib_dir, soname)):
                    bundled[soname] = os.path.join(lib_dir, soname)   # a libki* copied above
                    continue
                path = resolver.get(soname)
                if not path:
                    sys.exit(f"{f}: cannot resolve {soname}")
                queue.append(copy_lib(path, soname))

    walk(list(roots))

    # ngspice is dlopen()ed by name at run time (not a DT_NEEDED of anything), so the walker
    # never sees it; bundle it and its closure when the build machine has it.
    ngspice = "libngspice.so.0"
    for d in ("/usr/lib/x86_64-linux-gnu", "/usr/lib/aarch64-linux-gnu", "/usr/lib64", "/usr/lib"):
        p = os.path.join(d, ngspice)
        if ngspice not in bundled and os.path.exists(p):
            walk([copy_lib(p, ngspice)])
            break

    # ---- make it relocatable
    for f in os.listdir(bin_dir):
        subprocess.run(["patchelf", "--set-rpath", "$ORIGIN/../lib", os.path.join(bin_dir, f)], check=True)
    for f in os.listdir(lib_dir):
        p = os.path.join(lib_dir, f)
        if not os.path.islink(p):
            subprocess.run(["patchelf", "--set-rpath", "$ORIGIN", p], check=True)
    for f in os.listdir(bin_dir):
        subprocess.run(["strip", "--strip-unneeded", os.path.join(bin_dir, f)], check=False)
    for f in os.listdir(lib_dir):
        p = os.path.join(lib_dir, f)
        if not os.path.islink(p) and f.startswith("libki"):
            subprocess.run(["strip", "--strip-unneeded", p], check=False)

    # ---- data, launcher, provenance
    shutil.copytree(os.path.join(src, "api", "schemas"), os.path.join(share, "schemas"))
    shutil.copytree(os.path.join(src, "resources", "project_template"), os.path.join(share, "template"))

    launcher = os.path.join(out, "kicad-cli")
    with open(launcher, "w", encoding="utf-8") as f:
        f.write(
            '#!/bin/sh\n'
            '# Launcher for the relocatable kicad-cli bundle: KiCad has its data directory compiled\n'
            '# in (/opt/kicad/share/kicad), so point it at the one next to this script.\n'
            'here="$(cd "$(dirname "$0")" && pwd)"\n'
            'export KICAD_STOCK_DATA_HOME="${KICAD_STOCK_DATA_HOME:-$here/share/kicad}"\n'
            'exec "$here/bin/kicad-cli" "$@"\n'
        )
    os.chmod(launcher, 0o755)

    sha = os.environ.get("NIGHTLY_SHA") or run("git", "-C", src, "rev-parse", "HEAD").strip()
    version = os.environ.get("NIGHTLY_VERSION") or run("git", "-C", src, "describe", "--match", "[0-9]*", "--always").strip()
    with open(os.path.join(out, "KICAD_COMMIT"), "w") as f:
        f.write(sha + "\n")
    with open(os.path.join(out, "VERSION"), "w") as f:
        f.write(version + "\n")

    total = sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(out) for f in fs)
    print(f"staged {out}: {len(bundled)} bundled libraries, {total / 1e6:.0f} MB")
    for name in sorted(bundled):
        print(f"  lib/{name}")


if __name__ == "__main__":
    main()
