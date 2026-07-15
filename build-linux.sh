#!/bin/sh
# Build fully-static streamripper binaries for Linux, one per architecture.
#
# Produces:
#   dist/streamripper-linux-arm64
#   dist/streamripper-linux-amd64
#
# Requires Docker with the buildx plugin. On Apple silicon this is typically
# Colima:
#   colima start --arch aarch64
# The amd64 build runs under qemu emulation (slower) via Docker's binfmt.

set -eu

DOCKERFILE=Dockerfile.alpine
OUTDIR=dist
mkdir -p "$OUTDIR"

# Use a dedicated buildx builder (the docker driver is single-platform).
if ! docker buildx inspect sr-builder >/dev/null 2>&1; then
    docker buildx create --name sr-builder --driver docker-container --use >/dev/null
fi
docker buildx use sr-builder

build_one() {
    platform="$1"
    suffix="$2"
    jobs="$3"          # build parallelism; empty = all cores
    echo "==> Building for ${platform} (${suffix})"
    # The 'artifact' stage is FROM scratch and contains only the binary;
    # -o exports it to the host as $OUTDIR/streamripper.
    docker buildx build \
        --platform "$platform" \
        -f "$DOCKERFILE" \
        --target artifact \
        --build-arg "JOBS=${jobs}" \
        -o "type=local,dest=${OUTDIR}/_${suffix}" \
        .
    mv "${OUTDIR}/_${suffix}/streamripper" "${OUTDIR}/streamripper-linux-${suffix}"
    rmdir "${OUTDIR}/_${suffix}" 2>/dev/null || rm -rf "${OUTDIR}/_${suffix}"
    echo "==> Wrote ${OUTDIR}/streamripper-linux-${suffix}"
}

# arm64 builds natively here (fast, all cores); amd64 runs under QEMU
# emulation where parallel cc1 can segfault, so build it single-threaded.
build_one linux/arm64 arm64 ""
build_one linux/amd64 amd64 1

echo
echo "Done:"
ls -la "$OUTDIR"
file "$OUTDIR"/streamripper-linux-* 2>/dev/null || true
