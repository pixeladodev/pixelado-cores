#!/bin/bash
# Build the GBA core (pixelado-core-gpsp) from a computer with Docker (macOS or Linux), then pack it.
#   cores/gba/gpsp/build/build.sh                       # gba/dist/pixelado-core-gpsp, gba/dist/pixelado-core-gpsp-<version>.zip
#   PIXELADO_CORE_DRAFT=1 cores/gba/gpsp/build/build.sh # keep a binary whose sha is not cores/gba/gpsp/manifest's (a change being tried)
# Optional, to build without downloading what is already on this computer:
#   PIXELADO_CORE_GPSP_GIT          a gpSP clone that has the pinned commit
#   PIXELADO_CORE_TOOLCHAIN_CACHE   a folder kept between builds for /opt (the toolchain, libdrm, alsa); the DS
#                                   core's works too; default cores/gba/gpsp/.cache/toolchain
#   PIXELADO_CORE_IMAGE             the Docker image (default debian:trixie-slim)
#   PIXELADO_CORE_JOBS / PIXELADO_CORE_CPUS
set -euo pipefail
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)          # cores/gba/gpsp/
REPO=$(CDPATH= cd -- "$ROOT/../../.." && pwd)                # the repository
command -v docker >/dev/null 2>&1 || { echo "build: Docker is needed" >&2; exit 1; }
CACHE=${PIXELADO_CORE_TOOLCHAIN_CACHE:-$ROOT/.cache/toolchain}
SRC=$ROOT/.cache/src
IMAGE=${PIXELADO_CORE_IMAGE:-debian:trixie-slim}
NCPU=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
JOBS=${PIXELADO_CORE_JOBS:-$(( NCPU * 8 / 10 > 0 ? NCPU * 8 / 10 : 1 ))}
CPUS=${PIXELADO_CORE_CPUS:-$JOBS}
mkdir -p "$CACHE"

sh "$ROOT/build/fetch-sources.sh" "$SRC"
docker run --rm --platform linux/amd64 --cpus="$CPUS" \
    -e PIXELADO_CORE_JOBS="$JOBS" -e PIXELADO_CORE_DRAFT="${PIXELADO_CORE_DRAFT:-}" \
    -v "$REPO:/cores" -v "$CACHE:/opt" -v "$SRC:/tmp/vmac-gba/src" -w /cores \
    "$IMAGE" bash cores/gba/gpsp/build/release-build.sh
# On Linux the container writes dist/ as root: hand it back to this user so package.sh can write the zip
if [ -d "$ROOT/dist" ] && [ ! -w "$ROOT/dist" ]; then
    docker run --rm -v "$ROOT:/core" "$IMAGE" chown -R "$(id -u):$(id -g)" /core/dist
fi
"$ROOT/build/package.sh"
