#!/usr/bin/env bash
# Stage a relocatable macOS tree of the headless kicad-cli from a build directory:
#
#   kicad-cli/
#     kicad-cli                                  launcher: execs the binary in the bundle
#     KiCad.app/Contents/MacOS/kicad-cli
#     KiCad.app/Contents/PlugIns/_*.kiface       KIWAY loads kifaces from <bundle>/Contents/PlugIns
#     KiCad.app/Contents/Frameworks/*.dylib      libki* and every Homebrew dependency
#     KiCad.app/Contents/SharedSupport/          schemas + template (GetOSXKicadDataDir)
#     KICAD_COMMIT VERSION
#
# Every non-system dependency (otool -L, walked transitively) is copied into Frameworks,
# all load commands are rewritten to @rpath/<name>, the rpaths point into Frameworks and
# every Mach-O is re-signed ad hoc (a modified binary must be re-signed on Apple silicon).
#
# Usage: bundle.sh <build dir> <staging dir>     (KICAD_SRC: the source tree, default: this one)
set -euo pipefail

BUILD="$(cd "$1" && pwd)"
OUT="$2"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${KICAD_SRC:-$(cd "$HERE/../../.." && pwd)}"
BREW="$(brew --prefix)"

APP_SRC="$BUILD/kicad/KiCad.app"
rm -rf "$OUT"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
APP="$OUT/KiCad.app"
MACOS="$APP/Contents/MacOS"
PLUGINS="$APP/Contents/PlugIns"
FW="$APP/Contents/Frameworks"
SUPPORT="$APP/Contents/SharedSupport"
mkdir -p "$MACOS" "$PLUGINS" "$FW" "$SUPPORT"

cp "$APP_SRC/Contents/MacOS/kicad-cli" "$MACOS/"
cp "$APP_SRC/Contents/PlugIns/_pcbnew.kiface" "$APP_SRC/Contents/PlugIns/_eeschema.kiface" \
   "$APP_SRC/Contents/PlugIns/_cvpcb.kiface" "$PLUGINS/"
cp "$APP_SRC"/Contents/Frameworks/libki*.dylib "$FW/"
[ -f "$APP_SRC/Contents/Info.plist" ] && cp "$APP_SRC/Contents/Info.plist" "$APP/Contents/"
cp -R "$SRC/api/schemas" "$SUPPORT/schemas"
cp -R "$SRC/resources/project_template" "$SUPPORT/template"
chmod -R u+w "$APP"

is_system() { case "$1" in /usr/lib/*|/System/*) return 0 ;; esac; return 1; }

# Dependencies of a Mach-O file, without its own install name.
deps_of() {
  local f="$1" self
  self="$(basename "$f")"
  otool -L "$f" | tail -n +2 | awk '{print $1}' | while read -r dep; do
    [ "$(basename "$dep")" = "$self" ] && continue
    echo "$dep"
  done
}

rpaths_of() {
  otool -l "$1" | awk '/LC_RPATH/ {getline; getline; print $2}'
}

# Turn a load command into a file on this machine.
resolve() {
  local dep="$1" from="$2" base d
  case "$dep" in
    /*) echo "$dep"; return ;;
  esac
  base="$(basename "$dep")"
  for d in "$(dirname "$from")" $(rpaths_of "$from") "$BREW/lib" "$BREW"/opt/*/lib; do
    [ -f "$d/$base" ] && { echo "$d/$base"; return; }
  done
  echo ""
}

# ---- collect the closure of Homebrew (and other non-system) libraries into Frameworks
queue=("$MACOS/kicad-cli" "$PLUGINS"/*.kiface "$FW"/*.dylib)
while [ ${#queue[@]} -gt 0 ]; do
  f="${queue[${#queue[@]}-1]}"
  unset 'queue[${#queue[@]}-1]'
  while read -r dep; do
    [ -n "$dep" ] || continue
    is_system "$dep" && continue
    base="$(basename "$dep")"
    [ -e "$FW/$base" ] && continue
    path="$(resolve "$dep" "$f")"
    [ -n "$path" ] || { echo "$f: cannot resolve $dep" >&2; exit 1; }
    cp "$(python3 -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$path")" "$FW/$base"
    chmod u+w "$FW/$base"
    queue+=("$FW/$base")
  done < <(deps_of "$f")
done

# ---- rewrite load commands and rpaths, then sign
fix() {
  local f="$1" kind="$2" dep base rp
  while read -r dep; do
    [ -n "$dep" ] || continue
    is_system "$dep" && continue
    base="$(basename "$dep")"
    [ "$dep" = "@rpath/$base" ] || install_name_tool -change "$dep" "@rpath/$base" "$f"
  done < <(deps_of "$f")
  while read -r rp; do
    [ -n "$rp" ] && install_name_tool -delete_rpath "$rp" "$f" 2>/dev/null || true
  done < <(rpaths_of "$f" | sort -u)
  case "$kind" in
    exe)   install_name_tool -add_rpath "@executable_path/../Frameworks" "$f" ;;
    plugin) install_name_tool -add_rpath "@loader_path/../Frameworks" "$f" ;;
    lib)   install_name_tool -id "@rpath/$(basename "$f")" "$f"
           # dyld also searches the rpaths of the executable that (transitively) loaded a
           # library, so this one is a convenience; a bottle linked without header padding
           # may not have room for it.
           install_name_tool -add_rpath "@loader_path" "$f" 2>/dev/null \
             || echo "note: no room for an rpath in $(basename "$f")" ;;
  esac
  codesign --force --sign - "$f" 2>/dev/null
}

for f in "$FW"/*.dylib; do fix "$f" lib; done
for f in "$PLUGINS"/*.kiface; do fix "$f" plugin; done
fix "$MACOS/kicad-cli" exe

# Nothing may still point at Homebrew or the build tree.
if otool -L "$MACOS/kicad-cli" "$PLUGINS"/*.kiface "$FW"/*.dylib | grep -E "$BREW|$BUILD"; then
  echo "unrelocated load commands remain" >&2
  exit 1
fi

# ---- launcher and provenance
cat > "$OUT/kicad-cli" <<'EOF'
#!/bin/sh
# Launcher for the relocatable kicad-cli bundle (kifaces and data are found relative to
# the real executable inside KiCad.app, so exec it rather than symlinking it).
here="$(cd "$(dirname "$0")" && pwd)"
exec "$here/KiCad.app/Contents/MacOS/kicad-cli" "$@"
EOF
chmod 755 "$OUT/kicad-cli"

sha="${NIGHTLY_SHA:-$(git -C "$SRC" rev-parse HEAD)}"
version="${NIGHTLY_VERSION:-$(git -C "$SRC" describe --match '[0-9]*' --always)}"
echo "$sha" > "$OUT/KICAD_COMMIT"
echo "$version" > "$OUT/VERSION"

echo "staged $OUT: $(ls "$FW" | wc -l | tr -d ' ') libraries in Frameworks, $(du -sh "$OUT" | cut -f1)"
bash "$HERE/../smoke.sh" "$OUT/kicad-cli" "$SRC"
