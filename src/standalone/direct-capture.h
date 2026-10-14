/* SPDX-License-Identifier: MIT */
#ifndef M4_DIRECT_DESKTOP_CAPTURE_H
#define M4_DIRECT_DESKTOP_CAPTURE_H
#include "standalone/fdt.h"
#include "direct-smp.h"
#define M4_DIRECT_CAPTURE_MAGIC UINT64_C(0x4444495245435431)
#define M4_DIRECT_CAPTURE_VERSION 2
#define M4_DIRECT_CAPTURE_ENTERING 1
#define M4_DIRECT_DESKTOP_CONTRACT "j616s-direct-indexed-desktop-v2"
/* Native loader / GPU consumer ABI. Every field is one little-endian u64.
 * Loader identity is joined externally after the final paired ELF/raw build.
 */
struct m4_direct_capture_descriptor {
    uint64_t magic, version, bytes, phase;
    uint64_t loader_base, loader_end, arena_base, arena_size;
    uint64_t kernel, kernel_memory, initrd, initrd_bytes;
    uint64_t linux_dt, handoff_addr, adt, adt_bytes;
    uint64_t bootargs_addr, release_addr, release_page_base, release_page_bytes;
    uint64_t primary_mpidr, secondary_mpidr, framebuffer, framebuffer_bytes;
    uint64_t peer_count, peer_table, peer_entry_bytes;
};
struct m4_direct_capture_page {
    struct m4_direct_capture_descriptor descriptor;
    uint64_t pad_to_handoff[5];
    struct m4_handoff handoff;
    uint8_t reserved[M4_SMP_PAGE - 256 - sizeof(struct m4_handoff)];
};
_Static_assert(sizeof(struct m4_direct_capture_descriptor) == 216, "indexed direct capture ABI");
_Static_assert(sizeof(struct m4_handoff) == 96, "retained handoff ABI");
_Static_assert(offsetof(struct m4_direct_capture_page, handoff) == 256, "handoff ELF alias");
_Static_assert(sizeof(struct m4_direct_capture_page) == M4_SMP_PAGE, "capture page");
extern struct m4_direct_capture_page m4_direct_capture;
int m4_direct_desktop_fdt(void *, size_t);
int m4_smp_cache_pair_current_fdt(void *, size_t);
int m4_direct_desktop_ownership(const struct m4_memory *, struct m4_range,
                              struct m4_range);
#endif
