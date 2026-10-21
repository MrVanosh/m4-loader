/* QEMU virt, -cpu max -machine virt,virtualization=on: the image starts at EL2.
 * FLAVOR_EL2: run the production guard (EL2 registers) at EL2.
 * FLAVOR_EL1: configure HCR_EL2 (RW, AMO), install an HVC service for virtual-SError
 *             injection and drop to EL1, where the same guard source is built against
 *             the EL1 registers. */
.section .text.start,"ax"
.global _start
_start:
    ldr x0, =_stack_top
    mov sp, x0
    msr DAIFSet, #0xf
#ifdef FLAVOR_EL1
    adr x0, el2_vectors
    msr VBAR_EL2, x0
    mov x0, #(1 << 31)            /* HCR_EL2.RW */
    orr x0, x0, #(1 << 5)         /* AMO: SError routed to EL2, virtual SError for EL1 */
    msr HCR_EL2, x0
    msr SCTLR_EL1, xzr
    ldr x0, =_stack_top
    msr SP_EL1, x0
    mov x0, #0x3c5                /* EL1h, DAIF masked */
    msr SPSR_EL2, x0
    ldr x0, =el1_entry
    msr ELR_EL2, x0
    isb
    eret
el1_entry:
    bl test_main
    b .
.balign 2048
el2_vectors:
    .rept 8
    b el2_unexpected
    .balign 128
    .endr
.org el2_vectors + 0x400            /* lower EL, AArch64, synchronous: the HVC service */
    mrs x9, ESR_EL2
    lsr x10, x9, #26
    cmp x10, #0x16
    b.ne el2_unexpected
    and x10, x9, #0xffff
    cmp x10, #1
    b.ne 1f
    mrs x11, HCR_EL2              /* 1: pend a virtual SError */
    orr x11, x11, #(1 << 8)
    msr HCR_EL2, x11
    eret
1:  cmp x10, #2
    b.ne el2_unexpected
    mrs x11, HCR_EL2              /* 2: clear it */
    bic x11, x11, #(1 << 8)
    msr HCR_EL2, x11
    eret
el2_unexpected:
    mrs x0, ESR_EL2
    mrs x1, ELR_EL2
    bl report_unexpected
    b .
#else
    adrp x0, test_vectors
    add x0, x0, :lo12:test_vectors
    msr VBAR_EL2, x0
    mov x0, #0x180
    movk x0, #0x3000, lsl #16     /* the production SCTLR_EL2 baseline the guard accepts */
    msr SCTLR_EL2, x0
    isb
    bl test_main
    b .
.balign 2048
.global test_vectors
test_vectors:
    .rept 16
    mov x0, #0xdead
    b unexpected
    .balign 128
    .endr
unexpected:
    mrs x0, ESR_EL2
    mrs x1, ELR_EL2
    bl report_unexpected
    b .
#endif
.section .note.GNU-stack,"",%progbits
