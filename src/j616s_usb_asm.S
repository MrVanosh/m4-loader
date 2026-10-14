/* SPDX-License-Identifier: GPL-2.0-only */
#include "../build/build_cfg.h"
#ifdef J616S_SCREEN_DIAGNOSTIC
/* Single-core, non-reentrant callback guard. Abort the entire callback on a
 * same-EL data abort or our explicit BRK; never retry an MMIO instruction.
 * Saves the outer SP because the interrupted C function can have any frame. */
.section .bss.j616s_usb_saved_sp,"aw",%nobits
.balign 8
j616s_usb_saved_sp: .skip 8
.section .text.j616s_usb_call,"ax"
.global j616s_usb_call
.type j616s_usb_call,%function
j616s_usb_call:
    mrs x9, CurrentEL
    cmp x9, #8
    b.ne .Lrefuse
    mrs x9, SPSel
    cbz x9, .Lrefuse
    mrs x9, SCTLR_EL2
    mov x10, #0x180
    movk x10, #0x3000, lsl #16
    cmp x9, x10
    b.ne .Lrefuse
    adrp x9, j616s_usb_saved_sp
    ldr x10, [x9, :lo12:j616s_usb_saved_sp]
    cbnz x10, .Lrefuse
    stp x19, x20, [sp, #-160]!
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    mrs x10, VBAR_EL2
    mrs x11, DAIF
    stp x10, x11, [sp, #96]
    msr DAIFSet, #15
    mrs x10, ELR_EL2
    mrs x11, SPSR_EL2
    stp x10, x11, [sp, #112]
    mrs x10, ESR_EL2
    mrs x11, FAR_EL2
    stp x10, x11, [sp, #128]
    str x1, [sp, #144]
    stp xzr, xzr, [x1]
    stp xzr, xzr, [x1, #16]
    str xzr, [x1, #32]
    mov x10, sp
    str x10, [x9, :lo12:j616s_usb_saved_sp]
    adrp x10, j616s_usb_vectors
    add x10, x10, :lo12:j616s_usb_vectors
    msr VBAR_EL2, x10
    isb
    blr x0
    ldr x1, [sp, #144]
    str x0, [x1, #8]
.Lrestore:
    ldp x10, x11, [sp, #128]
    msr ESR_EL2, x10
    msr FAR_EL2, x11
    ldp x10, x11, [sp, #112]
    msr ELR_EL2, x10
    msr SPSR_EL2, x11
    ldp x10, x11, [sp, #96]
    msr VBAR_EL2, x10
    isb
    adrp x9, j616s_usb_saved_sp
    str xzr, [x9, :lo12:j616s_usb_saved_sp]
    msr DAIF, x11
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    ldp x19, x20, [sp], #160
    mov w0, #0
    ret
.Lrefuse:
    mov w0, #-1
    ret
.size j616s_usb_call, .-j616s_usb_call
.Lsync:
    mrs x10, SPSR_EL2
    and x10, x10, #31
    cmp x10, #9
    b.ne .Lfatal
    mrs x10, ESR_EL2
    lsr x11, x10, #26
    cmp x11, #0x25
    b.eq .Labort
    cmp x11, #0x3c
    b.ne .Lfatal
    and x11, x10, #0xffff
    mov x12, #0x6163
    cmp x11, x12
    b.ne .Lfatal
.Labort:
    adrp x11, j616s_usb_saved_sp
    ldr x11, [x11, :lo12:j616s_usb_saved_sp]
    cbz x11, .Lfatal
    mov sp, x11
    ldr x1, [sp, #144]
    mov x11, #1
    str x11, [x1]
    mrs x11, ELR_EL2
    stp x10, x11, [x1, #16]
    mrs x11, FAR_EL2
    str x11, [x1, #32]
    adr x11, .Lrestore
    msr ELR_EL2, x11
    mov x11, #0x3c9
    msr SPSR_EL2, x11
    eret
.global j616s_usb_fatal
j616s_usb_fatal:
.Lfatal:
    msr DAIFSet, #15
    b .Lfatal
.section .text.j616s_usb_vectors,"ax"
.balign 2048
.global j616s_usb_vectors
j616s_usb_vectors:
.set vector,0
.rept 16
    .if vector == 4
        b .Lsync
    .else
        b .Lfatal
    .endif
    .balign 128
    .set vector,vector+1
.endr
.global j616s_usb_vectors_end
j616s_usb_vectors_end:
#endif
.section .note.GNU-stack,"",%progbits
