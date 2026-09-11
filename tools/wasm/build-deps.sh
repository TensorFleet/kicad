#!/usr/bin/env bash
#
# Build KiCad's third-party dependencies for WebAssembly (wasm32-emscripten).
#
#   tools/wasm/build-deps.sh              # everything, then the smoke test
#   tools/wasm/build-deps.sh abseil       # one stage
#   tools/wasm/build-deps.sh protobuf wx  # several stages
#   tools/wasm/build-deps.sh smoke        # just re-run the smoke test
#
# Stages: fetch abseil protobuf zstd wx smoke
#
# Everything lands in build/wasm-deps/:
#   src/     downloaded + unpacked sources (patched in place)
#   build/   cmake build trees
#   prefix/  the install prefix that tools/wasm/env.sh points KiCad at
#   logs/    per-stage configure/build/install logs
#   smoke/   smoke test output
#
# Already-completed stages are skipped; delete build/wasm-deps/.stamp-<stage>
# (or the whole prefix) to force a rebuild.
#
# ---------------------------------------------------------------------------
# Things that were NOT obvious, and that this script therefore encodes:
#
#  1. -fwasm-exceptions everywhere. C++ exceptions are required by KiCad, and
#     every object in the prefix must agree on the exception ABI. Mixing
#     -fexceptions (JS-based EH) with -fwasm-exceptions produces link failures
#     that name personality/landing-pad symbols and never mention the flag.
#
#  2. -sWASM_LEGACY_EXCEPTIONS=1, passed explicitly. See env.sh - it is the
#     emscripten 6.0.9 default anyway, Node 20 cannot instantiate a module
#     built with the standardised encoding (=0), and stating it explicitly
#     works around an emscripten bug in the freetype port.
#
#  3. -DCMAKE_CXX_SCAN_FOR_MODULES=OFF on the FIRST configure of every project
#     that uses CMAKE_CXX_STANDARD>=20 with Ninja. brew's emscripten ships an
#     emscan-deps wrapper whose clang-scan-deps does not exist, so CMake's
#     C++20 module scanning fails every compile - and, much worse, silently
#     fails every check_cxx_source_compiles() probe. Adding the flag on a
#     later configure does not help: the wrong probe results are already
#     cached (this is how wxWidgets ends up believing size_t is unsigned int).
#
#  4. CMAKE_FIND_ROOT_PATH, not just CMAKE_PREFIX_PATH. The emscripten
#     toolchain sets CMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY, so find_package()
#     ignores CMAKE_PREFIX_PATH entries outside the find roots. Without it
#     protobuf silently FetchContent-downloads its own abseil and you end up
#     with two of them.
#
#  5. wxWidgets needs a patch for wasm32 pointer size - see
#     tools/wasm/patches/ - plus explicit -DwxUSE_* for every feature that
#     transitively depends on the ones we turn off, plus the system (port)
#     zlib rather than wx's bundled zlib, which does not compile here.
# ---------------------------------------------------------------------------

set -euo pipefail

HERE="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
REPO_ROOT="$( cd "$HERE/../.." && pwd )"

# ---------------------------------------------------------------------------
# versions  (protobuf MUST match the host protoc; abseil is protobuf's own pin)
# ---------------------------------------------------------------------------
PROTOBUF_VERSION=36.1                 # -> C++ runtime 7.36.1
ABSEIL_VERSION=20250512.1             # protobuf 36.1 MODULE.bazel pin
ZSTD_VERSION=1.5.7
WX_VERSION=3.2.11

JOBS="${JOBS:-4}"

# shellcheck source=env.sh
source "$HERE/env.sh"

SRC="$WASM_DEPS_ROOT/src"
BLD="$WASM_DEPS_ROOT/build"
LOGS="$WASM_DEPS_ROOT/logs"
mkdir -p "$SRC" "$BLD" "$LOGS" "$WASM_PREFIX"

say()  { printf '\n\033[1m== %s\033[0m\n' "$*"; }
die()  { printf '\033[31merror: %s\033[0m\n' "$*" >&2; exit 1; }

stamp_file() { echo "$WASM_DEPS_ROOT/.stamp-$1"; }
is_done()    { [ -f "$( stamp_file "$1" )" ]; }
mark_done()  { date > "$( stamp_file "$1" )"; }

fetch() {
    local url=$1 file=$2
    if [ -f "$SRC/$file" ]; then
        echo "have $file"
    else
        echo "fetching $file"
        curl -fsSL -o "$SRC/$file.part" "$url"
        mv "$SRC/$file.part" "$SRC/$file"
    fi
}

# ---------------------------------------------------------------------------
# preflight
# ---------------------------------------------------------------------------
preflight() {
    command -v emcc     >/dev/null || die "emcc not found - install/activate emscripten"
    command -v emcmake  >/dev/null || die "emcmake not found"
    command -v cmake    >/dev/null || die "cmake not found"
    command -v ninja    >/dev/null || die "ninja not found"
    command -v protoc   >/dev/null || die "host protoc not found"

    local host_protoc
    host_protoc=$( protoc --version | awk '{print $2}' )

    if [ "$host_protoc" != "$PROTOBUF_VERSION" ]; then
        die "host protoc is $host_protoc but this script builds wasm protobuf $PROTOBUF_VERSION.
     They must match exactly: generated .pb.cc carries a runtime version check
     that aborts at static-init time otherwise. Either install protobuf
     $PROTOBUF_VERSION on the host, or set PROTOBUF_VERSION here to $host_protoc."
    fi

    echo "emcc:   $( emcc --version | head -1 )"
    echo "protoc: $( protoc --version )  (host)"
    echo "cmake:  $( cmake --version | head -1 )"
    echo "prefix: $WASM_PREFIX"
    echo "flags:  $WASM_CXXFLAGS"
}

# ---------------------------------------------------------------------------
# stage: fetch
# ---------------------------------------------------------------------------
stage_fetch() {
    say "fetch"

    fetch "https://github.com/abseil/abseil-cpp/releases/download/${ABSEIL_VERSION}/abseil-cpp-${ABSEIL_VERSION}.tar.gz" \
          "abseil-cpp-${ABSEIL_VERSION}.tar.gz"
    fetch "https://github.com/protocolbuffers/protobuf/releases/download/v${PROTOBUF_VERSION}/protobuf-${PROTOBUF_VERSION}.tar.gz" \
          "protobuf-${PROTOBUF_VERSION}.tar.gz"
    fetch "https://github.com/facebook/zstd/releases/download/v${ZSTD_VERSION}/zstd-${ZSTD_VERSION}.tar.gz" \
          "zstd-${ZSTD_VERSION}.tar.gz"
    fetch "https://github.com/wxWidgets/wxWidgets/releases/download/v${WX_VERSION}/wxWidgets-${WX_VERSION}.tar.bz2" \
          "wxWidgets-${WX_VERSION}.tar.bz2"

    [ -d "$SRC/abseil-cpp-${ABSEIL_VERSION}" ] || tar xzf "$SRC/abseil-cpp-${ABSEIL_VERSION}.tar.gz" -C "$SRC"
    [ -d "$SRC/protobuf-${PROTOBUF_VERSION}" ] || tar xzf "$SRC/protobuf-${PROTOBUF_VERSION}.tar.gz" -C "$SRC"
    [ -d "$SRC/zstd-${ZSTD_VERSION}" ]         || tar xzf "$SRC/zstd-${ZSTD_VERSION}.tar.gz" -C "$SRC"

    if [ ! -d "$SRC/wxWidgets-${WX_VERSION}" ]; then
        tar xjf "$SRC/wxWidgets-${WX_VERSION}.tar.bz2" -C "$SRC"
        say "patching wxWidgets"
        patch -p1 -d "$SRC/wxWidgets-${WX_VERSION}" \
            < "$HERE/patches/wxWidgets-${WX_VERSION}-emscripten-pointer-size.patch"
    fi

    mark_done fetch
}

# ---------------------------------------------------------------------------
# stage: abseil
# ---------------------------------------------------------------------------
stage_abseil() {
    say "abseil-cpp ${ABSEIL_VERSION}"

    emcmake cmake -S "$SRC/abseil-cpp-${ABSEIL_VERSION}" -B "$BLD/abseil" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$WASM_PREFIX" \
        -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
        -DCMAKE_TRY_COMPILE_PLATFORM_VARIABLES=CMAKE_CXX_SCAN_FOR_MODULES \
        -DABSL_PROPAGATE_CXX_STD=ON \
        -DABSL_ENABLE_INSTALL=ON \
        -DBUILD_TESTING=OFF \
        -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_CXX_STANDARD=20 \
        -DCMAKE_C_FLAGS="$WASM_CFLAGS" \
        -DCMAKE_CXX_FLAGS="$WASM_CXXFLAGS" \
        > "$LOGS/abseil-configure.log" 2>&1

    cmake --build   "$BLD/abseil" -j"$JOBS" > "$LOGS/abseil-build.log"   2>&1
    cmake --install "$BLD/abseil"           > "$LOGS/abseil-install.log" 2>&1

    ls "$WASM_PREFIX/lib/cmake/absl/abslConfig.cmake" >/dev/null
    mark_done abseil
}

# ---------------------------------------------------------------------------
# stage: protobuf
# ---------------------------------------------------------------------------
stage_protobuf() {
    say "protobuf ${PROTOBUF_VERSION}"

    # protobuf_LOCAL_DEPENDENCIES_ONLY=ON turns a missing abseil into a hard
    # error instead of a FetchContent download, so a broken find_package can
    # never quietly give us a second abseil.
    emcmake cmake -S "$SRC/protobuf-${PROTOBUF_VERSION}" -B "$BLD/protobuf" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$WASM_PREFIX" \
        -DCMAKE_PREFIX_PATH="$WASM_PREFIX" \
        -DCMAKE_FIND_ROOT_PATH="$WASM_PREFIX" \
        -Dabsl_DIR="$WASM_PREFIX/lib/cmake/absl" \
        -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
        -DCMAKE_TRY_COMPILE_PLATFORM_VARIABLES=CMAKE_CXX_SCAN_FOR_MODULES \
        -Dprotobuf_BUILD_TESTS=OFF \
        -Dprotobuf_BUILD_PROTOC_BINARIES=OFF \
        -Dprotobuf_LOCAL_DEPENDENCIES_ONLY=ON \
        -Dprotobuf_BUILD_SHARED_LIBS=OFF \
        -Dprotobuf_WITH_ZLIB=ON \
        -Dprotobuf_BUILD_LIBUPB=ON \
        -Dprotobuf_INSTALL=ON \
        -DCMAKE_CXX_STANDARD=20 \
        -DCMAKE_C_FLAGS="$WASM_CFLAGS" \
        -DCMAKE_CXX_FLAGS="$WASM_CXXFLAGS" \
        > "$LOGS/protobuf-configure.log" 2>&1

    if grep -q "Fallback to downloading Abseil" "$LOGS/protobuf-configure.log"; then
        die "protobuf did not find our abseil and fell back to downloading one - check CMAKE_FIND_ROOT_PATH"
    fi

    cmake --build   "$BLD/protobuf" -j"$JOBS" > "$LOGS/protobuf-build.log"   2>&1
    cmake --install "$BLD/protobuf"           > "$LOGS/protobuf-install.log" 2>&1

    ls "$WASM_PREFIX/lib/libprotobuf.a" >/dev/null
    mark_done protobuf
}

# ---------------------------------------------------------------------------
# stage: zstd
# ---------------------------------------------------------------------------
stage_zstd() {
    say "zstd ${ZSTD_VERSION}"

    emcmake cmake -S "$SRC/zstd-${ZSTD_VERSION}/build/cmake" -B "$BLD/zstd" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$WASM_PREFIX" \
        -DZSTD_BUILD_PROGRAMS=OFF \
        -DZSTD_BUILD_SHARED=OFF \
        -DZSTD_BUILD_STATIC=ON \
        -DZSTD_BUILD_TESTS=OFF \
        -DZSTD_MULTITHREAD_SUPPORT=OFF \
        -DCMAKE_C_FLAGS="$WASM_CFLAGS" \
        -DCMAKE_CXX_FLAGS="$WASM_CXXFLAGS" \
        > "$LOGS/zstd-configure.log" 2>&1

    cmake --build   "$BLD/zstd" -j"$JOBS" > "$LOGS/zstd-build.log"   2>&1
    cmake --install "$BLD/zstd"           > "$LOGS/zstd-install.log" 2>&1

    ls "$WASM_PREFIX/lib/libzstd.a" >/dev/null
    mark_done zstd
}

# ---------------------------------------------------------------------------
# stage: wxWidgets (base only)
# ---------------------------------------------------------------------------
stage_wx() {
    say "wxWidgets ${WX_VERSION} (base, no GUI)"

    # Notes on the option list:
    #  * wxUSE_ZLIB=sys uses the emscripten zlib port. The bundled zlib does
    #    not build: its gz*.c call lseek/read/close without declaring them,
    #    which is a hard error under a modern clang.
    #  * XML_DEV_URANDOM: wx's bundled expat refuses to compile unless told
    #    where to get entropy. emscripten has no getrandom and no
    #    arc4random_buf, but it does have /dev/urandom.
    #  * Turning off wxUSE_SOCKETS/wxUSE_DYNLIB_CLASS is not enough on its own:
    #    wx/chkconf.h errors out unless the features that depend on them
    #    (wxUSE_PROTOCOL*, wxUSE_URL, wxUSE_FS_INET, wxUSE_DYNAMIC_LOADER) are
    #    turned off explicitly too.
    emcmake cmake -S "$SRC/wxWidgets-${WX_VERSION}" -B "$BLD/wx" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$WASM_PREFIX" \
        -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
        -DCMAKE_TRY_COMPILE_PLATFORM_VARIABLES=CMAKE_CXX_SCAN_FOR_MODULES \
        -DwxBUILD_SHARED=OFF \
        -DwxUSE_GUI=OFF \
        -DwxBUILD_MONOLITHIC=OFF \
        -DwxBUILD_PRECOMP=OFF \
        -DwxBUILD_SAMPLES=OFF \
        -DwxBUILD_TESTS=OFF \
        -DwxBUILD_DEMOS=OFF \
        -DwxUSE_SOCKETS=OFF \
        -DwxUSE_THREADS=OFF \
        -DwxUSE_LIBICONV=OFF \
        -DwxUSE_ZLIB=sys \
        -DwxUSE_EXPAT=builtin \
        -DwxUSE_REGEX=builtin \
        -DwxUSE_LIBLZMA=OFF \
        -DwxUSE_SECRETSTORE=OFF \
        -DwxUSE_WEBREQUEST=OFF \
        -DwxUSE_XML=ON \
        -DwxUSE_UNICODE=ON \
        -DwxUSE_DYNLIB_CLASS=OFF \
        -DwxUSE_DYNAMIC_LOADER=OFF \
        -DwxUSE_STACKWALKER=OFF \
        -DwxUSE_SNGLINST_CHECKER=OFF \
        -DwxUSE_FSWATCHER=OFF \
        -DwxUSE_IPC=OFF \
        -DwxUSE_STDPATHS=ON \
        -DwxUSE_PROTOCOL=OFF \
        -DwxUSE_PROTOCOL_HTTP=OFF \
        -DwxUSE_PROTOCOL_FTP=OFF \
        -DwxUSE_PROTOCOL_FILE=OFF \
        -DwxUSE_URL=OFF \
        -DwxUSE_FS_INET=OFF \
        -DwxUSE_COMPILER_TLS=OFF \
        -DCMAKE_CXX_STANDARD=20 \
        -DCMAKE_C_FLAGS="$WASM_CFLAGS -DXML_DEV_URANDOM" \
        -DCMAKE_CXX_FLAGS="$WASM_CXXFLAGS" \
        > "$LOGS/wx-configure.log" 2>&1

    # Guard the two detections that go wrong silently and only blow up much
    # later (or, worse, not at all).
    local setup_h="$BLD/wx/lib/wx/include/base-unicode-static-3.2/wx/setup.h"
    grep -q '^#define SIZEOF_VOID_P 4$'  "$setup_h" || die "wx setup.h has the wrong pointer size - is the patch applied?"
    grep -q '^#define wxSIZE_T_IS_ULONG' "$setup_h" || die "wx setup.h mis-detected size_t - did module scanning break the probes?"

    cmake --build   "$BLD/wx" -j"$JOBS" > "$LOGS/wx-build.log"   2>&1
    cmake --install "$BLD/wx"           > "$LOGS/wx-install.log" 2>&1

    ls "$WASM_PREFIX/lib/libwx_baseu-3.2-Emscripten.a" >/dev/null
    ls "$WASM_PREFIX/bin/wx-config" >/dev/null

    # A --disable-gui install ships only the ~170 base headers, and with
    # wxUSE_GUI 0 wx does not even DECLARE wxBitmap / wxFont / wxDC / wxWindow /
    # wxImage / wxColour / wxGridTableBase / wxCommandEvent.  KiCad's headless
    # translation units still name them, so they would fail to COMPILE, not just
    # to link.  Add the full 3.2.11 header set beside the installed one (never
    # overwriting an installed header) so that host/wx_headless/wxgui/wx/setup.h
    # can turn wxUSE_GUI back on for declarations only; nothing GUI is linked.
    rsync -a --ignore-existing "$SRC/wxWidgets-$WX_VERSION/include/wx/" \
                               "$WASM_PREFIX/include/wx-3.2/wx/"
    ls "$WASM_PREFIX/include/wx-3.2/wx/gtk/colour.h" >/dev/null

    mark_done wx
}

# ---------------------------------------------------------------------------
# stage: smoke
# ---------------------------------------------------------------------------
stage_smoke() {
    say "smoke test"
    bash "$HERE/smoke/build-smoke.sh"
}

# ---------------------------------------------------------------------------
# driver
# ---------------------------------------------------------------------------
ALL_STAGES=(fetch abseil protobuf zstd wx smoke)
STAGES=( "${@:-}" )
[ -z "${STAGES[0]:-}" ] && STAGES=( "${ALL_STAGES[@]}" )

preflight

START=$( date +%s )

for stage in "${STAGES[@]}"; do
    case "$stage" in
        fetch|abseil|protobuf|zstd|wx)
            if is_done "$stage"; then
                say "$stage: already built (rm $( stamp_file "$stage" ) to redo)"
            else
                "stage_$stage"
            fi
            ;;
        smoke)
            stage_smoke
            ;;
        *)
            die "unknown stage '$stage' (have: ${ALL_STAGES[*]})"
            ;;
    esac
done

END=$( date +%s )
say "done in $(( (END - START) / 60 ))m $(( (END - START) % 60 ))s"
echo "source tools/wasm/env.sh before configuring KiCad against $WASM_PREFIX"
