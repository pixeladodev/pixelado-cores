#!/bin/sh
# Build the link test ROM (needs arm-none-eabi-gcc; gba/tests/run.sh has it in its Docker image).
#   gba/tests/linkrom/build.sh OUT.gba
set -eu
D=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
OUT=${1:?the ROM to write}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
CC=${ARM_CC:-arm-none-eabi-gcc}
F="-mcpu=arm7tdmi -mthumb-interwork -O2 -ffreestanding -fno-builtin -nostdlib -fno-tree-loop-distribute-patterns"
$CC $F -c -o "$T/crt0.o" "$D/crt0.s"
$CC $F -mthumb -c -o "$T/main.o" "$D/main.c"
$CC $F -T "$D/gba.ld" -o "$T/rom.elf" "$T/crt0.o" "$T/main.o" -lgcc
${ARM_OBJCOPY:-arm-none-eabi-objcopy} -O binary "$T/rom.elf" "$T/rom.bin"
# the header's checksum, and the size rounded up to 4 KiB
python3 - "$T/rom.bin" "$OUT" << 'PY'
import sys
b = bytearray(open(sys.argv[1], "rb").read())
b[0xBD] = (-(sum(b[0xA0:0xBD]) + 0x19)) & 0xFF
b += b"\xff" * (-len(b) % 4096)
open(sys.argv[2], "wb").write(b)
PY
echo "linkrom: $OUT ($(wc -c < "$OUT") bytes)"
