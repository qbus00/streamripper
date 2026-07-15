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
# libvorbis.  libmad ships only a dylib on Homebrew, so it is built statically
# from source here.

set -eu

ROOT="$(cd "$(dirname "$0")" && pwd)"
DEPS="$ROOT/macos-build/deps"          # local prefix for from-source static deps
BUILD="$ROOT/macos-build/build"
CACHE="$ROOT/macos-build/cache"        # cached source tarballs (avoid re-download)
OUT="$ROOT/dist"
BREW="$(brew --prefix)"
mkdir -p "$DEPS" "$CACHE" "$OUT"

##-----------------------------------------------------------------------------
## 1. libmad (static, from source -- Homebrew has no .a)
##-----------------------------------------------------------------------------
if [ ! -f "$DEPS/lib/libmad.a" ]; then
    echo "==> Building libmad (static) from source"
    tmp="$(mktemp -d)"
    # Download to a persistent cache once; SourceForge mirrors are flaky (502s /
    # connection resets), so retry and fall back to other hosts.
    if [ ! -s "$CACHE/libmad-0.15.1b.tar.gz" ]; then
        dl_ok=0
        for url in \
            "https://downloads.sourceforge.net/mad/libmad-0.15.1b.tar.gz" \
            "https://ftp.osuosl.org/pub/blfs/conglomeration/libmad/libmad-0.15.1b.tar.gz" \
            "https://distfiles.macports.org/libmad/libmad-0.15.1b.tar.gz"
        do
            echo "    trying $url"
            if curl -fsSL --connect-timeout 15 --retry 5 --retry-delay 3 \
                    --retry-all-errors "$url" -o "$CACHE/libmad-0.15.1b.tar.gz"; then
                dl_ok=1; break
            fi
        done
        [ "$dl_ok" = 1 ] || { echo "ERROR: could not download libmad"; exit 1; }
    fi
    tar xzf "$CACHE/libmad-0.15.1b.tar.gz" -C "$tmp"
    cd "$tmp/libmad-0.15.1b"
    # 2004-era config scripts predate arm64; refresh them from the cache
    # (populated once; see below), falling back to a network fetch.
    for f in config.guess config.sub; do
        if [ -s "$CACHE/$f" ] && grep -q aarch64 "$CACHE/$f"; then
            cp "$CACHE/$f" "$f"
        else
            curl -fsSL --retry 4 --retry-all-errors \
                "https://git.savannah.gnu.org/cgit/config.git/plain/$f" -o "$f"
            cp "$f" "$CACHE/$f"
        fi
    done
    # libmad's configure appends ancient GCC-only optimization flags that
    # modern clang rejects; strip them all so the build uses plain -O2.
    sed -i '' \
        -e 's/-fforce-mem//g' \
        -e 's/-fthread-jumps//g' \
        -e 's/-fcse-follow-jumps//g' \
        -e 's/-fcse-skip-blocks//g' \
        -e 's/-fregmove//g' \
        -e 's/-fexpensive-optimizations//g' \
        -e 's/-fschedule-insns2//g' \
        configure
    ./configure --enable-static --disable-shared --prefix="$DEPS" \
        --host=aarch64-apple-darwin CFLAGS="-arch arm64 -O2"
    make -j"$(sysctl -n hw.ncpu)"
    make install
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
    -DMAD_LIBRARY="$DEPS/lib/libmad.a" \
    -DMAD_INCLUDE_DIR="$DEPS/include"

echo "==> Building streamripper"
cmake --build "$BUILD" --target streamripper -j"$(sysctl -n hw.ncpu)"

cp "$BUILD/streamripper" "$OUT/streamripper-macos-arm64"
strip "$OUT/streamripper-macos-arm64"

echo
echo "==> Wrote $OUT/streamripper-macos-arm64"
file "$OUT/streamripper-macos-arm64"
echo "--- non-system dynamic dependencies (should be none) ---"
otool -L "$OUT/streamripper-macos-arm64" | grep -viE "/usr/lib/|/System/|:$" || echo "  (none -- self-contained)"
