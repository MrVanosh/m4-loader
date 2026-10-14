/* SPDX-License-Identifier: MIT */
#ifndef M4_DISPLAY_HANDOFF_H
#define M4_DISPLAY_HANDOFF_H
#include "direct-capture.h"
#include "dart-tables.h"
#include "dart-preserve.h"

#define M4_DISPLAY_HANDOFF_MAGIC UINT64_C(0x4d34444830303031)
#define M4_DISPLAY_HANDOFF_WORDS 1280u
#define M4_DISPLAY_HANDOFF_HEADER 20u
#define M4_DISPLAY_HANDOFF_STREAM 24u

/* RAM serialization only. The caller supplies accepted copied table results.
 * Failed/incomplete captures emit a header with zero streams/segments.
 * A distinct scratch tree makes all failed FDT edits leave the input intact. */
struct m4_display_handoff {
    struct m4_dt_range dram;
    uint64_t framebuffer, framebuffer_bytes, frequency;
    int64_t status;
    uint64_t metadata_completed, metadata_phase, active, esr, far;
    uint64_t capture_phase, active_address, elapsed;
    int64_t capture_error;
    struct m4_dt_input input[3];
    struct m4_dt_result result[3];
    struct m4_dp_plan plan[3];
    struct m4_dt_segment segment[M4_DT_SEGMENTS];
    unsigned completed;
};
int m4_display_handoff_write(void *, size_t, void *, size_t,
                            const struct m4_display_handoff *);
#ifdef M4_DCP_STAGE
/* DCP test builds only. Same acceptance as the serialized handoff: stable,
 * locked, in-DRAM capture. */
int m4_display_handoff_accepted(const struct m4_display_handoff *);
/* Internal-panel DCP nodes from an accepted capture. Returns 0 when emitted,
 * >0 when refused (reason in /chosen/m4,dcp-fdt-status), <0 when the tree
 * could not be edited. */
int m4_dcp_fdt_write(void *, size_t, void *, size_t, const struct m4_display_handoff *);
#endif
int m4_display_handoff_plan(const struct m4_dt_input *,const struct m4_dt_result *,
                           const struct m4_dt_workspace *,const struct m4_dt_range *,
                           unsigned,struct m4_dt_range,struct m4_dp_plan *);

/* Call through one new, sequential j616s_usb_call after the normal direct
 * producer's guard returns. Never nest abort guards or retry a faulting load.
 * Only known read-only registers/current declared table pools are read.
 * No MMIO load has a guaranteed timeout. CPU release/handoff stay untouched. */
int m4_display_handoff_capture(const struct m4_memory *, struct m4_range loader,
                               struct m4_range arena, uint64_t framebuffer,
                               uint64_t framebuffer_bytes);
struct j616s_call_result;
int m4_display_handoff_publish(void *, size_t, void *scratch, size_t scratch_capacity,
                               int guard, const struct j616s_call_result *);
#endif
