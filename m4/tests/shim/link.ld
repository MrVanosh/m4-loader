/* QEMU virt: RAM at 0x40000000. */
ENTRY(_start)
SECTIONS {
    . = 0x40000000;
    .text : { *(.text.start) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    .bss : ALIGN(16) { *(.bss .bss.*) *(COMMON) . = ALIGN(16); _stack_bottom = .; . += 0x10000; _stack_top = .; }
    /DISCARD/ : { *(.note*) *(.comment*) *(.eh_frame*) }
}
