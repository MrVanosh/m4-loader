/* SPDX-License-Identifier: MIT */
/* MMU/cache transitions for the direct loader's cached windows.
 * Sequence follows V12's natively accepted j616s_mmu_once: drain the
 * Device-nGnRnE stores, discard stale lines once, then translate; on the way
 * out disable first and clean+invalidate by VA with no memory access until
 * the clean is complete. Both are leaf functions using x0-x17 only.
 *
 * x0 = struct m4_cache_hw: 0 ttbr0, 8 tcr, 16 mair, 24 sctlr_off,
 * 32 sctlr_on, 40 saved_ttbr0, 48 saved_tcr, 56 saved_mair, 64 line,
 * 72 r0_base, 80 r0_end, 88 r1_base, 96 r1_end, 104 invalidate,
 * 112 f0_base, 120 f0_end, 128 e0_base, 136 e0_end, 144 e1_base, 152 e1_end.
 */
.macro M4_CACHE_RANGE op, base, end, line
    cmp \base, \end
    b.hs 2f
1:  dc \op, \base
    add \base, \base, \line
    cmp \base, \end
    b.lo 1b
2:
.endm

.section .text.m4_cache_on,"ax"
.balign 4
.global m4_cache_on
.type m4_cache_on,%function
/* Returns the SCTLR_EL2 value read back; anything but sctlr_on is a failure. */
m4_cache_on:
    mrs x9, SCTLR_EL2
    ldr x10, [x0, #24]
    cmp x9, x10
    b.ne 9f
    ldr x15, [x0, #64]
    ldr x16, [x0, #104]
    ldp x5, x6, [x0, #0]
    ldr x7, [x0, #16]
    ldr x8, [x0, #32]
    /* First window: the whole image and the ADT. Later windows: RW outside
     * the pool, which the CPU wrote uncached since the last clean. */
    ldp x11, x12, [x0, #112]
    ldp x13, x14, [x0, #88]
    cbnz x16, 3f
    ldp x11, x12, [x0, #128]
    ldp x13, x14, [x0, #144]
3:  /* Earlier stores used Device-nGnRnE semantics; complete them first. */
    dsb sy
    M4_CACHE_RANGE ivac, x11, x12, x15
    M4_CACHE_RANGE ivac, x13, x14, x15
    dsb sy
    ic iallu
    dsb sy
    isb
    msr MAIR_EL2, x7
    msr TCR_EL2, x6
    msr TTBR0_EL2, x5
    isb
    tlbi alle2
    dsb sy
    isb
    msr SCTLR_EL2, x8
    isb
9:  mrs x0, SCTLR_EL2
    ret
.size m4_cache_on, .-m4_cache_on

.section .text.m4_cache_off,"ax"
.balign 4
.global m4_cache_off
.type m4_cache_off,%function
/* Returns 0 after a completed transition, 1 if the cached state was not live. */
m4_cache_off:
    ldp x11, x12, [x0, #72]
    ldp x13, x14, [x0, #88]
    ldr x15, [x0, #64]
    ldr x10, [x0, #24]
    ldp x5, x6, [x0, #40]
    ldr x7, [x0, #56]
    mrs x9, SCTLR_EL2
    tst x9, #1
    b.eq 9f
    dsb sy
    msr SCTLR_EL2, x10
    isb
    /* No load/store until every RW line has reached PoC. */
    mov x16, x11
    M4_CACHE_RANGE civac, x11, x12, x15
    M4_CACHE_RANGE civac, x13, x14, x15
    /* Settle pass: the first MiB was cleaned within microseconds of the
     * disable; drop any line a prefetch issued before it filled since. */
    add x17, x16, #0x100000
    cmp x17, x12
    csel x17, x17, x12, lo
    M4_CACHE_RANGE civac, x16, x17, x15
    dsb sy
    ic iallu
    dsb sy
    isb
    msr TTBR0_EL2, x5
    msr TCR_EL2, x6
    msr MAIR_EL2, x7
    isb
    tlbi alle2
    dsb sy
    isb
    mov x0, #0
    ret
9:  mov x0, #1
    ret
.size m4_cache_off, .-m4_cache_off
.section .text.m4_cache_clean_range,"ax"
.balign 4
.global m4_cache_clean_range
.type m4_cache_clean_range,%function
/* x0 base, x1 end, x2 line: clean+invalidate to PoC by VA (MMU off). */
m4_cache_clean_range:
    dsb sy
    M4_CACHE_RANGE civac, x0, x1, x2
    dsb sy
    ret
.size m4_cache_clean_range, .-m4_cache_clean_range
.section .note.GNU-stack,"",%progbits
