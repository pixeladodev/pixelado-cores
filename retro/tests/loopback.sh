#!/bin/sh
# The link and the group's pause between two processes on this computer, with the fake core (no emulator, no
# display): a guest that starts before its host (the join is tried again), packets both ways through the host, the
# host's menu pausing the guest (no frame runs on either side while it is open), the save RAM written.
#   tests/loopback.sh build/pixelado-core-fake
set -eu
BIN=${1:?the frontend built with the fake core}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
PORT=$((20000 + $$ % 20000))
: > "$T/game.bin"
echo "100 menu" > "$T/host.script"
fail=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi; }

"$BIN" --room join --room-addr 127.0.0.1 --room-port $PORT --player 1 --nickname Kira --video null --audio null \
    --input none --frames 400 --save-dir "$T/c" --log "$T/client.log" "$T/game.bin" 2> "$T/client.err" &
C=$!
sleep 1
"$BIN" --room host --room-players 2 --room-port $PORT --nickname Ana --video null --audio null \
    --input "script:$T/host.script" --test-pause-ms 1500 --frames 400 --save-dir "$T/h" --log "$T/host.log" \
    "$T/game.bin" 2> "$T/host.err" &
H=$!
rc_c=0; rc_h=0
wait $C || rc_c=$?
wait $H || rc_h=$?

echo "--- host"; cat "$T/host.log" "$T/host.err"
echo "--- guest"; cat "$T/client.log" "$T/client.err"
echo "---"
check "both exit 0" '[ $rc_c = 0 ] && [ $rc_h = 0 ]'
check "the guest tried again until the host listened" 'grep -q "in the host.s session as client 1" "$T/client.log"'
check "the host saw the guest join" 'grep -q "Kira joined as client 1" "$T/host.log"'
hrx=$(sed -n 's/.*fake: id=0 .*received=\([0-9]*\).*/\1/p' "$T/host.err")
crx=$(sed -n 's/.*fake: id=1 .*received=\([0-9]*\).*/\1/p' "$T/client.err")
check "host received the guest's packets ($hrx)" '[ "${hrx:-0}" -gt 200 ]'
check "guest received the host's packets ($crx)" '[ "${crx:-0}" -gt 200 ]'
check "the host's packets came from client 0" 'grep -q "fake: id=1 .*from=0x1 " "$T/client.err"'
check "the guest's packets came from client 1" 'grep -q "fake: id=0 .*from=0x2 " "$T/host.err"'
# the pause: the guest stops at a frame and goes on from the same frame, for about the host's 1.5 s
p0=$(sed -n 's/^\[ *\([0-9.]*\)\] \[pause\] the others. menu (frame \([0-9]*\)).*/\1 \2/p' "$T/client.log" | head -n 1)
p1=$(sed -n 's/^\[ *\([0-9.]*\)\] \[pause\] over (frame \([0-9]*\)).*/\1 \2/p' "$T/client.log" | head -n 1)
check "the guest paused with the host's menu ($p0 -> $p1)" '[ -n "$p0" ] && [ -n "$p1" ]'
if [ -n "$p0" ] && [ -n "$p1" ]; then
    t0=${p0% *} f0=${p0#* } t1=${p1% *} f1=${p1#* }
    check "no frame ran on the guest while paused" '[ "$f0" = "$f1" ]'
    check "the guest's pause lasted the host's (>= 1.2 s)" 'awk -v a="$t0" -v b="$t1" "BEGIN { exit !(b - a >= 1.2) }"'
fi
check "the save RAM written" '[ "$(head -c 1 "$T/h/game.srm" 2>/dev/null)" = F ]'
[ $fail = 0 ] && echo "loopback: all passed" || echo "loopback: FAILED"
exit $fail
