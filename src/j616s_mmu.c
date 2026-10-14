/* SPDX-License-Identifier: GPL-2.0-only */
#include "../build/build_cfg.h"
#ifdef J616S_SCREEN_DIAGNOSTIC
#include "j616s_mmu.h"
#include "j616s_screen.h"
#include "j616s_diag.h"
#include "xnuboot.h"

extern char _base[], _rodata_end[], _end[];
static struct j616s_pt tables;
static volatile u64 scratch[J616S_TEST_WORDS] __attribute__((aligned(0x4000)));
static struct j616s_mmu_result result;
_Static_assert(sizeof(result) == 64, "MMU assembly ABI");
_Static_assert(sizeof(struct j616s_cache_config) == 32, "cache assembly ABI");
#define READ(reg) ({ u64 v; __asm__ volatile("mrs %0, " #reg : "=r"(v)); v; })

struct saved_state { u64 values[12]; };
static struct saved_state state(void)
{
    return (struct saved_state){{READ(SCTLR_EL2), READ(TCR_EL2), READ(TTBR0_EL2),
        READ(MAIR_EL2), READ(VBAR_EL2), READ(DAIF), READ(ELR_EL2), READ(SPSR_EL2),
        READ(ESR_EL2), READ(FAR_EL2), READ(PAR_EL1), READ(HCR_EL2)}};
}

static bool run_phase(bool cached, u64 line, bool vhe, u64 base, u64 end)
{
    const char *label = cached ? "CACHE" : "MMU";
    struct j616s_cache_config config = {cached, base, end, line};
    u64 seed = cached ? J616S_CACHED_SEED : J616S_UNCACHED_SEED;
    /* A different seed for each phase makes loss of cached writes visible. */
    j616s_screen_printf("%s NEXT: %s; 16KiB patterns, translation/fault checks, restore\n",
                        label, cached ? "enable MMU + I/D caches" : "uncached reference");
    j616s_screen_flush();
    struct saved_state before = state();
    int rc = j616s_mmu_once((u64)tables.pages[0], j616s_mmu_tcr(vhe), scratch, &result, &config);
    struct saved_state after = state();
    unsigned changed = 0, bad_words = 0;
    for (unsigned i = 0; i < 12; i++)
        if (before.values[i] != after.values[i]) changed |= 1U << i;
    if (!rc && !result.status)
        for (unsigned i = 0; i < J616S_TEST_WORDS; i++)
            if (scratch[i] != (seed ^ i)) bad_words++;
    j616s_screen_printf("%s result: rc=%d status=0x%lx live-SCTLR=0x%lx restored-diff=0x%x RAM-mismatches=%u\n",
                        label, rc, result.status, result.live_sctlr, changed, bad_words);
    j616s_screen_printf("%s seed=0x%lx PAR-code=0x%lx PAR-unmapped=0x%lx\n",
                        label, result.scratch, result.par_code, result.par_guard);
    j616s_screen_printf("%s fault: ESR=0x%lx ELR=0x%lx FAR=0x%lx\n",
                        label, result.esr, result.elr, result.far);
    bool pass = !rc && !result.status && !changed && !bad_words;
    j616s_screen_printf("%s TEST %s: %s\n", label, pass ? "PASS" : "FAILED/REFUSED",
                        pass ? "patterns retained in RAM; expected fault; original registers restored"
                             : "preserve this screen; no further startup");
    return pass;
}

void j616s_mmu_report(u64 lower, u64 upper)
{
    u64 hcr = READ(HCR_EL2), mmfr = READ(ID_AA64MMFR0_EL1), sctlr = READ(SCTLR_EL2);
    bool vhe = (hcr >> 34) & 1;
    u64 base = (u64)_base, end = (u64)_end, ro_end = (u64)_rodata_end;
    j616s_screen_printf("MMU TEST: HCR=0x%lx MMFR0=0x%lx SCTLR=0x%lx VHE=%u\n",
                        hcr, mmfr, sctlr, vhe);
    unsigned pa = mmfr & 15, granule = (mmfr >> 20) & 15;
    if (READ(CurrentEL) != 8 || READ(SPSel) != 1 || sctlr != 0x30000180 ||
        pa < 3 || pa > 6 || (granule != 1 && granule != 2)) {
        j616s_screen_puts("MMU REFUSED: requires observed EL2h/SCTLR baseline, 42-bit PA and 16KiB support");
        return;
    }
    /* Exclude the entire 16KiB envelope around the raw bounds. This does not
     * establish inclusivity or protection semantics on T6040. */
    if (!lower || lower > upper || (lower & 0xfff) || (upper & 0xfff) ||
        upper >= J616S_PA_LIMIT ||
        !j616s_range_contains(cur_boot_args.phys_base, cur_boot_args.mem_size,
                              lower, upper - lower + 0x1000)) {
        j616s_screen_puts("MMU REFUSED: unexpected raw register bounds");
        return;
    }
    u64 lo = lower & ~0x3fffUL, hi = (upper + 0x4000) & ~0x3fffUL;
    if (j616s_range_overlaps(base, end - base, lo, hi - lo) ||
        !j616s_range_contains(ro_end, end - ro_end, (u64)&tables, sizeof(tables)) ||
        !j616s_range_contains(ro_end, end - ro_end, (u64)&scratch, sizeof(scratch)) ||
        !j616s_range_contains(ro_end, end - ro_end, (u64)&result, sizeof(result)) ||
        !j616s_pt_build(&tables, base, ro_end, end, vhe)) {
        j616s_screen_puts("MMU REFUSED: image/pool overlap, extent or page-table capacity");
        return;
    }
    if (j616s_pt_lookup(&tables, 0) || j616s_pt_lookup(&tables, lo) ||
        j616s_pt_lookup(&tables, hi - 1) || j616s_pt_lookup(&tables, base - 1) ||
        j616s_pt_lookup(&tables, end)) {
        j616s_screen_puts("MMU REFUSED: excluded addresses unexpectedly mapped");
        return;
    }
    j616s_screen_printf("MMU map: image only [0x%lx,0x%lx); RO code to 0x%lx; %u tables\n",
                        base, end, ro_end, tables.used);
    j616s_screen_printf("MMU excluded raw-bounds envelope [0x%lx,0x%lx); all other memory unmapped\n", lo, hi);
    u64 ctr = READ(CTR_EL0);
    u64 line = 4UL << ((ctr >> 16) & 15);
    j616s_screen_printf("CACHE capability: CTR=0x%lx D-line=%lu; maintenance limited to loader image\n", ctr, line);
    if (line < 16 || line > 2048) {
        j616s_screen_puts("CACHE REFUSED: unsupported cache-line size");
        return;
    }
    if (!run_phase(false, line, vhe, base, end)) return;
    if (!run_phase(true, line, vhe, base, end)) return;
    j616s_screen_puts("CACHE COMPLETE: MMU and caches off again; only loader RAM used; no controller/protection writes");
}
#endif
