/* SPDX-License-Identifier: GPL-2.0-only */
#include "../build/build_cfg.h"
#ifdef J616S_SCREEN_DIAGNOSTIC
/* x14: 0=uncached/clean, 1=preparing, 2=cache-active, 3=cleaning.
 * x15=line size, x16=incoming SCTLR, x17/x18=image bounds, x8=test seed.
 * No stack/data access is allowed between cache disable and completed clean.
 * The macro clobbers only x11; fault ESR/ELR in x9/x10 survive it. */
.macro CACHE_OFF
    dsb sy
    msr SCTLR_EL2, x16
    isb
    cmp x14, #2
    b.ne .Lclean_done\@
    mov x14, #3
    mov x11, x17
.Lclean_loop\@:
    dc civac, x11
    add x11, x11, x15
    cmp x11, x18
    b.lo .Lclean_loop\@
    dsb sy
    ic iallu
    dsb sy
    isb
    mov x14, #0
.Lclean_done\@:
.endm
.section .text.j616s_mmu_once,"ax"
.balign 4
.global j616s_mmu_once
.type j616s_mmu_once,%function
j616s_mmu_once:
    mrs x9, CurrentEL
    cmp x9, #8
    b.ne .Lrefuse
    mrs x9, SPSel
    cbz x9, .Lrefuse
    mrs x16, SCTLR_EL2
    ldr x9, =0x2001005 /* EE, I, C, M must all be off. */
    tst x16, x9
    b.ne .Lrefuse
    ldr x14, [x4]
    cmp x14, #1
    b.hi .Lrefuse
    ldp x17, x18, [x4, #8]
    ldr x15, [x4, #24]
    cbz x17, .Lrefuse
    cmp x18, x17
    b.ls .Lrefuse
    orr x9, x17, x18
    tst x9, #0x3fff
    b.ne .Lrefuse
    mov x9, #1
    lsl x9, x9, #42
    cmp x18, x9
    b.hi .Lrefuse
    sub x9, x18, x17
    mov x12, #0x1000000 /* at most 16MiB */
    cmp x9, x12
    b.hi .Lrefuse
    cmp x15, #16
    b.lo .Lrefuse
    cmp x15, #2048
    b.hi .Lrefuse
    sub x9, x15, #1
    tst x15, x9
    b.ne .Lrefuse
    stp x29, x30, [sp, #-128]!
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
    mrs x9, FAR_EL2
    str x9, [sp, #56]
    str x16, [sp, #64]
    mrs x9, TCR_EL2
    str x9, [sp, #72]
    mrs x9, TTBR0_EL2
    str x9, [sp, #80]
    mrs x9, MAIR_EL2
    str x9, [sp, #88]
    mrs x9, PAR_EL1
    str x9, [sp, #96]
    str x3, [sp, #104]
    str x2, [sp, #112]
    stp xzr, xzr, [x3]
    stp xzr, xzr, [x3, #16]
    stp xzr, xzr, [x3, #32]
    stp xzr, xzr, [x3, #48]
    mov x9, #1
    str x9, [x3]
    adrp x9, j616s_mmu_vectors
    add x9, x9, :lo12:j616s_mmu_vectors
    msr VBAR_EL2, x9
    isb
    cbz x14, .Lprepare_nc
    /* Prior writes used MMU/cache-off semantics. Drain them before discarding
     * stale cache lines for our own image. No external addresses are touched. */
    dsb sy
    mov x9, x17
.Linvalidate:
    dc ivac, x9
    add x9, x9, x15
    cmp x9, x18
    b.lo .Linvalidate
    dsb sy
    ic iallu
    dsb sy
    isb
    mov x9, #0xff /* Normal inner/outer write-back, read/write allocate. */
    ldr x8, =0x616512120000abcd
    b .Lprepare_tables
.Lprepare_nc:
    mov x9, #0x44 /* Normal non-cacheable reference phase. */
    ldr x8, =0x616511110000abcd
.Lprepare_tables:
    msr MAIR_EL2, x9
    msr TCR_EL2, x1
    msr TTBR0_EL2, x0
    dsb sy
    isb
    tlbi alle2
    dsb sy
    isb
    orr x9, x16, #1
    cbz x14, .Lenable
    orr x9, x9, #4
    orr x9, x9, #0x1000
    mov x14, #2
.Lenable:
    msr SCTLR_EL2, x9
    isb
    mrs x10, SCTLR_EL2
    str x10, [x3, #8]
    cmp x9, x10
    b.ne .Lfailed
    mov x9, #2
    str x9, [x3]
    mov x10, #0
.Lfill:
    eor x9, x8, x10
    str x9, [x2, x10, lsl #3]
    add x10, x10, #1
    cmp x10, #2048
    b.lo .Lfill
    dsb sy
    mov x10, #2048
.Lcheck:
    sub x10, x10, #1
    ldr x11, [x2, x10, lsl #3]
    eor x9, x8, x10
    cmp x9, x11
    b.ne .Lfailed
    cbnz x10, .Lcheck
    str x8, [x3, #16]
    adr x9, j616s_mmu_once
    at s1e2r, x9
    isb
    mrs x10, PAR_EL1
    str x10, [x3, #48]
    tbnz x10, #0, .Lfailed
    ldr x11, =0x0000fffffffff000
    and x10, x10, x11
    and x9, x9, x11
    cmp x9, x10
    b.ne .Lfailed
    mov x1, #0 /* Deliberately unmapped VA; never read unless translation fails. */
    at s1e2r, x1
    isb
    mrs x10, PAR_EL1
    str x10, [x3, #56]
    tbz x10, #0, .Lfailed
    mov x9, #3
    str x9, [x3]
.global j616s_mmu_guard_pc
j616s_mmu_guard_pc:
    ldr x0, [x1]
    ldr x3, [sp, #104]
    ldr x9, [x3]
    cmp x9, #4
    b.ne .Lfailed
    str xzr, [x3] /* All checks, including expected abort, passed. */
    b .Lrestore
.Lfailed:
    /* Nonzero status already identifies last reached stage or unexpected fault. */
.Lrestore:
    CACHE_OFF
    tlbi alle2
    dsb sy
    isb
    ldr x9, [sp, #72]
    msr TCR_EL2, x9
    ldr x9, [sp, #80]
    msr TTBR0_EL2, x9
    ldr x9, [sp, #88]
    msr MAIR_EL2, x9
    isb
    ldr x9, [sp, #96]
    msr PAR_EL1, x9
    ldr x9, [sp, #56]
    msr FAR_EL2, x9
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
    ldp x29, x30, [sp], #128
    mov w0, #0
    ret
.Lrefuse:
    mov w0, #-1
    ret
.size j616s_mmu_once, .-j616s_mmu_once

.section .text.j616s_mmu_handler,"ax"
.Lsync:
    cmp x14, #3
    b.eq .Lcache_stop /* Never recurse through failing cache maintenance. */
    mrs x9, ESR_EL2
    mrs x10, ELR_EL2
    mrs x11, SPSR_EL2
    and x12, x11, #31
    cmp x12, #9
    b.eq .Lmode_ok
    mov x3, #4
    b .Lunrecoverable
.Lmode_ok:
    lsr x12, x9, #26
    cmp x12, #0x25 /* Same-EL Data Abort */
    b.ne .Lunexpected
    and x12, x9, #0x3c
    cmp x12, #4 /* Translation fault, any level */
    b.ne .Lunexpected
    tbnz x9, #6, .Lunexpected /* WnR */
    tbnz x9, #10, .Lunexpected /* FnV */
    adr x12, j616s_mmu_guard_pc
    cmp x10, x12
    b.ne .Lunexpected
    mrs x12, FAR_EL2
    cbnz x12, .Lunexpected
    ldr x3, [sp, #104]
    ldr x13, [x3]
    cmp x13, #3
    b.ne .Lunexpected
    stp x9, x10, [x3, #24]
    str x12, [x3, #40]
    mov x13, #4
    str x13, [x3]
    add x10, x10, #4
    msr ELR_EL2, x10
    eret
.Lunexpected:
    /* Complete writeback with identity physical addresses before any memory
     * access under the restored cache-off configuration. */
    CACHE_OFF
    ldr x3, [sp, #104]
    mov x12, #0x20
    str x12, [x3]
    stp x9, x10, [x3, #24]
    mrs x12, FAR_EL2
    str x12, [x3, #40]
    adr x12, .Lfailed
    msr ELR_EL2, x12
    eret
.Lunrecoverable:
    cmp x14, #3
    b.eq .Lcache_stop
    CACHE_OFF
    mrs x0, ESR_EL2
    mrs x1, ELR_EL2
    mrs x2, SPSR_EL2
    b j616s_probe_unexpected
.Lcache_stop:
    /* Memory/stack coherence is no longer known if cache maintenance itself
     * faults. Leave the previously flushed screen and stop without C calls. */
    msr DAIFSet, #15
    b .Lcache_stop

.section .text.j616s_mmu_vectors,"ax"
.balign 2048
.global j616s_mmu_vectors
j616s_mmu_vectors:
.set vector, 0
.rept 16
    .if vector == 4
        b .Lsync
    .else
        mov x3, #vector
        b .Lunrecoverable
    .endif
    .balign 128
    .set vector, vector + 1
.endr
.global j616s_mmu_vectors_end
j616s_mmu_vectors_end:
#endif
.section .note.GNU-stack,"",%progbits
