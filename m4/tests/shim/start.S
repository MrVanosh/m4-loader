/* QEMU virt, -machine virt,virtualization=on: starts at EL2. Production flavor of the guard (EL2 registers). */
.section .text.start,"ax"
.global _start
_start:
    ldr x0, =_stack_top
    mov sp, x0
    msr DAIFSet, #0xf
    adrp x0, test_vectors
    add x0, x0, :lo12:test_vectors
    msr VBAR_EL2, x0
    mov x0, #0x180
    movk x0, #0x3000, lsl #16     /* the SCTLR_EL2 baseline the production guard accepts */
    msr SCTLR_EL2, x0
    ldr x0, =1000000000
    msr CNTFRQ_EL0, x0            /* the loader requires a 1 GHz counter */
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
.section .note.GNU-stack,"",%progbits
