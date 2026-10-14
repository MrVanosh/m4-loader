/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef J616S_MMU_H
#define J616S_MMU_H
#include "types.h"

/* Only the loader image is mapped, at 16KiB page granularity. No allocator,
 * controller, firmware table, alias or external RAM is used by this mapper. */
#define J616S_PT_PAGE 0x4000UL
#define J616S_PT_COUNT 8
#define J616S_PA_LIMIT (1UL << 42)
struct j616s_pt {
    u64 pages[J616S_PT_COUNT][2048] __attribute__((aligned(J616S_PT_PAGE)));
    unsigned used;
};
struct j616s_mmu_result {
    u64 status, live_sctlr, scratch, esr, elr, far, par_code, par_guard;
};
#define J616S_TEST_WORDS 2048
#define J616S_UNCACHED_SEED 0x616511110000abcdUL
#define J616S_CACHED_SEED 0x616512120000abcdUL
/* All cache-maintenance addresses stay in this validated, allocated image. */
struct j616s_cache_config { u64 cached, image_start, image_end, line_size; };
bool j616s_pt_build(struct j616s_pt *pt, u64 base, u64 ro_end, u64 end, bool vhe);
u64 j616s_pt_lookup(const struct j616s_pt *pt, u64 va);
u64 j616s_mmu_tcr(bool vhe);
int j616s_mmu_once(u64 root, u64 tcr, volatile u64 *scratch,
                    struct j616s_mmu_result *result, const struct j616s_cache_config *config);
void j616s_mmu_report(u64 lower, u64 upper);
#endif
