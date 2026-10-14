/* SPDX-License-Identifier: GPL-2.0-only */
#include "../build/build_cfg.h"
#ifdef J616S_SCREEN_DIAGNOSTIC

/* This is a deliberately separate EL2-only exception path. It never calls
 * normal exception_initialize(), changes HCR/SCTLR, or programs peripherals.
 * Every probe restores its incoming VBAR, DAIF, ESR, ELR and SPSR. */
.section .text.j616s_probe_once, "ax"
.balign 4
.global j616s_probe_once
.type j616s_probe_once, %function
j616s_probe_once:
    mrs x9, CurrentEL
    cmp x9, #8
    b.ne .Lrefuse
    mrs x9, SPSel
    cbz x9, .Lrefuse
    cmp w0, #4
    b.hi .Lrefuse
    stp x29, x30, [sp, #-64]!
    mrs x9, VBAR_EL2
    str x9, [sp, #16]
    mrs x9, DAIF
    str x9, [sp, #24]
    msr DAIFSet, #15
    mrs x9, ELR_EL2
    str x9, [sp, #32]
    mrs x9, SPSR_EL2
    str x9, [sp, #40]
    mrs x9, ESR_EL2
    str x9, [sp, #48]
    str x1, [sp, #56]
    stp xzr, xzr, [x1]
    stp xzr, xzr, [x1, #16]
    adrp x9, j616s_probe_vectors
    add x9, x9, :lo12:j616s_probe_vectors
    msr VBAR_EL2, x9
    isb
    cbz w0, .Lread_el
    cmp w0, #1
    b.eq .Lread_brk
    cmp w0, #2
    b.eq .Lread_udf
    cmp w0, #3
    b.eq .Lread_lower
    b .Lread_upper
.Lread_el:
    mrs x0, CurrentEL
    b .Lfinish
.global j616s_probe_brk_pc
j616s_probe_brk_pc:
.Lread_brk:
    brk #0x616
    b .Lfinish
.global j616s_probe_udf_pc
j616s_probe_udf_pc:
.Lread_udf:
    .inst 0 /* Architecturally undefined; test unknown-instruction recovery. */
    b .Lfinish
.global j616s_probe_lower_pc
j616s_probe_lower_pc:
.Lread_lower:
    /* Existing pinned cpu_regs.h: SYS_IMP_APL_CTRR_LOWER_EL1. Raw only. */
    mrs x0, S3_0_C11_C0_0
    b .Lfinish
.global j616s_probe_upper_pc
j616s_probe_upper_pc:
.Lread_upper:
    /* Existing pinned cpu_regs.h: SYS_IMP_APL_CTRR_UPPER_EL1. Raw only. */
    mrs x0, S3_0_C11_C0_1
.Lfinish:
    ldr x1, [sp, #56]
    str x0, [x1, #8]
    ldr x9, [sp, #48]
    msr ESR_EL2, x9
    ldr x9, [sp, #40]
    msr SPSR_EL2, x9
    ldr x9, [sp, #32]
    msr ELR_EL2, x9
    ldr x9, [sp, #16]
    msr VBAR_EL2, x9
    isb
    ldr x9, [sp, #24]
    msr DAIF, x9
    ldp x29, x30, [sp], #64
    mov w0, #0
    ret
.Lrefuse:
    mov w0, #-1
    ret
.size j616s_probe_once, . - j616s_probe_once

/* Only current-EL SPx synchronous exceptions can recover. No C calls and no
 * extra stack use on that path; x0-x10 are caller-clobbered probe scratch. */
.section .text.j616s_probe_handler, "ax"
.Lsync:
    mrs x2, ESR_EL2
    mrs x3, ELR_EL2
    mrs x4, SPSR_EL2
    and x5, x4, #31
    cmp x5, #9 /* EL2h, AArch64 */
    b.ne .Lfatal_sync
    lsr x5, x2, #26
    and x5, x5, #63
    adr x6, j616s_probe_brk_pc
    cmp x3, x6
    b.eq .Lcheck_brk
    adr x6, j616s_probe_udf_pc
    cmp x3, x6
    b.eq .Lcheck_udf
    adr x6, j616s_probe_lower_pc
    cmp x3, x6
    b.eq .Lcheck_mrs
    adr x6, j616s_probe_upper_pc
    cmp x3, x6
    b.ne .Lfatal_sync
.Lcheck_mrs:
    cbz x5, .Lrecover /* Undefined system register access. */
    cmp x5, #0x18 /* Architected trapped AArch64 MRS/MSR class. */
    b.eq .Lrecover
    b .Lfatal_sync
.Lcheck_udf:
    cbz x5, .Lrecover
    b .Lfatal_sync
.Lcheck_brk:
    cmp x5, #0x3c
    b.ne .Lfatal_sync
    and x5, x2, #0x1ffffff
    cmp x5, #0x616
    b.ne .Lfatal_sync
.Lrecover:
    ldr x1, [sp, #56]
    mov x0, #1
    str x0, [x1]
    stp x2, x3, [x1, #16]
    add x3, x3, #4
    msr ELR_EL2, x3
    mov x0, #0 /* No value is reported for a trapped read. */
    eret
.Lfatal_sync:
    mov x3, #4
.Lfatal:
    msr DAIFSet, #15
    mrs x0, ESR_EL2
    mrs x1, ELR_EL2
    mrs x2, SPSR_EL2
    b j616s_probe_unexpected

.section .text.j616s_probe_vectors, "ax"
.balign 2048
.global j616s_probe_vectors
j616s_probe_vectors:
.set vector, 0
.rept 16
    .if vector == 4
        b .Lsync
    .else
        mov x3, #vector
        b .Lfatal
    .endif
    .balign 128
    .set vector, vector + 1
.endr
.global j616s_probe_vectors_end
j616s_probe_vectors_end:
#endif
.section .note.GNU-stack,"",%progbits
