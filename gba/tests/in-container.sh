#!/bin/sh
# gba/tests/run.sh's half inside the arm64 container (the repository at /cores, read-only; everything built in /tmp).
set -eu
R=/cores
T=/tmp/t
mkdir -p "$T"
fail=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi; }
J=$(nproc)

echo "=== common/ (the channel with Party, the room's socket)"
make -s -C "$R/common" test OUT="$T/common" | grep -v '^  ' || fail=1

echo "=== retro/ with the fake core (the link's loopback, Party and the room)"
make -s -C "$R/retro" fake OUT="$T/retro" >/dev/null
sh "$R/retro/tests/loopback.sh" "$T/retro/pixelado-core-fake" | grep -E '^(ok|FAIL|loopback)' || fail=1
python3 "$R/retro/tests/party_room.py" "$T/retro/pixelado-core-fake" | grep -E '^(ok|FAIL|party_room)' || fail=1

echo "=== gpSP, built as on the handhelds (arm64 recompiler), and pixelado-core-gba"
sh "$R/gba/build/fetch-sources.sh" "$T/gpsp" >/dev/null
CC=gcc CXX=g++ AR=ar PIXELADO_CORE_JOBS=$J sh "$R/gba/build/build-core.sh" "$T/gpsp" "$T/core" > /dev/null 2> "$T/core.log" ||
    { tail -30 "$T/core.log"; exit 1; }
make -s -C "$R/retro" SYS=gba CORE_LIB="$T/core/libgpsp.a" VERSION=test OUT="$T/bin" \
    DEFAULT_OPTIONS="gpsp_sound_rate=32768" LDFLAGS="-static-libgcc" > "$T/bin.log" 2>&1 || { cat "$T/bin.log"; exit 1; }
BIN=$T/bin/pixelado-core-gba
check "pixelado-core-gba built (KMS, ALSA and evdev in)" '[ -x "$BIN" ]'
"$BIN" --version

echo "=== the link test ROM (built from gba/tests/linkrom)"
sh "$R/gba/tests/linkrom/build.sh" "$T/linktest.gba"

px() { python3 -c "
import sys
d=open(sys.argv[1],'rb').read(); h=d.split(b'\\n',3); w,hh=map(int,h[1].split()); px=h[3]
x,y=int(sys.argv[2]),int(sys.argv[3]); o=(y*w+x)*3; print('%02x%02x%02x'%(px[o],px[o+1],px[o+2]))" "$@"; }
green() { case $(px "$1" 320 200) in 00f[0-9a-f]00) return 0 ;; esac; return 1; }   # (gpSP's 555 to 565: 00fb00)
byte() { od -An -tu1 -j"$2" -N1 "$1" | tr -d ' '; }
chr() { od -An -c -j"$2" -N1 "$1" | tr -d ' '; }

# the ROM on one binary: alone, then two of it linked, the host opening its menu for 6 s once they are
rom_tests() {
    B=$1 D=$2 L=$3
    mkdir -p "$D/solo" "$D/h" "$D/g"
    echo "=== $L alone: the ROM boots and draws (the adapter answers, no host: it keeps looking)"
    printf C > "$D/solo/linktest.srm"
    "$B" --video null --audio null --input none --frames 120 --dump-frame "60:$D/solo.ppm" --option gpsp_serial=rfu \
        --save-dir "$D/solo" --log "$D/solo.log" "$T/linktest.gba" || true
    cat "$D/solo.log"
    check "$L: the picture, 2x and centred on a 640x480 panel: blue while looking ($(px "$D/solo.ppm" 320 200))" \
        '[ "$(px "$D/solo.ppm" 320 200)" = "0000ff" ]'
    check "$L: around it, the panel's black ($(px "$D/solo.ppm" 10 10))" '[ "$(px "$D/solo.ppm" 10 10)" = "000000" ]'
    check "$L: the guest went as far as reading broadcasts (step $(byte "$D/solo/linktest.srm" 2))" \
        '[ "$(byte "$D/solo/linktest.srm" 2)" = 3 ]'

    echo "=== $L linked: two cores over TCP, gpSP's wireless adapter on both; the host's menu open 6 s at frame 400"
    printf H > "$D/h/linktest.srm"
    printf C > "$D/g/linktest.srm"
    echo "400 menu" > "$D/host.script"
    "$B" --room join --room-addr 127.0.0.1 --player 1 --nickname Kira --video null --audio null --input none \
        --option gpsp_serial=rfu --frames 900 --dump-frame "880:$D/g.ppm" --save-dir "$D/g" --log "$D/g.log" \
        "$T/linktest.gba" &
    G=$!
    sleep 1
    "$B" --room host --room-players 2 --nickname Ana --video null --audio null --input "script:$D/host.script" \
        --test-pause-ms 6000 --option gpsp_serial=rfu --frames 900 --dump-frame "880:$D/h.ppm" --save-dir "$D/h" \
        --log "$D/h.log" "$T/linktest.gba" &
    H=$!
    wait $G || true
    wait $H || true
    echo "--- host"; cat "$D/h.log"; echo "--- guest"; cat "$D/g.log"; echo "---"
    check "$L: the host saw the guest connect (save: $(chr "$D/h/linktest.srm" 1), step $(byte "$D/h/linktest.srm" 2))" \
        '[ "$(chr "$D/h/linktest.srm" 1)" = K ]'
    check "$L: the guest found the host's broadcast and connected (save: $(chr "$D/g/linktest.srm" 1), step $(byte "$D/g/linktest.srm" 2))" \
        '[ "$(chr "$D/g/linktest.srm" 1)" = K ]'
    check "$L: both screens green ($(px "$D/h.ppm" 320 200) $(px "$D/g.ppm" 320 200))" 'green "$D/h.ppm" && green "$D/g.ppm"'
    check "$L: gpSP's link on the netpacket session" 'grep -q "link (netpacket)" "$D/h.log" && grep -q "Kira joined as client 1" "$D/h.log"'
    p0=$(sed -n 's/^\[ *\([0-9.]*\)\] \[pause\] the others. menu (frame \([0-9]*\)).*/\1 \2/p' "$D/g.log" | head -n 1)
    p1=$(sed -n 's/^\[ *\([0-9.]*\)\] \[pause\] over (frame \([0-9]*\)).*/\1 \2/p' "$D/g.log" | head -n 1)
    check "$L: the guest paused with the host's menu, no frame run meanwhile ($p0 -> $p1)" \
        '[ -n "$p0" ] && [ "${p0#* }" = "${p1#* }" ] && awk -v a="${p0% *}" -v b="${p1% *}" "BEGIN { exit !(b - a >= 5.5) }"'
    check "$L: the link stayed up through the 6 s pause (longer than gpSP's 4 s timeouts)" \
        '! grep -q "left: silent\|nothing from the host" "$D/g.log" "$D/h.log"'
}

rom_tests "$BIN" "$T/test" "test build"
if [ -x "$R/gba/dist/pixelado-core-gba" ]; then
    echo "=== the release binary (gba/dist, Bootlin's toolchain): $(sha256sum "$R/gba/dist/pixelado-core-gba" | cut -c1-16)"
    rom_tests "$R/gba/dist/pixelado-core-gba" "$T/dist" "release"
fi

echo "=== speed: unpaced and headless (the test ROM mostly waits for the vblank: an upper bound, not a game's speed)"
s=$(date +%s.%N)
"$BIN" --video null --audio null --input none --frames 3000 --unpaced --option gpsp_serial=disabled \
    --save-dir "$T/solo" --log "$T/speed.log" "$T/linktest.gba" || true
e=$(date +%s.%N)
python3 -c "import sys; print('3000 frames in %.2f s: %.0f fps' % (float(sys.argv[2]) - float(sys.argv[1]), 3000 / (float(sys.argv[2]) - float(sys.argv[1]))))" "$s" "$e"

[ $fail = 0 ] && echo "gba tests: all passed" || echo "gba tests: FAILED"
exit $fail
