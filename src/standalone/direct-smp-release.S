/* SPDX-License-Identifier: MIT */
/* Native and QEMU use these exact bytes for the terminal spin-table handoff.
 * Busy polling is intentional: no pre-Linux WFE/WFI or returning dispatcher.
 */
#include "direct-smp.h"
.section .text.m4_direct_smp_release,"ax"
.balign 16384
.global m4_direct_smp_release_start, m4_direct_smp_release_end
m4_direct_smp_release_start:
.global m4_direct_smp_linux_wait
.type m4_direct_smp_linux_wait,%function
m4_direct_smp_linux_wait:
    mrs x3,CurrentEL
    cmp x3,#8
    b.ne m4_direct_smp_release_park
    msr DAIFSet,#15
    adr x3,m4_direct_smp_release_early_vectors
    msr VBAR_EL2,x3
    isb
    mov x20,x0
    cbz x20,m4_direct_smp_release_park
    tst x20,#0x3fff
    b.ne m4_direct_smp_release_park
    ldr x3,[x20]
    ldr x4,=M4_SMP_MAGIC
    cmp x3,x4
    b.ne m4_direct_smp_release_park
    ldr x3,[x20,#8]
    cmp x3,#M4_SMP_INIT
    b.ne m4_direct_smp_release_park
    mrs x3,MPIDR_EL1
    ldr x4,=0xff00ffffff
    and x3,x3,x4
    ldr x4,[x20,#40]
    cmp x3,x4
    b.ne m4_direct_smp_release_park
    mrs x3,SPSel
    cmp x3,#1
    b.ne m4_direct_smp_release_park
    mov x3,sp
    ldr x4,[x20,#24]
    ldr x5,[x20,#32]
    tst x3,#15
    b.ne m4_direct_smp_release_park
    cmp x3,x4
    b.ls m4_direct_smp_release_park
    cmp x3,x5
    b.hi m4_direct_smp_release_park
    sub x6,x5,x4
    cmp x6,#16,lsl #12
    b.ne m4_direct_smp_release_park
    mrs x3,SCTLR_EL2
    mov x4,#0x1005
    tst x3,x4
    b.ne m4_direct_smp_release_park
    mrs x3,HCR_EL2
    tbnz x3,#0,m4_direct_smp_release_park
    ldr x3,[x20,#64]
    cbnz x3,m4_direct_smp_release_park
    adr x3,m4_direct_smp_release_vectors
    msr VBAR_EL2,x3
    isb
    mrs x3,CNTPCT_EL0
    str x3,[x20,#176]
    dsb sy
    mov x3,#M4_SMP_READY
    str x3,[x20,#8]
    dsb sy
1:  ldr x9,[x20,#64]
    cbz x9,1b
    dmb sy
    tst x9,#3
    b.ne m4_direct_smp_bad_release
    ldr x3,[x20,#48]
    ldr x4,[x20,#56]
    cbz x4,m4_direct_smp_bad_release
    adds x4,x3,x4
    b.cs m4_direct_smp_bad_release
    cmp x9,x3
    b.lo m4_direct_smp_bad_release
    cmp x9,x4
    b.hs m4_direct_smp_bad_release
    mov x3,#M4_SMP_RELEASED
    str x3,[x20,#8]
    dsb sy
    mov x0,xzr
    mov x1,xzr
    mov x2,xzr
    mov x3,xzr
    mov x4,xzr
    dsb sy
    ic iallu
    dsb sy
    isb
    br x9
.size m4_direct_smp_linux_wait,.-m4_direct_smp_linux_wait
m4_direct_smp_bad_release:
    mov x3,#61
    str x3,[x20,#16]
    mov x3,#M4_SMP_FAILED
    str x3,[x20,#8]
    dsb sy
    b m4_direct_smp_release_park
.balign 2048
m4_direct_smp_release_early_vectors:
.rept 16
    b m4_direct_smp_release_park
    .balign 128
.endr
.balign 2048
m4_direct_smp_release_vectors:
.rept 16
    b m4_direct_smp_release_fault
    .balign 128
.endr
m4_direct_smp_release_fault:
    msr DAIFSet,#15
    mrs x3,ESR_EL2
    str x3,[x20,#112]
    mrs x3,ELR_EL2
    str x3,[x20,#120]
    mrs x3,FAR_EL2
    str x3,[x20,#128]
    mov x3,sp
    str x3,[x20,#136]
    mov x3,#62
    str x3,[x20,#16]
    mov x3,#M4_SMP_FAILED
    str x3,[x20,#8]
    dsb sy
m4_direct_smp_release_park:
    nop
    b m4_direct_smp_release_park
    .ltorg
m4_direct_smp_release_end:
.section .note.GNU-stack,"",%progbits
