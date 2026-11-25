#!/bin/bash
# Make the DS core's sources editable: the pinned upstream (upstream.lock) with every patch applied as one git
# commit, in two repositories:
#
#   <tree>/bananads   the SuperDrastic tree (BananaDS in bananads/, without melonDS), then one commit per
#                     patches/bananads/*.patch
#   <tree>/melonds    melonDS, then all of BananaDS's own melonDS patches, then ours (patches/melonds/,
#                     numbered from 0001), one commit each, in the order the build applies them
#
# Edit and commit there, then dev/export.sh writes patches/ back from those commits. The build itself
# always starts from patches/ (build/build.sh), so what is shipped is what the patches say.
#
#   dev/tree.sh            # make the tree (refuses to replace one with work not exported yet)
#   dev/tree.sh -f         # replace it anyway
#
#   PIXELADO_CORE_DEV_TREE      where (default .cache/dev in this repo)
#   PIXELADO_CORE_SUPERDRASTIC_GIT, PIXELADO_CORE_MELONDS_TREE   local copies of the pinned sources (build/fetch-sources.sh)
#
# Commit subjects tell the export what each commit is:
#   "upstream: ..."     the pinned sources (the root commit)
#   "bananads: <file>"  one of BananaDS's own melonDS patches (left out of the export)
#   "pixelado: <file>"  one of our patches; its message body is the text before the patch's first diff
# Any other commit is a new patch: its subject names it and gets the next number after ours. Keep our
# commits after all the "bananads:" ones, as the build applies them.
set -euo pipefail
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)          # cores/nds/bananads/
TREE=${PIXELADO_CORE_DEV_TREE:-$ROOT/.cache/dev}
FORCE=0; [ "${1:-}" = -f ] && FORCE=1
export LC_COLLATE=C

g() { git -C "$1" -c core.hooksPath=/dev/null -c user.name="Pixelado Dev" \
      -c user.email=350114043+pixeladodev@users.noreply.github.com "${@:2}"; }

if [ -d "$TREE" ] && [ "$FORCE" = 0 ]; then
    if ! PIXELADO_CORE_DEV_TREE="$TREE" "$ROOT/dev/export.sh" --check >/dev/null 2>&1; then
        echo "dev/tree.sh: $TREE has changes not in patches/ (dev/export.sh first, or -f to drop them)" >&2
        exit 1
    fi
fi

SRC=$(mktemp -d); trap 'rm -rf "$SRC"' EXIT
sh "$ROOT/build/fetch-sources.sh" "$SRC/sd"
rm -rf "$TREE"; mkdir -p "$TREE"
mv "$SRC/sd/melonds" "$TREE/melonds"
mv "$SRC/sd" "$TREE/bananads"
lock() { sed -n "s/^$1=//p" "$ROOT/upstream.lock" | head -n 1 | tr -d '\r'; }

# one commit for a patch: apply it as the build does (patch -p1, no fuzz), message verbatim
commit_patch() {   # repo, patch file, subject
    local repo=$1 file=$2 subject=$3 msg
    patch -F0 -g0 -p1 --no-backup-if-mismatch -d "$repo" -t -N -s < "$file" \
        || { echo "dev/tree.sh: $(basename "$file") does not apply" >&2; exit 1; }
    msg=$(mktemp)
    { printf '%s\n\n' "$subject"; awk '/^diff --git /{exit} {print}' "$file"; } > "$msg"
    g "$repo" add -A
    g "$repo" commit -q --allow-empty --cleanup=verbatim -F "$msg"
    rm -f "$msg"
}

init_repo() {   # repo, subject
    g "$1" init -q
    g "$1" config core.hooksPath /dev/null   # the export's patches go through this repo's own hooks
    g "$1" add -A
    g "$1" commit -q -m "$2"
}

# --- BananaDS (the SuperDrastic tree) ------------------------------------------
B=$TREE/bananads
init_repo "$B" "upstream: SuperDrastic $(lock superdrastic)"
for f in "$ROOT"/patches/bananads/*.patch; do
    [ -e "$f" ] || continue
    commit_patch "$B" "$f" "pixelado: $(basename "$f")"
done

# --- melonDS -------------------------------------------------------------------
M=$TREE/melonds
init_repo "$M" "upstream: melonDS $(lock melonds)"
for f in "$B"/bananads/patches/melonds/*.patch; do
    commit_patch "$M" "$f" "bananads: $(basename "$f")"
done
for f in "$ROOT"/patches/melonds/*.patch; do
    [ -e "$f" ] || continue
    commit_patch "$M" "$f" "pixelado: $(basename "$f")"
done

cat <<MSG
pixelado-core-bananads sources ready in $TREE:
  bananads/   BananaDS (bananads/src ...), $(g "$B" rev-list --count HEAD) commits
  melonds/    melonDS with BananaDS's patches and ours, $(g "$M" rev-list --count HEAD) commits
Edit, commit (a new commit = a new patch; to change a patch, amend or fix up its commit), then
dev/export.sh to write patches/.
MSG
