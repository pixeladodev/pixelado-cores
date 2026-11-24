#!/bin/sh
# The GBA core's tests, on a computer with Docker, in an arm64 Linux container (native on an arm64 Mac): gpSP built
# with its arm64 recompiler as on the handhelds, the frontend with all its parts, then, headless (no display, no
# sound): the link test ROM on two cores linked over TCP (gpSP's wireless adapter, both directions), the frontend's
# own tests (common/, the loopback and the Party/room test with the fake core), and a speed check.
#   gba/tests/run.sh
#   PIXELADO_CORE_GPSP_GIT=/path/to/gpsp/clone gba/tests/run.sh   (no download of gpSP)
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
IMAGE=pixelado-cores-gba-test:1
docker image inspect "$IMAGE" >/dev/null 2>&1 ||
    docker build --platform linux/arm64 -t "$IMAGE" -f "$ROOT/gba/tests/Dockerfile" "$ROOT/gba/tests"
set -- -v "$ROOT:/cores:ro"
if [ -n "${PIXELADO_CORE_GPSP_GIT:-}" ]; then set -- "$@" -v "$PIXELADO_CORE_GPSP_GIT:/gpsp-git:ro" -e PIXELADO_CORE_GPSP_GIT=/gpsp-git; fi
docker run --rm --platform linux/arm64 "$@" "$IMAGE" sh /cores/gba/tests/in-container.sh
