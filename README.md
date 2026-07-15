# streamripper

Record shoutcast/icecast radio streams to disk, split into individual tracks.

Streamripper connects to a shoutcast- or icecast-compatible stream, reads the
inline metadata to detect where each song begins and ends, and saves the tracks
to your hard drive in the stream's native format (**mp3, aac, nsv, ogg**). It
can also run a local relay server so you can listen while you record.

> **This fork** adds **HTTPS/TLS support** (modern `https://` stream URLs) and
> **self-contained cross-builds** for Linux (arm64/amd64) and Apple Silicon
> macOS. It is based on the original
> [streamripper](http://streamripper.sourceforge.net/) by Jon Clegg and
> Gregory C. Sharp.

---

## Highlights

- 🎧 Rips mp3 / aac / nsv / ogg streams, split into per-track files
- 🔒 **`https://` streams supported** — TLS via OpenSSL, opt-in certificate
  verification (`--ssl-verify`)
- 📻 Built-in relay server so you can listen while ripping
- ✂️ Silence-based splitpoint detection for clean track boundaries
- 📦 Builds as a **self-contained static binary** (no runtime dependencies) or
  a normal dynamically-linked binary

## Quick start

```sh
# Rip a stream (http or https) into a folder named after the station
streamripper https://ice1.somafm.com/groovesalad-128-mp3

# Rip for one hour into a specific directory, no per-stream subfolder
streamripper URL -l 3600 -d /my/music/stream1 -s

# Rip to a single file instead of individual tracks
streamripper URL -a -A

# Rip and also relay the stream on port 8000 (listen in any player)
streamripper URL -r 8000
```

> **Tip:** some CDNs reject the default user-agent. If a stream connects but
> then fails to read a header, add `-u "WinampMPEG/5.0"`.

## HTTPS / TLS

- `https://` URLs are detected automatically (default port **443**) and
  negotiated with OpenSSL.
- **Certificate verification is off by default** — connections are encrypted
  but not verified. Turn it on with **`--ssl-verify`** (use `--no-ssl-verify`
  to force the default).
- https **through an http proxy** (`-p`) is not supported.

## Common options

| Option | Description |
|---|---|
| `-d dir` | Destination directory |
| `-s` | Don't make a subdirectory per stream |
| `-a [file]` | Rip to a single file (with cue sheet) |
| `-A` | Don't write individual tracks |
| `-l seconds` | Stop after this many seconds |
| `-r [[ip:]port]` | Run a relay server (default port 8000) |
| `-R num` | Max relay clients (`0` = unlimited) |
| `-u useragent` | Use a custom User-Agent |
| `--wav` | Decode mp3 tracks and save them as `.wav` files |
| `--no-cue` | Don't write `.cue` sheet files |
| `--http10` | Use HTTP/1.0 (for servers that mishandle HTTP/1.1) |
| `--ssl-verify` | Verify the TLS certificate for https streams |
| `-m seconds` | Timeout before a stalled connection is dropped |

Run `streamripper -h` for the full list, or see the man page
([`streamripper.1`](streamripper.1)) for complete documentation, including
splitpoint tuning (`--xs-*`) and codeset options.

## Downloads / which binary

This fork is designed to produce a single self-contained binary per target:

| Target | Binary | Notes |
|---|---|---|
| x86-64 Linux | `streamripper-linux-amd64` | fully static; runs on any distro/NAS |
| aarch64 Linux | `streamripper-linux-arm64` | fully static; 64-bit ARM only (not armv7) |
| Apple Silicon macOS (M1–M5) | `streamripper-macos-arm64` | no Homebrew needed at runtime |

Build them yourself with the scripts below (they aren't checked into the repo).

## Building

See **[BUILDING.md](BUILDING.md)** for full instructions. In short:

```sh
# Dynamic build (default) — needs dev libs installed on the build machine
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Self-contained static binaries
./build-linux.sh     # -> dist/streamripper-linux-{arm64,amd64}  (needs Docker/Colima)
./build-macos.sh     # -> dist/streamripper-macos-arm64
```

Dependencies: glib-2.0 and libmad are required; OpenSSL (for https, on by
default) and libogg/libvorbis (for ogg) are optional. CMake ≥ 3.5 and
pkg-config are needed to build.

## Relay server

With `-r`, streamripper serves the stream locally while recording. Point your
player (VLC, Winamp, mpv, …) at `http://<host>:8000` to listen. Use `-R` to
allow more than one simultaneous listener.

## Resources

- Original project: <http://streamripper.sourceforge.net/>
- SourceForge: <http://sourceforge.net/projects/streamripper>

## License

Copyright © 2000–2002 Jon Clegg, © 2004–2009 Gregory C. Sharp, and fork
contributors. Free use is granted under the terms of the **GNU General Public
License, version 2** (see [`COPYING`](COPYING)).
