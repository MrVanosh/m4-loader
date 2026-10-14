/* SPDX-License-Identifier: MIT */
/* Identity tables and window bookkeeping for the cached direct loader.
 * Permission encodings are exactly V12's natively accepted pair: RO+X for
 * code/rodata, RW+XN for data. Device mappings use the RW+XN encoding.
 */
#include "loader-cache.h"
#include <string.h>

#ifdef M4_CACHE_TEST
extern uint64_t m4_cache_test_read(const char *);
extern uint64_t m4_cache_test_counter(void);
#define READ(reg) m4_cache_test_read(#reg)
#define COUNTER() m4_cache_test_counter()
#else
#include "utils.h"
#include "boot-timing.h"
#define READ(reg) mrs(reg)
#define COUNTER() m4_boot_counter()
#endif

struct m4_cache_state m4_cache;
uint64_t m4_cache_live;

#define PAGE UINT64_C(0x4000)
#define BLOCK UINT64_C(0x2000000)
#define ADDR_MASK UINT64_C(0x0000ffffffffc000)
#define AF (UINT64_C(1) << 10)
#define ISH (UINT64_C(3) << 8)
#define RO (UINT64_C(1) << 7)
#define UXN (UINT64_C(1) << 54)
#define PXN (UINT64_C(1) << 53)
#define ATTR_NORMAL (UINT64_C(1) << 2)
#define DEVICE_BLOCK (AF | UXN | PXN | 1)
#define DEVICE_PAGE (AF | UXN | PXN | 3)
#define NORMAL_RX (ATTR_NORMAL | ISH | AF | RO | UXN | 3)
#define NORMAL_RW (ATTR_NORMAL | ISH | AF | UXN | PXN | 3)
#define TABLE 3
#define LIMIT ((uint64_t)M4_CACHE_L1_ENTRIES << 36)

static uint64_t (*pool)[2048];
static unsigned pool_count;

static uint64_t *table(void)
{
    if (m4_cache.tables_used >= pool_count) return NULL;
    return pool[m4_cache.tables_used++];
}

static int aligned(uint64_t base, uint64_t end)
{
    return base < end && !((base | end) & (PAGE - 1)) && end <= LIMIT;
}

static int overlaps(uint64_t base, uint64_t end, struct m4_range r)
{
    return r.size && base < r.base + r.size && r.base < end;
}

/* Returns the level-3 slot for a page, splitting its 32 MiB Device block. */
static uint64_t *slot(uint64_t *l1, uint64_t pa)
{
    uint64_t *l2 = (uint64_t *)(l1[pa >> 36] & ADDR_MASK);
    uint64_t *entry = &l2[(pa >> 25) & 2047];
    if ((*entry & 3) == 1) {
        uint64_t *l3 = table(), base = pa & ~(BLOCK - 1);
        if (!l3) return NULL;
        for (unsigned i = 0; i < 2048; i++) l3[i] = (base + i * PAGE) | DEVICE_PAGE;
        *entry = (uint64_t)l3 | TABLE;
    }
    return (uint64_t *)(*entry & ADDR_MASK) + ((pa >> 14) & 2047);
}

static int map(uint64_t *l1, uint64_t base, uint64_t end, uint64_t attributes)
{
    for (uint64_t pa = base; pa < end; pa += PAGE) {
        uint64_t *entry = slot(l1, pa);
        if (!entry) return M4_CACHE_NO_TABLES;
        *entry = pa | attributes;
    }
    return 0;
}

int m4_cache_build(const struct m4_cache_layout *l, uint64_t (*tables)[2048], unsigned table_count)
{
    uint64_t started = COUNTER();
    if (m4_cache.status != M4_CACHE_NOT_BUILT) return M4_CACHE_REFUSED_STATE;
    int rc = M4_CACHE_REFUSED_STATE;
    uint64_t mmfr = READ(ID_AA64MMFR0_EL1), ctr = READ(CTR_EL0);
    uint64_t line = UINT64_C(4) << ((ctr >> 16) & 15);
    /* Accepted inherited baseline only: EL2h, VHE+TGE, MMU/caches off, 42-bit
     * PA and 16 KiB granule support, translation registers still dormant. */
    if (READ(CurrentEL) != 8 || READ(SPSel) != 1 || READ(SCTLR_EL2) != M4_CACHE_SCTLR_OFF ||
        READ(HCR_EL2) != UINT64_C(0x30488000000) || (mmfr & 15) < 3 ||
        ((mmfr >> 20) & 15) != 1 || line < 16 || line > 2048 || READ(TTBR0_EL2) ||
        READ(TCR_EL2)) goto out;
    rc = M4_CACHE_REFUSED_LAYOUT;
    if (!l || !tables || ((uintptr_t)tables & (PAGE - 1)) || table_count < 2 + M4_CACHE_L1_ENTRIES ||
        table_count > M4_CACHE_MAX_TABLES || l->hole_count > M4_CACHE_MAX_HOLES ||
        !aligned(l->image_base, l->image_ro_end) || !aligned(l->image_ro_end, l->image_end) ||
        ((uintptr_t)tables < l->image_ro_end || (uintptr_t)(tables + table_count) > l->image_end))
        goto out;
    if (l->adt_base || l->adt_end) {
        if (!aligned(l->adt_base, l->adt_end) ||
            (l->adt_base < l->image_end && l->image_base < l->adt_end)) goto out;
    }
    for (unsigned i = 0; i < l->hole_count; i++) {
        struct m4_range h = l->holes[i];
        if (!aligned(h.base, h.base + h.size) || h.base < l->image_ro_end ||
            h.base + h.size > l->image_end) goto out;
    }
    if (l->pool.size && (!aligned(l->pool.base, l->pool.base + l->pool.size) ||
        l->pool.base < l->image_ro_end || l->pool.base + l->pool.size > l->image_end)) goto out;
    if (l->exec.size && (!aligned(l->exec.base, l->exec.base + l->exec.size) ||
        l->exec.base < l->image_ro_end || l->exec.base + l->exec.size > l->image_end)) goto out;
    rc = M4_CACHE_REFUSED_PROTECTED;
    if (overlaps(l->image_base, l->image_end, l->protected) ||
        overlaps(l->adt_base, l->adt_end, l->protected)) goto out;

    rc = M4_CACHE_NO_TABLES;
    pool = tables; pool_count = table_count; m4_cache.tables_used = 0;
    uint64_t *l0 = table(), *l1 = table();
    for (unsigned i = 0; i < 2048; i++) l0[i] = l1[i] = 0;
    l0[0] = (uint64_t)l1 | TABLE;
    for (unsigned i = 0; i < M4_CACHE_L1_ENTRIES; i++) {
        uint64_t *l2 = table(), base = (uint64_t)i << 36;
        for (unsigned j = 0; j < 2048; j++) l2[j] = (base + j * BLOCK) | DEVICE_BLOCK;
        l1[i] = (uint64_t)l2 | TABLE;
    }
    if ((rc = map(l1, l->image_base, l->image_ro_end, NORMAL_RX)) ||
        (rc = map(l1, l->image_ro_end, l->image_end, NORMAL_RW)) ||
        (l->adt_end && (rc = map(l1, l->adt_base, l->adt_end, NORMAL_RW)))) goto out;
    if (l->exec.size && (rc = map(l1, l->exec.base, l->exec.base + l->exec.size, NORMAL_RX))) goto out;
    m4_cache.normal_bytes = l->image_end - l->image_base + (l->adt_end - l->adt_base);
    for (unsigned i = 0; i < l->hole_count; i++) {
        if ((rc = map(l1, l->holes[i].base, l->holes[i].base + l->holes[i].size, DEVICE_PAGE))) goto out;
        m4_cache.normal_bytes -= l->holes[i].size;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    m4_cache.hw = (struct m4_cache_hw){
        .ttbr0 = (uint64_t)l0, .tcr = M4_CACHE_TCR, .mair = M4_CACHE_MAIR,
        .sctlr_off = M4_CACHE_SCTLR_OFF, .sctlr_on = M4_CACHE_SCTLR_ON,
        .saved_ttbr0 = READ(TTBR0_EL2), .saved_tcr = READ(TCR_EL2), .saved_mair = READ(MAIR_EL2),
        .line = line,
        /* Only RW pages can be dirty: code/rodata is mapped read-only. */
        .r0_base = l->image_ro_end, .r0_end = l->image_end,
        .r1_base = l->adt_base, .r1_end = l->adt_end, .invalidate = 1,
        /* start.S relocated pointers in rodata uncached: discard iBoot's lines too. */
        .f0_base = l->image_base, .f0_end = l->image_end,
        .e0_base = l->image_ro_end, .e0_end = l->pool.size ? l->pool.base : l->image_end,
        .e1_base = l->pool.size ? l->pool.base + l->pool.size : l->image_end, .e1_end = l->image_end};
    rc = M4_CACHE_READY;
out:
    m4_cache.status = rc;
    m4_cache.build_ticks = COUNTER() - started;
    return rc == M4_CACHE_READY ? 0 : rc;
}

void m4_cache_enter(void)
{
    if (m4_cache.status != M4_CACHE_READY || m4_cache.active) return;
    uint64_t started = COUNTER();
    /* Stale lines can only predate the first window: later windows begin after
     * a clean+invalidate with every access Device in between. */
    m4_cache.hw.invalidate = !m4_cache.invalidated;
    uint64_t sctlr = m4_cache_on(&m4_cache.hw);
    if (sctlr != M4_CACHE_SCTLR_ON) {
        if (sctlr & 1) m4_cache_off(&m4_cache.hw);
        m4_cache.status = M4_CACHE_ENABLE_FAILED;
        return;
    }
    m4_cache.active = 1;
    m4_cache_live = 1;
    m4_cache.invalidated = 1;
    m4_cache.enters++;
    m4_cache.enter_ticks += COUNTER() - started;
}

void m4_cache_leave(void)
{
    if (!m4_cache.active) return;
    uint64_t started = COUNTER();
    m4_cache_off(&m4_cache.hw);
    /* RAM is authoritative again; these writes are Device-nGnRnE. */
    m4_cache.active = 0;
    m4_cache_live = 0;
    m4_cache.leaves++;
    uint64_t spent = COUNTER() - started;
    m4_cache.leave_ticks += spent;
    if (spent > m4_cache.max_leave_ticks) m4_cache.max_leave_ticks = spent;
}

void m4_cache_scrub(void)
{
    if (m4_cache.status != M4_CACHE_READY || m4_cache.active || (READ(SCTLR_EL2) & 1)) return;
    m4_cache_clean_range(m4_cache.hw.r0_base, m4_cache.hw.r0_end, m4_cache.hw.line);
    m4_cache_clean_range(m4_cache.hw.r1_base, m4_cache.hw.r1_end, m4_cache.hw.line);
}

unsigned m4_cache_compare(const void *dst, const void *src, size_t bytes, size_t full_prefix)
{
    const volatile uint64_t *d = dst, *s = src;
    size_t words = bytes / 8, full = (full_prefix < bytes ? full_prefix : bytes) / 8;
    unsigned bad = 0;
    for (size_t i = 0; i < full; i++) bad += d[i] != s[i];
    /* One word per 4 KiB, walking through the lines of the page. */
    for (size_t i = full, n = 0; i < words; i += 512, n++) {
        size_t at = i + (n * 8) % 512;
        if (at < words) bad += d[at] != s[at];
    }
    if (words > full) bad += d[words - 1] != s[words - 1];
    const unsigned char *db = (const unsigned char *)dst + words * 8, *sb = (const unsigned char *)src + words * 8;
    for (size_t i = 0; i < bytes % 8; i++) bad += db[i] != sb[i];
    return bad;
}
