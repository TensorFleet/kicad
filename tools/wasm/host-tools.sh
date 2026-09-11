#!/usr/bin/env bash
#
# The two NATIVE tools the wasm build needs on the host, for machines that cannot get
# them from a package manager (i.e. Linux CI -- on macOS `brew install protobuf` already
# gives protoc 36.1 and any KiCad build tree gives lemon):
#
#   tools/wasm/host-tools.sh            # both
#   tools/wasm/host-tools.sh lemon      # just lemon
#   tools/wasm/host-tools.sh protoc     # just protoc
#
# Output (add build/host-tools/bin to PATH, or point the two variables at it):
#
#   build/host-tools/bin/lemon          LEMON_EXE
#   build/host-tools/bin/protoc         Protobuf_PROTOC_EXECUTABLE
#
# Why not the distro packages:
#
#   lemon   -- thirdparty/lemon/lemon.c is a single self-contained C file (sqlite's parser
#              generator).  configure.sh's default path is build/headless/thirdparty/lemon/lemon,
#              i.e. the NATIVE KiCad configure, but that pulls in the whole native dependency set
#              (wxGTK, OCCT, ngspice, ...) for one 6k-line C program.  cc it directly instead and
#              pass LEMON_EXE.  The wasm tree still has its own `lemon` target -- CMake's
#              generate_lemon_grammar() has DEPENDS lemon -- but LEMON_EXE is a cache variable, so
#              the value passed on the command line is the one actually invoked.
#
#   protoc  -- the host protoc must match the wasm libprotobuf EXACTLY (generated .pb.cc carries a
#              runtime version check that aborts at static-init time otherwise), and no apt
#              release ships protobuf 36.1.  So build it: abseil at protobuf's own pin, then
#              protobuf with -Dprotobuf_ABSL_PROVIDER=package.  ~5-8 min, cache the prefix.
#
# Versions are read out of build-deps.sh so there is one place to change them.
set -euo pipefail

HERE="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
REPO_ROOT="${WASM_REPO_ROOT:-$( cd "$HERE/../.." && pwd )}"

TOOLS_ROOT="${HOST_TOOLS_ROOT:-$REPO_ROOT/build/host-tools}"
PREFIX="$TOOLS_ROOT/prefix"
BIN="$TOOLS_ROOT/bin"
SRC="$TOOLS_ROOT/src"
BLD="$TOOLS_ROOT/build"
JOBS="${JOBS:-$( getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4 )}"

version_from_deps() { sed -n "s/^$1=\([^ ]*\).*/\1/p" "$HERE/build-deps.sh" | head -1; }
PROTOBUF_VERSION="$( version_from_deps PROTOBUF_VERSION )"
ABSEIL_VERSION="$( version_from_deps ABSEIL_VERSION )"

say() { printf '\n\033[1m== %s\033[0m\n' "$*"; }
die() { printf '\033[31merror: %s\033[0m\n' "$*" >&2; exit 1; }

mkdir -p "$BIN" "$SRC" "$BLD" "$PREFIX"

fetch() {
    local url=$1 file=$2
    # build-deps.sh downloads the same tarballs; reuse them when they are already there.
    local shared="$REPO_ROOT/build/wasm-deps/src/$file"
    [ -f "$SRC/$file" ] && { echo "have $file"; return; }
    [ -f "$shared" ] && { cp "$shared" "$SRC/$file"; echo "have $file (from build/wasm-deps/src)"; return; }
    echo "fetching $file"
    curl -fsSL -o "$SRC/$file.part" "$url"
    mv "$SRC/$file.part" "$SRC/$file"
}

# ---------------------------------------------------------------------------
stage_lemon() {
    say "lemon (native)"
    if [ -x "$BIN/lemon" ] && [ "$BIN/lemon" -nt "$REPO_ROOT/thirdparty/lemon/lemon.c" ]; then
        echo "have $BIN/lemon"; return
    fi
    "${CC:-cc}" -O2 -w -o "$BIN/lemon" "$REPO_ROOT/thirdparty/lemon/lemon.c"
    echo "$BIN/lemon"
}

# ---------------------------------------------------------------------------
stage_protoc() {
    say "protoc $PROTOBUF_VERSION (native)"

    if [ -x "$BIN/protoc" ] && [ "$( "$BIN/protoc" --version | awk '{print $2}' )" = "$PROTOBUF_VERSION" ]; then
        echo "have $BIN/protoc ($PROTOBUF_VERSION)"; return
    fi

    # An already-installed matching protoc is good enough (macOS: brew).
    if command -v protoc >/dev/null 2>&1 &&
       [ "$( protoc --version | awk '{print $2}' )" = "$PROTOBUF_VERSION" ]; then
        ln -sf "$( command -v protoc )" "$BIN/protoc"
        echo "have system protoc $PROTOBUF_VERSION -> $BIN/protoc"; return
    fi

    command -v cmake >/dev/null || die "cmake not found"

    fetch "https://github.com/abseil/abseil-cpp/releases/download/${ABSEIL_VERSION}/abseil-cpp-${ABSEIL_VERSION}.tar.gz" \
          "abseil-cpp-${ABSEIL_VERSION}.tar.gz"
    fetch "https://github.com/protocolbuffers/protobuf/releases/download/v${PROTOBUF_VERSION}/protobuf-${PROTOBUF_VERSION}.tar.gz" \
          "protobuf-${PROTOBUF_VERSION}.tar.gz"

    [ -d "$SRC/abseil-cpp-${ABSEIL_VERSION}" ] || tar xzf "$SRC/abseil-cpp-${ABSEIL_VERSION}.tar.gz" -C "$SRC"
    [ -d "$SRC/protobuf-${PROTOBUF_VERSION}" ] || tar xzf "$SRC/protobuf-${PROTOBUF_VERSION}.tar.gz" -C "$SRC"

    if [ ! -f "$PREFIX/lib/cmake/absl/abslConfig.cmake" ] &&
       [ ! -f "$PREFIX/lib64/cmake/absl/abslConfig.cmake" ]; then
        cmake -S "$SRC/abseil-cpp-${ABSEIL_VERSION}" -B "$BLD/absl" \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_INSTALL_PREFIX="$PREFIX" \
            -DCMAKE_CXX_STANDARD=17 \
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
            -DABSL_PROPAGATE_CXX_STD=ON \
            -DABSL_ENABLE_INSTALL=ON \
            -DBUILD_TESTING=OFF \
            -DBUILD_SHARED_LIBS=OFF
        cmake --build "$BLD/absl" -j "$JOBS"
        cmake --install "$BLD/absl"
    fi

    # Only the compiler is wanted: the native runtime is never linked into anything here.
    cmake -S "$SRC/protobuf-${PROTOBUF_VERSION}" -B "$BLD/protobuf" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DCMAKE_PREFIX_PATH="$PREFIX" \
        -DCMAKE_CXX_STANDARD=17 \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -Dprotobuf_ABSL_PROVIDER=package \
        -Dprotobuf_BUILD_TESTS=OFF \
        -Dprotobuf_BUILD_EXAMPLES=OFF \
        -Dprotobuf_BUILD_SHARED_LIBS=OFF \
        -Dprotobuf_BUILD_PROTOC_BINARIES=ON
    cmake --build "$BLD/protobuf" -j "$JOBS" --target protoc

    # protobuf's CMake build names the binary protoc-<version>.0 and leaves `protoc` as a
    # symlink to it (Linux); macOS produces a plain `protoc`.  Accept either, follow the link.
    local built
    built="$( find "$BLD/protobuf" -maxdepth 2 \( -type f -o -type l \) \
                  \( -name 'protoc' -o -name 'protoc-[0-9]*' \) -perm -u+x \
              | grep -vE '\.(so|dylib|a)(\.|$)' | sort | head -1 )"
    [ -n "$built" ] || die "protoc was not produced under $BLD/protobuf"
    cp -L "$built" "$BIN/protoc"

    # protoc resolves the well-known types (google/protobuf/any.proto, ...) from
    # <dir of the binary>/../include; a bare binary copied out of the build tree has none.
    mkdir -p "$TOOLS_ROOT/include/google/protobuf"
    cp "$SRC/protobuf-${PROTOBUF_VERSION}/src/google/protobuf/"*.proto "$TOOLS_ROOT/include/google/protobuf/"

    local got
    got="$( "$BIN/protoc" --version | awk '{print $2}' )"
    [ "$got" = "$PROTOBUF_VERSION" ] || die "built protoc reports $got, expected $PROTOBUF_VERSION"
    echo "$BIN/protoc ($got)"
}

# ---------------------------------------------------------------------------
if [ $# -eq 0 ]; then
    STAGES=( lemon protoc )
else
    STAGES=( "$@" )
fi

for s in "${STAGES[@]}"; do
    case "$s" in
        lemon)  stage_lemon  ;;
        protoc) stage_protoc ;;
        *) die "unknown stage: $s (lemon protoc)" ;;
    esac
done

say "host tools in $BIN"
ls -l "$BIN"
