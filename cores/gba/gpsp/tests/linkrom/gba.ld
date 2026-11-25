/* The link test ROM: everything in the cartridge's ROM, nothing to copy at start. */
ENTRY(_start)
MEMORY
{
    rom (rx) : ORIGIN = 0x08000000, LENGTH = 32M
}
SECTIONS
{
    .text : { KEEP(*(.text.start)) *(.text*) *(.rodata*) } > rom
    .data : { *(.data*) *(.bss*) *(COMMON) } > rom
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) }
}
ASSERT(SIZEOF(.data) == 0, "the link test ROM has no data or bss")
