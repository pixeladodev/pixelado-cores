#!/bin/sh
# Put the pinned gpSP (upstream.lock) in DEST. Nothing is patched here: build/build-core.sh does that.
#   build/fetch-sources.sh DEST
# Without network, from a clone already on this computer:
#   PIXELADO_CORE_GPSP_GIT=/path/to/a/gpsp/clone   (git archive of the pinned commit)
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)          # cores/gba/gpsp/
DEST=${1:-}
[ -n "$DEST" ] || { echo "usage: build/fetch-sources.sh DEST" >&2; exit 2; }
lock() { sed -n "s/^$1=//p" "$ROOT/upstream.lock" | head -n 1 | tr -d '\r'; }
REPO=$(lock gpsp_repo) SHA=$(lock gpsp)
[ -n "$SHA" ] || { echo "upstream.lock needs gpsp" >&2; exit 1; }
rm -rf "$DEST"
mkdir -p "$DEST"
if [ -n "${PIXELADO_CORE_GPSP_GIT:-}" ] && git -C "$PIXELADO_CORE_GPSP_GIT" cat-file -e "$SHA^{commit}" 2>/dev/null; then
    git -C "$PIXELADO_CORE_GPSP_GIT" archive "$SHA" | tar -x -C "$DEST"
    echo "gpSP $SHA (local clone)"
else
    curl -fsSL "$REPO/archive/$SHA.tar.gz" | tar -xz -C "$DEST" --strip-components=1
    echo "gpSP $SHA ($REPO)"
fi
