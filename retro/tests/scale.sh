#!/bin/sh
# The game's size on the panel (the null video, a 640x480 panel and a 1024x768 one): fit keeps the 3:2 aspect ratio
# and fills the panel's width, integer uses whole numbers; the setting kept in the core's own file and the command
# line over it.
#   tests/scale.sh build/pixelado-core-fake
set -eu
BIN=${1:?the frontend built with the fake core}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
export PIXELADO_CORE_CONFIG="$T/conf"
: > "$T/game.bin"
fail=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi; }

# the bounding box of what is not black in a PPM: "w h x y"
bbox() {
    python3 -I - "$1" <<'PY'
import sys
d = open(sys.argv[1], "rb").read()
parts = d.split(b"\n", 3)
w, h = map(int, parts[1].split())
px = parts[3]
xs, ys = [], []
for y in range(h):
    row = px[y * w * 3:(y + 1) * w * 3]
    if any(row):
        ys.append(y)
        nz = [x for x in range(w) if row[x * 3:x * 3 + 3] != b"\0\0\0"]
        xs += [nz[0], nz[-1]]
print(max(xs) - min(xs) + 1, max(ys) - min(ys) + 1, min(xs), min(ys))
PY
}
run() {   # name W H extra args...
    n=$1 w=$2 h=$3
    shift 3
    "$BIN" --video null --null-size "${w}x${h}" --audio null --input none --frames 30 --dump-frame 20:"$T/$n.ppm" \
        --log "$T/$n.log" "$@" "$T/game.bin" 2> "$T/$n.err"
}
run fit480 640 480
run fit768 1024 768
run int480 640 480 --scale integer
run int768 1024 768 --scale integer
check "fit on 640x480: 640x426 at 0,27" '[ "$(bbox "$T/fit480.ppm")" = "640 426 0 27" ]'
check "fit on 1024x768: 1024x682 at 0,43" '[ "$(bbox "$T/fit768.ppm")" = "1024 682 0 43" ]'
check "integer on 640x480: 480x320 at 80,80" '[ "$(bbox "$T/int480.ppm")" = "480 320 80 80" ]'
check "integer on 1024x768: 960x640 at 32,64" '[ "$(bbox "$T/int768.ppm")" = "960 640 32 64" ]'
# the saved setting: a conf with integer is read by the next run; the command line wins over it
mkdir -p "$T/conf"
echo "screen.scale=integer" > "$T/conf/fake.conf"
run saved 640 480
run over 640 480 --scale fit
check "the saved integer is used" '[ "$(bbox "$T/saved.ppm")" = "480 320 80 80" ]'
check "--scale fit wins over the saved one" '[ "$(bbox "$T/over.ppm")" = "640 426 0 27" ]'
[ $fail = 0 ] && echo "scale: all passed" || echo "scale: FAILED"
[ $fail = 0 ]
