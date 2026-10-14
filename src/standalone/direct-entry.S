/* SPDX-License-Identifier: MIT */
/* Terminal direct entry. Linux initializes EL2, its hyp stub and EL1/VHE. */
.section .text.m4_direct_enter,"ax"
.global m4_direct_enter
.type m4_direct_enter,%function
m4_direct_enter:
    /* x0=kernel physical entry, x1=validated FDT physical address. */
    msr DAIFSet, #15
    mov x9, x0
    mov x0, x1
    mov x1, xzr
    mov x2, xzr
    mov x3, xzr
    mov x4, xzr
    dsb sy
    ic iallu
    dsb sy
    isb
    br x9
.size m4_direct_enter, .-m4_direct_enter
.section .note.GNU-stack,"",%progbits
