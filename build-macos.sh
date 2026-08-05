#!/bin/sh
# Build a self-contained arm64 streamripper for macOS on Apple Silicon.
#
# Produces:  dist/streamripper-macos-arm64
#
# "Self-contained" on macOS = every third-party (Homebrew) library is linked
# statically; only the always-present macOS system libraries/frameworks remain
# dynamic (a fully static binary is impossible on macOS -- there is no static
# libSystem).  The result runs on any Apple Silicon Mac (M1..M5) with no
# Homebrew install required.
#
# Requires (Homebrew): cmake pkg-config glib pcre2 gettext openssl@3 libogg
# libvorbis.  MP3 decoding uses the vendored header-only minimp3 (no lib).

set -eu

ROOT="$(cd "$(dirname "$0")" && pwd)"
DEPS="$ROOT/macos-build/deps"          # local prefix for from-source static deps
BUILD="$ROOT/macos-build/build"
CACHE="$ROOT/macos-build/cache"        # cached source tarballs (avoid re-download)
OUT="$ROOT/dist"
BREW="$(brew --prefix)"
mkdir -p "$DEPS" "$CACHE" "$OUT"

##-----------------------------------------------------------------------------
## 1b. faad2 (static, from source -- Homebrew ships only a dylib)
##     Used for AAC silence detection (track splitting on aac streams).
##-----------------------------------------------------------------------------
if [ ! -f "$DEPS/lib/libfaad.a" ]; then
    echo "==> Building faad2 (static) from source"
    tmp="$(mktemp -d)"
    if [ ! -s "$CACHE/faad2-2.11.2.tar.gz" ]; then
        curl -fsSL --connect-timeout 15 --retry 5 --retry-delay 3 \
            --retry-all-errors \
            "https://github.com/knik0/faad2/archive/refs/tags/2.11.2.tar.gz" \
            -o "$CACHE/faad2-2.11.2.tar.gz" \
            || { echo "ERROR: could not download faad2"; exit 1; }
    fi
    tar xzf "$CACHE/faad2-2.11.2.tar.gz" -C "$tmp"
    cd "$tmp/faad2-2.11.2"
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_INSTALL_PREFIX="$DEPS"
    cmake --build build -j"$(sysctl -n hw.ncpu)"
    cmake --install build
    cd "$ROOT"
    rm -rf "$tmp"
fi

##-----------------------------------------------------------------------------
## 2. streamripper (self-contained)
##-----------------------------------------------------------------------------
echo "==> Configuring streamripper"
rm -rf "$BUILD"
cmake -S "$ROOT" -B "$BUILD" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DSTREAMRIPPER_STATIC=ON \
    -DWITH_SSL=ON \
    -DOPENSSL_ROOT_DIR="$BREW/opt/openssl@3" \
    -DFAAD_LIBRARIES="$DEPS/lib/libfaad.a" \
    -DFAAD_INCLUDE_DIR="$DEPS/include"

echo "==> Building streamripper"
cmake --build "$BUILD" --target streamripper -j"$(sysctl -n hw.ncpu)"

cp "$BUILD/streamripper" "$OUT/streamripper-macos-arm64"
strip "$OUT/streamripper-macos-arm64"

echo
echo "==> Wrote $OUT/streamripper-macos-arm64"
file "$OUT/streamripper-macos-arm64"
echo "--- non-system dynamic dependencies (should be none) ---"
otool -L "$OUT/streamripper-macos-arm64" | grep -viE "/usr/lib/|/System/|:$" || echo "  (none -- self-contained)"
