/* SPDX-License-Identifier: GPL-2.0-only */
#include "j616s_mmu.h"

static const unsigned shifts[] = {47, 36, 25, 14};
#define ADDR_MASK 0x0000ffffffffc000UL

u64 j616s_mmu_tcr(bool vhe)
{
    /* 48-bit VA, 42-bit PA, 16KiB granule, uncached table walks, inner shareable.
     * VHE: disable TTBR1 walks; do not modify TTBR1 or HCR. Non-VHE RES1 bits.
     * Only SCTLR.M will be set. Both caches remain disabled. */
    u64 common = 16 | (2UL << 14) | (3UL << 12);
    return vhe ? common | (3UL << 32) | (1UL << 30) | (16UL << 16) | (1UL << 23)
               : common | (3UL << 16) | (1UL << 31) | (1UL << 23);
}

bool j616s_pt_build(struct j616s_pt *pt, u64 base, u64 ro_end, u64 end, bool vhe)
{
    if (((u64)pt & (J616S_PT_PAGE - 1)) ||
        ((base | ro_end | end) & (J616S_PT_PAGE - 1)) ||
        !base || base >= ro_end || ro_end >= end || end > J616S_PA_LIMIT ||
        end - base > 0x1000000 || (u64)pt > J616S_PA_LIMIT - sizeof(*pt))
        return false;
    for (unsigned i = 0; i < J616S_PT_COUNT; i++)
        for (unsigned j = 0; j < 2048; j++) pt->pages[i][j] = 0;
    pt->used = 1;
    for (u64 addr = base; addr < end; addr += J616S_PT_PAGE) {
        u64 *table = pt->pages[0];
        for (unsigned level = 0; level < 3; level++) {
            unsigned ix = (addr >> shifts[level]) & 2047;
            if (!table[ix]) {
                if (pt->used == J616S_PT_COUNT) return false;
                table[ix] = (u64)pt->pages[pt->used++] | 3;
            }
            table = (u64 *)(table[ix] & ADDR_MASK);
        }
        /* AP[2]=RO for code/rodata, privileged-only. PXN is RES0 without VHE;
         * bit54 is XN without VHE, UXN with VHE. AF is already set. */
        u64 perm = addr < ro_end ? (1UL << 7) : (1UL << 54);
        if (vhe) perm |= (1UL << 54) | (addr < ro_end ? 0 : (1UL << 53));
        table[(addr >> 14) & 2047] = addr | 3 | (1UL << 10) | (3UL << 8) | perm;
    }
    return true;
}

u64 j616s_pt_lookup(const struct j616s_pt *pt, u64 va)
{
    if (va >> 48 || !pt->used || pt->used > J616S_PT_COUNT) return 0;
    const u64 *table = pt->pages[0];
    for (unsigned level = 0; level < 3; level++) {
        u64 d = table[(va >> shifts[level]) & 2047];
        if ((d & 3) != 3) return 0;
        u64 next = d & ADDR_MASK;
        if (next < (u64)pt->pages || next >= (u64)pt->pages[pt->used] ||
            (next - (u64)pt->pages) % J616S_PT_PAGE) return 0;
        table = (const u64 *)next;
    }
    return table[(va >> 14) & 2047];
}
