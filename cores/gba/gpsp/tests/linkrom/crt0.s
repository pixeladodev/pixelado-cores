@ The link test ROM's start: the cartridge header (no Nintendo logo: the emulator's built-in BIOS doesn't check it),
@ the stacks, then main() in Thumb. Built by cores/gba/gpsp/tests/linkrom/build.sh; GPL-3.0-or-later like the rest of this repo.
    .section .text.start, "ax"
    .arm
    .global _start
_start:
    b       reset
    .fill   156, 1, 0               @ 0x04: the logo (left empty)
    .ascii  "PXLINKTEST  "          @ 0xA0: the title, 12 bytes
    .ascii  "PXLT"                  @ 0xAC: the game code (not in any database: the link mode comes from the option)
    .ascii  "00"                    @ 0xB0: the maker
    .byte   0x96, 0x00, 0x00        @ 0xB2: fixed, unit, device
    .fill   7, 1, 0                 @ 0xB5: reserved
    .byte   0x00                    @ 0xBC: version
    .byte   0x00                    @ 0xBD: the header's checksum (build.sh writes it)
    .byte   0x00, 0x00              @ 0xBE: reserved
reset:
    mov     r0, #0x12               @ IRQ mode's stack
    msr     cpsr_c, r0
    ldr     sp, =0x03007FA0
    mov     r0, #0x1F               @ system mode's stack
    msr     cpsr_c, r0
    ldr     sp, =0x03007F00
    ldr     r0, =main
    mov     lr, pc
    bx      r0
1:  b       1b
    .ltorg
    .align  2
    .ascii  "SRAM_V113"             @ the save type, as the cartridge libraries mark it
    .align  2
