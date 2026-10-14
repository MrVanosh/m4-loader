/* SPDX-License-Identifier: MIT */
/* Cached execution windows for the direct loader (m1n1 mmu_init/mmu_shutdown
 * pattern). Loader-owned RAM and the ADT are Normal write-back; everything
 * else, including the peer pages, stays Device-nGnRnE as with the MMU off.
 * Every window ends with the MMU, D-cache and I-cache off and the written
 * range cleaned to PoC, so peers, opaque callbacks and Linux see RAM only.
 */
#ifndef M4_STANDALONE_LOADER_CACHE_H
#define M4_STANDALONE_LOADER_CACHE_H
#include <stdint.h>
#include <stddef.h>
#include "standalone/memory.h"

#define M4_CACHE_SCTLR_OFF UINT64_C(0x30000180)
#define M4_CACHE_SCTLR_ON  (M4_CACHE_SCTLR_OFF | (1 << 12) | (1 << 2) | 1)
/* attr0 Device-nGnRnE (the MMU-off data type), attr1 Normal WB RW-allocate. */
#define M4_CACHE_MAIR UINT64_C(0xff00)
/* V12's natively accepted VHE TCR: 48-bit VA, 42-bit PA, 16 KiB granule,
 * non-cacheable inner-shareable walks, TTBR1 walks disabled. */
#define M4_CACHE_TCR UINT64_C(0x34090b010)
#define M4_CACHE_L1_ENTRIES 17 /* identity [0, 1088 GiB): MMIO below 1 TiB, then DRAM */
#define M4_CACHE_MAX_TABLES 40
#define M4_CACHE_MAX_HOLES 4

enum m4_cache_status {
    M4_CACHE_NOT_BUILT = 0,
    M4_CACHE_READY = 1,
    M4_CACHE_REFUSED_STATE = -1,    /* not EL2h/VHE, or SCTLR/HCR differ from the accepted baseline */
    M4_CACHE_REFUSED_LAYOUT = -2,   /* unaligned, overlapping or out-of-range ranges */
    M4_CACHE_REFUSED_PROTECTED = -3, /* a Normal range touches the protected envelope */
    M4_CACHE_NO_TABLES = -4,
    M4_CACHE_ENABLE_FAILED = -5,    /* SCTLR read back differently; MMU left off */
};

/* Assembly ABI, see loader-cache-asm.S. Fields are loaded into registers
 * before any mode change. r0/r1: cleaned+invalidated on every leave (image
 * RW, ADT). f0: invalidated before the first window (whole image: relocated
 * rodata included). e0/e1: invalidated before every later window (RW minus
 * the allocator pool, where the CPU writes uncached between windows). */
struct m4_cache_hw {
    uint64_t ttbr0, tcr, mair, sctlr_off, sctlr_on;
    uint64_t saved_ttbr0, saved_tcr, saved_mair, line;
    uint64_t r0_base, r0_end, r1_base, r1_end, invalidate;
    uint64_t f0_base, f0_end, e0_base, e0_end, e1_base, e1_end;
};
_Static_assert(sizeof(struct m4_cache_hw) == 160, "cache assembly ABI");

struct m4_cache_layout {
    uint64_t image_base, image_ro_end, image_end; /* [base,ro_end) RX, [ro_end,end) RW */
    uint64_t adt_base, adt_end;                   /* 16 KiB-rounded RW, or 0/0 */
    struct m4_range holes[M4_CACHE_MAX_HOLES];    /* Device pages inside [ro_end,end) */
    unsigned hole_count;
    struct m4_range pool;                         /* allocator pool inside [ro_end,end), or 0 */
    struct m4_range protected;                    /* never Normal-mapped */
    struct m4_range exec;                         /* R39: read-only+executable sub-range of [ro_end,end), or 0 */
};

struct m4_cache_state {
    int status;
    unsigned active, invalidated, tables_used;
    unsigned enters, leaves;
    uint64_t build_ticks, enter_ticks, leave_ticks, max_leave_ticks;
    uint64_t normal_bytes;
    struct m4_cache_hw hw;
};
extern struct m4_cache_state m4_cache;
extern uint64_t m4_cache_live; /* read by the nested guard (j616s_exec_asm.S) */

uint64_t m4_cache_on(struct m4_cache_hw *);
uint64_t m4_cache_off(struct m4_cache_hw *);
void m4_cache_clean_range(uint64_t base, uint64_t end, uint64_t line);

/* Builds the identity tables in caller-owned 16 KiB-aligned RAM while the MMU
 * is off. A refusal leaves the loader on its uncached path. */
int m4_cache_build(const struct m4_cache_layout *, uint64_t (*tables)[2048], unsigned table_count);
void m4_cache_enter(void);
void m4_cache_leave(void);
/* MMU off only: clean+invalidate every range a window may have cached again,
 * before an uncached recopy (a mismatch means a clean was missed). */
void m4_cache_scrub(void);
/* Read through the MMU-off (Device) view after a window: mismatching sampled
 * words mean a missed clean; the caller then redoes the copy uncached. */
unsigned m4_cache_compare(const void *dst, const void *src, size_t bytes, size_t full_prefix);

static inline int m4_cache_sctlr_admitted(uint64_t sctlr)
{
    return !(sctlr & 0x1005) || (m4_cache.active && sctlr == M4_CACHE_SCTLR_ON);
}
#endif
