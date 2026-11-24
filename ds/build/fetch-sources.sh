#!/bin/sh
# Put the pinned upstream sources (upstream.lock) in DEST: the SuperDrastic tree with BananaDS
# (bananads/) and melonDS in DEST/melonds. Nothing is patched here: build/release-build.sh does that.
#   build/fetch-sources.sh DEST
# Without network, from copies already on this computer:
#   PIXELADO_CORE_SUPERDRASTIC_GIT=/path/to/a/SuperDrastic/clone   (git archive of the pinned commit)
#   PIXELADO_CORE_MELONDS_TREE=/path/to/melonDS/at/the/pinned/commit (copied as it is)
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
DEST=${1:-}
[ -n "$DEST" ] || { echo "usage: build/fetch-sources.sh DEST" >&2; exit 2; }
lock() { sed -n "s/^$1=//p" "$ROOT/upstream.lock" | head -n 1 | tr -d '\r'; }
SD_REPO=$(lock superdrastic_repo) SD_SHA=$(lock superdrastic)
MEL_REPO=$(lock melonds_repo) MEL_SHA=$(lock melonds)
[ -n "$SD_SHA" ] && [ -n "$MEL_SHA" ] || { echo "upstream.lock needs superdrastic and melonds" >&2; exit 1; }

rm -rf "$DEST"
mkdir -p "$DEST/melonds"
if [ -n "${PIXELADO_CORE_SUPERDRASTIC_GIT:-}" ] && git -C "$PIXELADO_CORE_SUPERDRASTIC_GIT" cat-file -e "$SD_SHA^{commit}" 2>/dev/null; then
    git -C "$PIXELADO_CORE_SUPERDRASTIC_GIT" archive "$SD_SHA" | tar -x -C "$DEST"
    echo "SuperDrastic $SD_SHA (local clone)"
else
    tmp=$(mktemp -d)
    git -C "$tmp" init -q
    git -C "$tmp" fetch -q --depth 1 "$SD_REPO" "$SD_SHA"
    git -C "$tmp" archive FETCH_HEAD | tar -x -C "$DEST"
    rm -rf "$tmp"
    echo "SuperDrastic $SD_SHA ($SD_REPO)"
fi
if [ -n "${PIXELADO_CORE_MELONDS_TREE:-}" ] && [ -f "$PIXELADO_CORE_MELONDS_TREE/CMakeLists.txt" ]; then
    (cd "$PIXELADO_CORE_MELONDS_TREE" && tar -cf - --exclude .git .) | tar -x -C "$DEST/melonds"
    echo "melonDS $MEL_SHA (local tree $PIXELADO_CORE_MELONDS_TREE)"
else
    curl -fsSL "$MEL_REPO/archive/$MEL_SHA.tar.gz" | tar -xz -C "$DEST/melonds" --strip-components=1
    echo "melonDS $MEL_SHA ($MEL_REPO)"
fi
