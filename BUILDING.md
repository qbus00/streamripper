# Building streamripper

This fork builds with **CMake** and adds **HTTPS/TLS** support (via OpenSSL).
There are two kinds of build:

- **Dynamic** (default) — links the system's shared libraries. Small binary,
  but the target machine must have the libraries installed. Best when you build
  on the same machine you'll run on.
- **Self-contained / static** — bundles the third-party libraries into the
  binary so it runs with no external dependencies. Best for shipping one file
  to a NAS, router, or a machine without a dev environment.

## Dependencies

| Library | Purpose | Required? |
|---|---|---|
| glib-2.0 | core data structures | yes |
| libmad | MP3 decode (silence-based track splitting) | yes |
| OpenSSL | TLS for `https://` streams | optional (on by default; `-DWITH_SSL=OFF` to disable) |
| faad2 | AAC decode for silence-based splitting of aac streams | optional (on by default; `-DWITH_FAAD=OFF` to disable) |
| libogg + libvorbis | Ogg/Vorbis stream support | optional |

A C compiler, CMake (>= 3.5), and pkg-config are also needed.

## Compilers / toolchains verified

- **Linux**: GCC 13 (Alpine/musl) and GCC 12 (Debian/glibc)
- **macOS**: AppleClang 21 (Xcode command-line tools)

---

## Dynamic build (default)

### Linux (Debian/Ubuntu example)

```sh
sudo apt install cmake pkg-config build-essential \
    libglib2.0-dev libmad0-dev libfaad-dev libogg-dev libvorbis-dev libssl-dev

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
# -> build/streamripper  (dynamically linked)
```

### macOS (Apple Silicon or Intel)

```sh
brew install cmake pkg-config glib mad faad2 libogg libvorbis openssl@3

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$(brew --prefix)" \
    -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
cmake --build build -j"$(sysctl -n hw.ncpu)"
# -> build/streamripper  (links Homebrew dylibs)
```

---

## Self-contained (static) builds

These produce the binaries shipped in `dist/`.

### Linux — fully static, both architectures

Uses Docker (Alpine/musl) so the result is a **truly static** binary with no
shared-library dependencies at all. On Apple silicon, a Linux VM is provided by
Colima; the amd64 build runs under QEMU emulation.

```sh
brew install colima docker docker-buildx      # one time
colima start --arch aarch64                    # start the Linux VM

./build-linux.sh
# -> dist/streamripper-linux-arm64   (aarch64, e.g. ARM routers/NAS)
# -> dist/streamripper-linux-amd64   (x86_64,  e.g. Intel NAS/PC)

colima stop                                    # when done
```

Build a single architecture directly:

```sh
docker buildx build --platform linux/arm64 -f Dockerfile.alpine \
    --target artifact -o type=local,dest=out .
# -> out/streamripper
```

#### Note on the emulated amd64 build (QEMU `cc1` segfaults)

On an arm64 host the amd64 image is compiled under QEMU user-mode emulation,
where GCC's `cc1` **segfaults at random** — a valid build step fails with
`internal compiler error: Segmentation fault` on no particular file, and a
plain re-run usually gets further. It's an emulation bug, not a problem with
the source. The build scripts work around it in three ways, so you normally
don't have to think about it:

- **`build-linux.sh` retries** the emulated build (up to 8 attempts). Docker
  caches the layers that already succeeded, so each retry resumes where the
  last one crashed rather than starting over.
- **`JOBS=1`** is passed for the amd64 target so every from-source dependency
  and the final link compile single-threaded (parallel `cc1` under QEMU is the
  worst case). Native arm64 still builds with all cores.
- **faad2 builds only the `faad` target** (the one static lib we need) instead
  of its default four library variants + CLI — that is ~75% less code fed
  through the emulated compiler, which markedly cuts the crash rate.

If you build the amd64 image with a raw `docker buildx build` (bypassing
`build-linux.sh`) and hit the segfault, just run the same command again — the
cache makes it resume.

### macOS — self-contained arm64

macOS cannot produce a *fully* static binary (there is no static libSystem), so
"self-contained" means all third-party libraries are linked statically while the
always-present macOS system libraries/frameworks stay dynamic. libmad and faad2
have no static package on Homebrew, so the script builds them from source
(cached under `macos-build/cache/`).

```sh
brew install cmake pkg-config glib pcre2 gettext openssl@3 libogg libvorbis faad2

./build-macos.sh
# -> dist/streamripper-macos-arm64   (runs on any Apple Silicon Mac, M1..M5)
```

A binary you build yourself runs without any Gatekeeper prompt. A binary
**downloaded** from the releases page is quarantined by macOS because it is not
code-signed/notarized; clear the flag once with
`xattr -d com.apple.quarantine streamripper-macos-arm64` (or approve it under
*System Settings → Privacy & Security → Open Anyway*).

---

## CMake options

| Option | Default | Meaning |
|---|---|---|
| `STREAMRIPPER_STATIC` | `OFF` | Self-contained build. On Linux: fully static (`-static`, musl). On macOS: static third-party libs, dynamic system libs. |
| `WITH_SSL` | `ON` | Build with OpenSSL for `https://` support. `OFF` disables TLS (https URLs then error out). |
| `WITH_FAAD` | `ON` | Build with faad2 for AAC silence-based track splitting. `OFF` (or faad2 absent) → aac streams split at a fixed interval instead. |

When cross-relevant, also useful:
`-DCMAKE_OSX_ARCHITECTURES=arm64`, `-DOPENSSL_ROOT_DIR=...`,
`-DMAD_LIBRARY=... -DMAD_INCLUDE_DIR=...`.

---

## Which binary runs where

| Target | Binary | Notes |
|---|---|---|
| x86_64 Linux (kernel ≥ ~3.2) | `streamripper-linux-amd64` | fully static; any distro |
| aarch64 Linux (kernel ≥ ~3.2) | `streamripper-linux-arm64` | fully static; 64-bit ARM only (not armv7) |
| Apple Silicon macOS (M1–M5) | `streamripper-macos-arm64` | needs no Homebrew; uses OS libs |

The static Linux binaries are architecture-specific and 64-bit only. There is no
32-bit (armv7 / i386) build here.

## HTTPS usage notes

- `https://` is auto-detected (default port 443); TLS is negotiated with OpenSSL.
- Certificate verification is **off by default**; pass `--ssl-verify` to enable it.
- https through an http proxy (`-p`) works via CONNECT tunneling (with
  `Proxy-Authorization` if the proxy URL includes `user:pass@`).
- Some CDNs drop the connection for the default `User-Agent`; if a stream
  connects then fails to read a header, try `-u "WinampMPEG/5.0"`.
