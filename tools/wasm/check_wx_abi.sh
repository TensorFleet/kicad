#!/usr/bin/env bash
#
# host/wx_headless/wxgui/wx/setup.h compiles KiCad against wx's FULL header set
# with wxUSE_GUI forced to 1, while the library it links against
# (libwx_baseu-3.2-Emscripten.a) was compiled with wxUSE_GUI 0.  That is only
# sound if no class wxBase actually DEFINES changes layout between the two
# settings.  This script proves it by compiling the same sizeof() table twice.
#
# Usage: tools/wasm/check_wx_abi.sh        (exit 0 = identical)
set -euo pipefail

HERE="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
export WASM_REPO_ROOT="${WASM_REPO_ROOT:-$( cd "$HERE/../.." && pwd )}"
# shellcheck source=/dev/null
source "$HERE/env.sh"

TMP="$( mktemp -d )"
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/abi.cpp" <<'CPP'
#include <wx/app.h>
#include <wx/apptrait.h>
#include <wx/event.h>
#include <wx/log.h>
#include <wx/msgout.h>
#include <wx/stdpaths.h>
#include <wx/cmdline.h>
#include <wx/filename.h>
#include <wx/string.h>
#include <wx/config.h>
#include <wx/fileconf.h>
#include <wx/translation.h>
#include <wx/stream.h>
#include <wx/wfstream.h>
#include <wx/textfile.h>
#include <wx/datetime.h>
#include <wx/thread.h>
#include <wx/timer.h>
#include <wx/variant.h>
#include <wx/any.h>
#include <wx/object.h>
#include <cstdio>
#define P(T) printf("%-28s %zu\n", #T, sizeof(T));
int main()
{
    P(wxObject) P(wxEvtHandler) P(wxAppConsole) P(wxConsoleAppTraits) P(wxLog)
    P(wxLogStderr) P(wxMessageOutput) P(wxStandardPaths) P(wxCmdLineParser)
    P(wxFileName) P(wxString) P(wxFileConfig) P(wxTranslations) P(wxTextFile)
    P(wxDateTime) P(wxTimer) P(wxVariant) P(wxAny) P(wxEvent)
    P(wxOutputStream) P(wxFFileOutputStream) P(wxArrayString) P(wxConfigBase)
    return 0;
}
CPP

# 1. exactly what the library was compiled with
em++ -std=c++20 $WASM_CXXFLAGS \
     -I"$WASM_PREFIX/lib/wx/include/base-unicode-static-3.2" \
     -I"$WASM_PREFIX/include/wx-3.2" \
     "$TMP/abi.cpp" -o "$TMP/base.js"
node "$TMP/base.js" > "$TMP/base.txt"

# 2. exactly what KiCad is compiled with
em++ -std=c++20 $WASM_CXXFLAGS \
     -I"$WASM_REPO_ROOT/host/wx_headless/wxgui" \
     -I"$WASM_PREFIX/lib/wx/include" \
     -I"$WASM_PREFIX/include/wx-3.2" \
     "$TMP/abi.cpp" -o "$TMP/gui.js"
node "$TMP/gui.js" > "$TMP/gui.txt"

if diff -u "$TMP/base.txt" "$TMP/gui.txt"; then
    echo "wx ABI: identical under wxUSE_GUI=0 and the KiCad wxUSE_GUI=1 shim"
else
    echo "wx ABI: DIFFERS -- the wxUSE_GUI=1 shim is not safe against this wxBase" >&2
    exit 1
fi
