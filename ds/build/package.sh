#!/bin/sh
# Pack dist/pixelado-core-ds for the apps that bundle it (Pixelado Party): dist/pixelado-core-ds-<version>.zip, with
# everything under pixelado-core-ds/:
#   pixelado-core-ds                   the engine (see docs/RUNTIME.md)
#   run-emu, lan.sh, status.sh, common.sh   runtime/bananaos/
#   manifest, LICENSE, NOTICE, SOURCE  the version, the licence, and where this version's source is
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BIN=${1:-$ROOT/dist/pixelado-core-ds}
[ -s "$BIN" ] || { echo "package: no $BIN (build/build.sh first)" >&2; exit 1; }
ver=$(sed -n 's/^version=//p' "$ROOT/manifest" | head -n 1 | tr -d '\r')
want=$(sed -n 's/^binary_sha=//p' "$ROOT/manifest" | head -n 1 | tr -d '\r')
sha() { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}'; else shasum -a 256 "$1" | awk '{print $1}'; fi; }
got=$(sha "$BIN")
if [ "$got" != "$want" ] && [ "${PIXELADO_CORE_DRAFT:-}" != 1 ]; then
    echo "package: binary sha is $got, manifest wants $want" >&2; exit 1
fi
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
APP=$STAGE/pixelado-core-ds
mkdir -p "$APP"
cp "$BIN" "$APP/pixelado-core-ds"
cp "$ROOT/runtime/bananaos/run-emu" "$ROOT/runtime/bananaos/lan.sh" "$ROOT/runtime/bananaos/status.sh" \
    "$ROOT/runtime/bananaos/common.sh" "$ROOT/manifest" "$ROOT/../LICENSE" "$ROOT/../NOTICE" "$APP/"
printf 'pixelado-core-ds %s\nsource: https://github.com/pixeladodev/pixelado-cores/tree/ds-v%s/ds\n' "$ver" "$ver" > "$APP/SOURCE"
chmod 755 "$APP/pixelado-core-ds" "$APP/run-emu" "$APP/lan.sh" "$APP/status.sh"
ZIP=$ROOT/dist/pixelado-core-ds-$ver.zip
python3 - "$STAGE" "$ZIP" << 'PY'
import os, sys, zipfile
stage, dest = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as z:
    for dirpath, _, files in sorted(os.walk(stage)):
        for name in sorted(files):
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, stage).replace(os.sep, "/")
            info = zipfile.ZipInfo(rel, date_time=(2026, 1, 1, 0, 0, 0))  # no local time in the zip
            info.external_attr = (os.stat(full).st_mode & 0xFFFF) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            with open(full, "rb") as fh:
                z.writestr(info, fh.read())
PY
(cd "$ROOT/dist" && { sha "pixelado-core-ds-$ver.zip" | sed "s/\$/  pixelado-core-ds-$ver.zip/"; echo "$got  pixelado-core-ds"; } > SHA256SUMS)
cat "$ROOT/dist/SHA256SUMS"
