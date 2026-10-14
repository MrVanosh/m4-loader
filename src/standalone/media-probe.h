/* SPDX-License-Identifier: MIT */
#ifndef M4_MEDIA_PROBE_H
#define M4_MEDIA_PROBE_H
#include <stdint.h>
#include <stddef.h>

#define M4_MEDIA_PROBE_PROPERTY "m4,media-probe"
#define M4_MEDIA_PROBE_MAGIC UINT64_C(0x4d344d4544505231) /* "M4MEDPR1" */
#define M4_MEDIA_PROBE_VERSION 1
#define M4_MEDIA_HEADER_WORDS 8
#define M4_MEDIA_MAX_RECORDS 16
#define M4_MEDIA_MAX_SEGMENTS 8

/* Record ids (state: 0 value, 1 abort, 2 skipped (value = reason), 3 guard refused). */
enum m4_media_record_id {
    M4_MEDIA_REC_PMGR_AVD_SYS_PS = 1,
    M4_MEDIA_REC_PMGR_FAB0_SOC_PS = 2,
    M4_MEDIA_REC_AOP0_CPU_CONTROL = 3,
    M4_MEDIA_REC_AOP0_MBOX_A2I_CONTROL = 4,
    M4_MEDIA_REC_AOP0_MBOX_I2A_CONTROL = 5,
    M4_MEDIA_REC_AOP1_CPU_CONTROL = 6,
    M4_MEDIA_REC_AVD_CODE_0 = 7,
    M4_MEDIA_REC_AVD_MBOX_1098000_90 = 8,
    M4_MEDIA_REC_AVD_MBOX_10A0000_90 = 9,
    M4_MEDIA_REC_AVD_MBOX_10A8000_90 = 10,
    M4_MEDIA_REC_AVD_CTRL_0 = 11,
    M4_MEDIA_REC_AVD_CTRL_4114 = 12,
};

struct m4_media_record { uint64_t id, state, address, value, esr, far; };

/* Reads the probe registers, each under its own j616s_usb_call guard. Never
 * writes a register, never stops: an abort or refusal is only recorded. Call
 * once, from the loader, outside any cached window (SCTLR_EL2 == guard value). */
void m4_media_probe_run(void);
/* ADT-only gate evaluation (no register access). Call inside a cached loader window before
 * m4_media_probe_run(); the run evaluates it itself if this was not called. */
void m4_media_probe_gates(void);
/* Publication-time RAM edit of the Linux DT (nodes, reservations, /chosen record). */
int m4_media_fdt_write(void *tree, size_t bytes, void *scratch, size_t capacity);
#endif
