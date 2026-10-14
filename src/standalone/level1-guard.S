/* SPDX-License-Identifier: MIT */
/* R41 Level 1 v2: the abort guard of the Level 1 attempt.
 *
 *   int m4_l1_guard_call(int (*fn)(void *), void *arg, struct m4_l1_guard *g);
 *
 * R39's guard (j616s_exec_call) masked SError for the whole guarded call, so an
 * asynchronous bus error raised by an ANS/PMGR/SART register write stayed
 * pending, fired when the guard restored the outer DAIF (outside every Level 1
 * handler) and stopped the boot. This guard takes the SError INSIDE the call:
 *
 *   - D, I and F are masked, A (SError) is UNMASKED for the guarded call;
 *   - ISR_EL1 is read at entry (recorded in g->isr); an SError that is already
 *     pending is taken at the first isb and reported as status 2;
 *   - every one of the eight current-EL vectors (SP0/SPx x sync/IRQ/FIQ/SError)
 *     aborts the call (all ESR classes, not only four), records ESR/ELR/FAR and
 *     the vector slot, unwinds to this frame and returns;
 *   - before a normal return: dsb sy; isb with A still unmasked (a late SError is
 *     taken here), then A is masked, ISR_EL1.A is re-read and, if an SError
 *     arrived after the barrier, it is unmasked once more so it is taken inside;
 *   - VBAR, DAIF, ELR, SPSR, ESR and FAR of the caller are restored.
 *
 * Frames are chained (m4_l1_guard_top), so the guard nests: inside the loader's
 * own outer guard (j616s_usb_call), inside a stand-in of itself, and phase guards
 * inside the whole-attempt guard. Depth is bounded (8). One frame is 192 bytes
 * on the caller's stack; the only static word is m4_l1_guard_top (8 bytes, .bss).
 *
 * Result g (8 x u64): status, value, esr, elr, far, vector, isr, daif
 *   status 0 ran to completion      1 aborted inside fn (vector/esr/elr/far valid)
 *          2 an SError that was pending at entry was taken (earlier code's fault): fn was NOT run
 *          3 refused, fn not run, the record is zeroed except: value = reason 1 not at the guard's EL,
 *            2 SP_EL0 in use, 3 SCTLR not a baseline, 4 SCTLR is the cached-window value but the window is
 *            not live, 5 nesting too deep; esr = SCTLR of the guard's EL (reason 3, 4), vector = 0xff
 *   vector = slot index 0..7 (offset / 0x80: 0-3 current EL SP0, 4-7 SPx; sync, IRQ, FIQ, SError)
 * Returns 0 when the guard operated (look at g->status), -1 when it refused.
 *
 * Build: production = EL2 (EL2 system registers); -DM4_L1G_EL1 builds the same
 * code against the EL1 registers for the QEMU test (virtual SError injection is
 * only possible for a guest that runs at EL1); -DM4_L1G_NOCHECK drops the
 * SCTLR_EL2 baseline check (the test images run with other SCTLR values). */

#ifdef M4_L1G_EL1
#define EL_CUR      4
#define R_VBAR      VBAR_EL1
#define R_ELR       ELR_EL1
#define R_SPSR      SPSR_EL1
#define R_ESR       ESR_EL1
#define R_FAR       FAR_EL1
#define R_SCTLR     SCTLR_EL1
#define SPSR_RET    0x3c5          /* EL1h, DAIF masked */
#else
#define EL_CUR      8
#define R_VBAR      VBAR_EL2
#define R_ELR       ELR_EL2
#define R_SPSR      SPSR_EL2
#define R_ESR       ESR_EL2
#define R_FAR       FAR_EL2
#define R_SCTLR     SCTLR_EL2
#define SPSR_RET    0x3c9          /* EL2h, DAIF masked */
#endif

.section .bss.m4_l1_guard_top,"aw",%nobits
.balign 8
.global m4_l1_guard_top
m4_l1_guard_top: .skip 8

/* The vector table must be 2 KiB aligned. Slots 0..7 (the current EL) live at
 * their fixed offsets; the lower-EL slots (8..15, never taken: the loader owns EL2
 * and nothing runs below it) hold the guard code, so the table costs no extra text. */
.section .text.m4_l1_guard,"ax"
.balign 2048
.global m4_l1_guard_vectors
m4_l1_guard_vectors:
.macro VEC idx
.org m4_l1_guard_vectors + \idx * 0x80
    mov x9, #\idx
    b .Labort
.endm
    VEC 0
    VEC 1
    VEC 2
    VEC 3
    VEC 4
    VEC 5
    VEC 6
    VEC 7
.org m4_l1_guard_vectors + 0x400

.global m4_l1_guard_call
.type m4_l1_guard_call,%function
m4_l1_guard_call:
    mov x15, #1                             /* x15 = refusal reason, see the header */
    mrs x9, CurrentEL
    cmp x9, #EL_CUR
    b.ne .Lrefuse
    mov x15, #2
    mrs x9, SPSel
    cbz x9, .Lrefuse
#ifndef M4_L1G_NOCHECK
    mov x15, #3
    /* same baselines as j616s_exec_call: the plain loader SCTLR, or the cached
     * window's (only while the window is live) */
    mrs x9, SCTLR_EL2
    mov x10, #0x180
    movk x10, #0x3000, lsl #16
    cmp x9, x10
    b.eq .Lsctlr_ok
    mov x10, #0x1185
    movk x10, #0x3000, lsl #16
    cmp x9, x10
    b.ne .Lrefuse
    mov x15, #4
    adrp x10, m4_cache_live
    ldr x10, [x10, :lo12:m4_cache_live]
    cbz x10, .Lrefuse
.Lsctlr_ok:
#endif
    adrp x12, m4_l1_guard_top
    ldr x13, [x12, :lo12:m4_l1_guard_top]   /* previous frame, 0 = outermost */
    mov x14, #0                             /* nesting depth */
    cbz x13, .Lframe
    ldr x14, [x13, #168]
    add x14, x14, #1
    mov x15, #5
    cmp x14, #8
    b.hi .Lrefuse
.Lframe:
    sub sp, sp, #192
    stp x19, x20, [sp]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    mrs x10, R_VBAR
    mrs x11, DAIF
    stp x10, x11, [sp, #96]
    mrs x10, R_ELR
    mrs x11, R_SPSR
    stp x10, x11, [sp, #112]
    mrs x10, R_ESR
    mrs x11, R_FAR
    stp x10, x11, [sp, #128]
    stp x2, x13, [sp, #144]                 /* result, previous frame */
    stp xzr, x14, [sp, #160]                /* entered = 0, depth */
    stp xzr, xzr, [x2]
    stp xzr, xzr, [x2, #16]
    str xzr, [x2, #32]
    mov x10, #0xff
    mrs x15, ISR_EL1
    stp x10, x15, [x2, #40]                 /* vector (none yet), ISR_EL1 at entry */
    mrs x11, DAIF
    str x11, [x2, #56]
    mov x10, sp
    str x10, [x12, :lo12:m4_l1_guard_top]   /* this frame is now the innermost */
    adrp x10, m4_l1_guard_vectors
    add x10, x10, :lo12:m4_l1_guard_vectors
    msr R_VBAR, x10
    isb
    msr DAIFSet, #0xb                       /* D, I, F masked */
    msr DAIFClr, #0x4                       /* A unmasked */
    isb                                     /* an SError pending from earlier code is taken here (status 2) */
    mov x10, #1
    str x10, [sp, #160]                     /* entered */
    mov x9, x0
    mov x0, x1
    blr x9
    ldr x1, [sp, #144]
    mov w9, w0
    str x9, [x1, #8]
    dsb sy
    isb                                     /* a late SError is taken here, inside the guard */
    msr DAIFSet, #0x4
    isb
    mrs x9, ISR_EL1
    tbz x9, #8, .Lrestore
    msr DAIFClr, #0x4                       /* arrived after the barrier: take it now, inside */
    isb
    nop
    nop
    msr DAIFSet, #0x4                       /* not taken (spurious ISR bit): carry on */
.Lrestore:                                  /* also the abort return point; sp = this frame */
    ldp x10, x11, [sp, #128]
    msr R_ESR, x10
    msr R_FAR, x11
    ldp x10, x11, [sp, #112]
    msr R_ELR, x10
    msr R_SPSR, x11
    ldr x13, [sp, #152]
    adrp x12, m4_l1_guard_top
    str x13, [x12, :lo12:m4_l1_guard_top]
    ldp x10, x11, [sp, #96]
    msr R_VBAR, x10
    isb
    msr DAIF, x11
    ldp x19, x20, [sp]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    add sp, sp, #192
    mov w0, #0
    ret
.Lrefuse:                                   /* nothing touched; the whole record is defined */
    mov x9, #3
    stp x9, x15, [x2]                       /* status 3, value = reason */
    stp xzr, xzr, [x2, #16]
    str xzr, [x2, #32]
    mov x10, #0xff
    str x10, [x2, #40]
    str xzr, [x2, #48]
    str xzr, [x2, #56]
    cmp x15, #3
    b.lo 1f                                 /* reasons 1, 2: the EL may not even have this register */
    mrs x10, R_SCTLR
    str x10, [x2, #16]                      /* esr = the SCTLR that was refused */
1:  mov w0, #-1
    ret
.size m4_l1_guard_call, .-m4_l1_guard_call

.Labort:                                    /* x9 = vector slot index; any exception class */
    mrs x10, R_ESR
    mrs x11, R_ELR
    mrs x12, R_FAR
    adrp x13, m4_l1_guard_top
    ldr x13, [x13, :lo12:m4_l1_guard_top]
    cbz x13, .Lfatal
    mov sp, x13
    ldr x1, [sp, #144]
    ldr x14, [sp, #160]
    mov x15, #1
    cbnz x14, .Lentered
    mov x15, #2                             /* taken before fn started: pending from earlier */
.Lentered:
    str x15, [x1]
    stp x10, x11, [x1, #16]
    stp x12, x9, [x1, #32]
    adr x10, .Lrestore
    msr R_ELR, x10
    mov x10, #SPSR_RET
    msr R_SPSR, x10
    eret
.Lfatal:                                    /* an exception with no guard frame: cannot happen */
    msr DAIFSet, #0xf
1:  b 1b
.org m4_l1_guard_vectors + 0x800    /* assertion: the code must fit in the table (.org may not move back) */
.section .note.GNU-stack,"",%progbits
