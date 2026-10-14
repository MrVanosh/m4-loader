/* R39 Level 1 blob: flat, position-independent, no writable data, no relocations.
 * Linked at 0; every reference is PC-relative (-mcmodel=tiny -fno-pic). */
ENTRY(l1_header)
SECTIONS {
    . = 0;
    .header : { KEEP(*(.l1header)) }
    .text : ALIGN(16) { *(.text .text.*) }
    .rodata : ALIGN(16) { *(.rodata .rodata.*) }
    .l1end : ALIGN(16) { LONG(0x444e4531); _l1_size = .; }
    /DISCARD/ : { *(.eh_frame*) *(.note*) *(.comment*) *(.ARM.attributes) *(.got*) *(.plt*) *(.igot*) }
    .data : { *(.data .data.*) }
    .bss : { *(.bss .bss.* COMMON) }
    ASSERT(SIZEOF(.data) == 0, "Level 1 blob must have no initialised writable data")
    ASSERT(SIZEOF(.bss) == 0, "Level 1 blob must have no static writable data")
    ASSERT(_l1_size <= 0x7ff0, "Level 1 blob does not fit the 32 KiB slot")
}
