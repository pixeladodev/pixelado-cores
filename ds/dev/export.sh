#!/bin/bash
# Write patches/ from the editable sources dev/tree.sh made: one patch per commit of ours, in order.
#
#   dev/export.sh           # rewrite patches/bananads and patches/melonds, then check them (below)
#   dev/export.sh --check   # only say whether patches/ already matches the tree (exit 1 if not)
#
# A "pixelado: <file>" commit keeps its file name and its description (the commit's message body). Any
# other commit after the root that isn't "bananads: ..." becomes a new patch: the next number after ours
# and a name made from its subject. Uncommitted changes stop it: commit them first.
#
# The check: the patches are applied again, as the build does (patch -p1, no fuzz, name order; in
# melonDS all of BananaDS's patches first, then ours), to a clean copy of the pinned sources, and the
# result must be the tree's HEAD byte for byte. A commit of ours placed among BananaDS's fails here.
set -euo pipefail
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TREE=${PIXELADO_CORE_DEV_TREE:-$ROOT/.cache/dev}
CHECK=0; [ "${1:-}" = --check ] && CHECK=1
export LC_COLLATE=C
B=$TREE/bananads M=$TREE/melonds
[ -d "$B/.git" ] && [ -d "$M/.git" ] || { echo "dev/export.sh: no tree in $TREE (dev/tree.sh)" >&2; exit 1; }
g() { git -C "$1" -c core.abbrev=7 "${@:2}"; }
for r in "$B" "$M"; do
    if [ -n "$(g "$r" status --porcelain)" ]; then
        echo "dev/export.sh: uncommitted changes in $r" >&2; exit 1
    fi
done

OUT=$(mktemp -d); trap 'rm -rf "$OUT"' EXIT
mkdir -p "$OUT/bananads" "$OUT/melonds"

slug() { printf '%s' "$1" | sed -e 's/[^A-Za-z0-9]\{1,\}/-/g' -e 's/^-//' -e 's/-$//' | cut -c1-64 | sed 's/-$//'; }
# the commit's message without its subject line, verbatim (the description before the first diff)
desc() { g "$1" cat-file commit "$2" | awk 'BEGIN{s=0} s==2{print; next} s==1{s=2; next} /^$/ && s==0{s=1; next}' | tail -n +2; }

export_repo() {   # repo, out dir, diff path filter...
    local repo=$1 out=$2; shift 2
    local root c subj name num n
    root=$(g "$repo" rev-list --max-parents=0 HEAD)
    # the next number: after our highest
    local ours_max=0
    for c in $(g "$repo" rev-list --reverse "$root"..HEAD); do
        subj=$(g "$repo" log -1 --format=%s "$c")
        case $subj in pixelado:\ *) n=${subj#pixelado: }; n=$((10#${n%%-*})); [ $n -gt $ours_max ] && ours_max=$n ;; esac
    done
    num=$ours_max
    for c in $(g "$repo" rev-list --reverse "$root"..HEAD); do
        subj=$(g "$repo" log -1 --format=%s "$c")
        case $subj in
            bananads:\ *) continue ;;
            pixelado:\ *)
                name=${subj#pixelado: }
                desc "$repo" "$c" > "$out/$name" ;;
            *)
                num=$((num + 1))
                name=$(printf '%04d' $num)-$(slug "$subj").patch
                { echo "From: Pixelado Dev <350114043+pixeladodev@users.noreply.github.com>"
                  echo "Subject: [PATCH] $subj"
                  b=$(g "$repo" log -1 --format=%b "$c")
                  [ -n "$b" ] && { echo; printf '%s\n' "$b"; }
                  echo "---"; } > "$out/$name" ;;
        esac
        g "$repo" diff --binary "$c^" "$c" -- "$@" >> "$out/$name"
    done
}

export_repo "$B" "$OUT/bananads" . ':!bananads/patches/melonds'
export_repo "$M" "$OUT/melonds" .

same() { diff -rq "$OUT/bananads" "$ROOT/patches/bananads" >/dev/null 2>&1 && diff -rq "$OUT/melonds" "$ROOT/patches/melonds" >/dev/null 2>&1; }
if [ "$CHECK" = 1 ]; then
    same && { echo "patches/ matches $TREE"; exit 0; }
    diff -rq "$ROOT/patches" "$OUT" | sed "s|$OUT|(tree)|; s|$ROOT/||" || true
    exit 1
fi

# apply them again on clean sources and compare with the tree
V=$(mktemp -d); trap 'rm -rf "$OUT" "$V"' EXIT
apply() { patch -F0 -g0 -p1 --no-backup-if-mismatch -d "$1" -t -N -s < "$2" \
          || { echo "dev/export.sh: $(basename "$2") does not apply to clean sources" >&2; exit 1; }; }
mkdir -p "$V/b" "$V/m" "$V/hb" "$V/hm"
g "$B" archive "$(g "$B" rev-list --max-parents=0 HEAD)" | tar -x -C "$V/b"
g "$M" archive "$(g "$M" rev-list --max-parents=0 HEAD)" | tar -x -C "$V/m"
for f in "$OUT"/bananads/*.patch; do [ -e "$f" ] && apply "$V/b" "$f"; done
for f in "$B"/bananads/patches/melonds/*.patch "$OUT"/melonds/*.patch; do [ -e "$f" ] && apply "$V/m" "$f"; done
g "$B" archive HEAD | tar -x -C "$V/hb"
g "$M" archive HEAD | tar -x -C "$V/hm"
diff -r "$V/b" "$V/hb" >/dev/null && diff -r "$V/m" "$V/hm" >/dev/null \
    || { echo "dev/export.sh: the patches don't rebuild the tree (order?). patches/ left as it was" >&2
         diff -rq "$V/b" "$V/hb" | head; diff -rq "$V/m" "$V/hm" | head; exit 1; }

if same; then echo "patches/ already matches $TREE"; exit 0; fi
diff -rq "$ROOT/patches" "$OUT" | sed "s|$OUT|(tree)|; s|$ROOT/||" || true
rm -f "$ROOT"/patches/bananads/*.patch "$ROOT"/patches/melonds/*.patch
cp "$OUT"/bananads/*.patch "$ROOT/patches/bananads/" 2>/dev/null || true
cp "$OUT"/melonds/*.patch "$ROOT/patches/melonds/"
echo "patches/ written from $TREE (checked: they rebuild it). Build with PIXELADO_CORE_DRAFT=1."
