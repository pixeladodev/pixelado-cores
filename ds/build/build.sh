#!/bin/bash
# Build the DS core (pixelado-core-ds) from a computer with Docker (macOS or Linux), then pack it.
#   build/build.sh                 # dist/pixelado-core-ds, dist/pixelado-core-ds-<version>.zip
#   PIXELADO_CORE_DRAFT=1 build/build.sh   # keep a binary whose sha is not ./manifest's (a change being tried)
# Optional, to build without downloading what is already on this computer:
#   PIXELADO_CORE_SUPERDRASTIC_GIT  a SuperDrastic clone that has the pinned commit
#   PIXELADO_CORE_MELONDS_TREE      melonDS at the pinned commit
#   PIXELADO_CORE_TOOLCHAIN_CACHE   a folder kept between builds for /opt (the toolchain, libdrm, alsa, libenet);
#                             default .cache/toolchain in this repo
#   PIXELADO_CORE_IMAGE             the Docker image (default debian:trixie-slim; one with cmake, ninja and
#                             patch already in it skips apt)
#   PIXELADO_CORE_JOBS / PIXELADO_CORE_CPUS
set -euo pipefail
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
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
    -e PIXELADO_CORE_JOBS="$JOBS" -e PIXELADO_CORE_DRAFT="${PIXELADO_CORE_DRAFT:-}" -e PIXELADO_CORE_UNSTRIPPED="${PIXELADO_CORE_UNSTRIPPED:-}" -e PIXELADO_CORE_GENGIS="${PIXELADO_CORE_GENGIS:-1}" \
    -v "$ROOT:/core" -v "$CACHE:/opt" -v "$SRC:/tmp/vmac/src" -w /core \
    "$IMAGE" bash build/release-build.sh
# On Linux the container writes dist/ as root: hand it back to this user so package.sh can write the zip
# (Docker Desktop on a Mac maps ownership already)
if [ -d "$ROOT/dist" ] && [ ! -w "$ROOT/dist" ]; then
    docker run --rm -v "$ROOT:/core" "$IMAGE" chown -R "$(id -u):$(id -g)" /core/dist
fi
"$ROOT/build/package.sh"
