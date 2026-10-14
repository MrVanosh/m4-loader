/* SPDX-License-Identifier: MIT */
/* Locked Apple RVBAR -> unchanged _vectors_start -> this one-shot entry.
 * No shared reset stack, console, generic exception dispatcher or WFE/WFI.
 */
#include "direct-smp.h"
.section .text.m4_direct_smp_reset,"ax"
.balign 16384
.global m4_direct_smp_reset_start, m4_direct_smp_reset_end
m4_direct_smp_reset_start:
.global m4_direct_smp_reset
.type m4_direct_smp_reset,%function
m4_direct_smp_reset:
    /* Unknown E reset translation/cache state cannot touch owned RAM. */
    mrs x2,CurrentEL
    cmp x2,#8
    b.ne m4_direct_smp_reset_park
    mrs x2,SCTLR_EL2
    mov x3,#0x1005
    tst x2,x3
    b.ne m4_direct_smp_reset_park
    adrp x3,m4_direct_indexed_native_gate
    ldr x3,[x3,:lo12:m4_direct_indexed_native_gate]
    cbz x3,m4_direct_smp_reset_park
    msr DAIFSet,#15
    mrs x3,CurrentEL
    cmp x3,#8
    b.ne m4_direct_smp_reset_park
    adr x3,m4_direct_smp_reset_early_vectors
    msr VBAR_EL2,x3
    isb
    bl m4_direct_smp_reset_lookup
    cbz x20,m4_direct_smp_reset_unknown
    ldr x3,[x20]
    ldr x4,=M4_SMP_MAGIC
    cmp x3,x4
    b.ne m4_direct_smp_reset_park
    ldr x3,[x20,#8]
    cmp x3,#M4_SMP_PREPARED
    b.ne m4_direct_smp_reset_park
    ldr x3,[x20,#40]
    mrs x4,MPIDR_EL1
    ldr x5,=0xff00ffffff
    and x4,x4,x5
    str x4,[x20,#72]
    cmp x3,x4
    b.ne m4_direct_smp_reset_park
    mrs x3,CurrentEL
    str x3,[x20,#864]
    ldr x3,[x20,#184]
    cmp x3,#1
    b.ne m4_direct_smp_reset_unattempted
    ldr x3,[x20,#24]
    ldr x4,[x20,#32]
    tst x3,#0x3fff
    b.ne m4_direct_smp_reset_park
    tst x4,#0x3fff
    b.ne m4_direct_smp_reset_park
    sub x5,x4,x3
    cmp x5,#16,lsl #12
    b.ne m4_direct_smp_reset_park
    msr SPSel,#1
    mov sp,x4
    adr x3,m4_direct_smp_reset_vectors
    msr VBAR_EL2,x3
    isb
    mov x3,#M4_SMP_RESET
    str x3,[x20,#8]
    dsb sy
    mov x0,x20
    bl m4_direct_smp_secondary
    b m4_direct_smp_reset_park
.size m4_direct_smp_reset,.-m4_direct_smp_reset

/* Stackless bounded identity lookup. Duplicate identities never select a record.
 * Faults repeat the lookup, rather than trust a C callee's current x20 value. */
.type m4_direct_smp_reset_lookup,%function
m4_direct_smp_reset_lookup:
    mov x20,xzr
    adrp x9,m4_direct_smp_record_count
    ldr x9,[x9,:lo12:m4_direct_smp_record_count]
    cbz x9,3f
    cmp x9,#M4_SMP_MAX_PEERS
    b.hi 3f
    mrs x10,MPIDR_EL1
    ldr x11,=0xff00ffffff
    and x10,x10,x11
    adrp x11,m4_direct_smp_states
    add x11,x11,:lo12:m4_direct_smp_states
1:  ldr x12,[x11,#40]
    cmp x12,x10
    b.ne 2f
    cbnz x20,4f
    mov x20,x11
2:  add x11,x11,#4,lsl #12
    subs x9,x9,#1
    b.ne 1b
3:  ret
4:  mov x20,xzr
    ret
.size m4_direct_smp_reset_lookup,.-m4_direct_smp_reset_lookup
.balign 2048
m4_direct_smp_reset_early_vectors:
.rept 16
    b m4_direct_smp_reset_park
    .balign 128
.endr
.balign 2048
m4_direct_smp_reset_vectors:
.rept 16
    b m4_direct_smp_reset_fault
    .balign 128
.endr
m4_direct_smp_reset_unattempted:
    mov x3,#65
    str x3,[x20,#16]
    mov x3,#M4_SMP_FAILED
    str x3,[x20,#8]
    dsb sy
    b m4_direct_smp_failed_park
m4_direct_smp_reset_fault:
    msr DAIFSet,#15
    bl m4_direct_smp_reset_lookup
    cbz x20,m4_direct_smp_reset_park
    mrs x3,ESR_EL2
    str x3,[x20,#112]
    mrs x3,ELR_EL2
    str x3,[x20,#120]
    mrs x3,FAR_EL2
    str x3,[x20,#128]
    mov x3,sp
    str x3,[x20,#136]
    mov x3,#63
    str x3,[x20,#16]
    mov x3,#M4_SMP_FAILED
    str x3,[x20,#8]
    dsb sy
    b m4_direct_smp_failed_park
.global m4_direct_smp_failed_park, m4_direct_smp_failed_loop
.type m4_direct_smp_failed_park,%function
m4_direct_smp_failed_park:
    msr DAIFSet,#15
    mrs x3,CurrentEL
    cmp x3,#8
    b.ne m4_direct_smp_reset_park
    bl m4_direct_smp_reset_lookup
    cbz x20,m4_direct_smp_reset_park
    ldr x3,[x20]
    ldr x4,=M4_SMP_MAGIC
    cmp x3,x4
    b.ne m4_direct_smp_reset_park
    ldr x3,[x20,#8]
    cmp x3,#M4_SMP_FAILED
    b.ne m4_direct_smp_reset_park
    mrs x3,DAIF
    str x3,[x20,#880]
    mrs x3,MPIDR_EL1
    ldr x4,=0xff00ffffff
    and x3,x3,x4
    str x3,[x20,#888]
    mrs x3,CurrentEL
    str x3,[x20,#896]
    adr x3,m4_direct_smp_failed_loop
    str x3,[x20,#904]
    dsb sy
m4_direct_smp_failed_loop:
    ldr x3,[x20,#872]
    add x3,x3,#1
    str x3,[x20,#872]
    dsb sy
    nop
    b m4_direct_smp_failed_loop
.size m4_direct_smp_failed_park,.-m4_direct_smp_failed_park
/* A mismatched first entry records only in the one explicitly launched slot.
 * No stack, console, initialization, translation change or release follows. */
m4_direct_smp_reset_unknown:
    adrp x9,m4_direct_smp_pending_peer
    ldr x9,[x9,:lo12:m4_direct_smp_pending_peer]
    cbz x9,m4_direct_smp_reset_park
    cmp x9,#M4_SMP_MAX_PEERS
    b.hi m4_direct_smp_reset_park
    sub x9,x9,#1
    adrp x20,m4_direct_smp_states
    add x20,x20,:lo12:m4_direct_smp_states
    add x20,x20,x9,lsl #14
    ldr x3,[x20]
    ldr x4,=M4_SMP_MAGIC
    cmp x3,x4
    b.ne m4_direct_smp_reset_park
    ldr x3,[x20,#8]
    cmp x3,#M4_SMP_PREPARED
    b.ne m4_direct_smp_reset_park
    ldr x3,[x20,#184]
    cmp x3,#1
    b.ne m4_direct_smp_reset_park
    mrs x3,MPIDR_EL1
    ldr x4,=0xff00ffffff
    and x3,x3,x4
    str x3,[x20,#72]
    mrs x3,CurrentEL
    str x3,[x20,#864]
    mov x3,#64
    str x3,[x20,#16]
    mov x3,#M4_SMP_FAILED
    str x3,[x20,#8]
    dsb sy
    b m4_direct_smp_reset_park
m4_direct_smp_reset_park:
    nop
    b m4_direct_smp_reset_park
    .ltorg
m4_direct_smp_reset_end:
.section .note.GNU-stack,"",%progbits
