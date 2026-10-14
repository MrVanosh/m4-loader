/* SPDX-License-Identifier: MIT */
#ifndef M4_DIRECT_SMP_H
#define M4_DIRECT_SMP_H
#define M4_SMP_MAGIC 0x44534d50314c4142
#define M4_SMP_PREPARED 1
#define M4_SMP_RESET 2
#define M4_SMP_INIT 3
#define M4_SMP_READY 4
#define M4_SMP_RELEASED 5
#define M4_SMP_FAILED 6
#define M4_SMP_CAPTURED 7
#define M4_SMP_PAGE 16384
#define M4_SMP_STACK 65536
#define M4_SMP_MAX_PEERS 13
#define M4_SMP_NATIVE_PEERS 13
#define M4_SMP_P_PEERS 9
#define M4_SMP_E_PEERS 4
#define M4_SMP_EXTRA_REGS 16
#ifndef __ASSEMBLER__
#include "standalone/memory.h"
#include <stddef.h>
enum m4_smp_cold {
    S_SCTLR, S_HCR, S_GSCTLR, S_MIDR, S_DAIF, S_SPSEL,
    S_TCR, S_TTBR0, S_MAIR, S_GTCR, S_GTTBR0, S_GTTBR1, S_GMAIR,
    S_CNTHCTL, S_CNTVOFF, S_CPTR, S_CTR, S_FRQ, S_MMFR0, S_PFR0, S_PFR1, S_REGS
};
/* First 24 words are an assembly ABI. Linux writes only release, in LE.
 * The full page and stack remain owned/reserved after every attempted start.
 */
struct m4_direct_smp_state {
    uint64_t magic;
    volatile uint64_t phase, error;
    uint64_t stack_base, stack_top, affinity, kernel_base, kernel_size;
    volatile uint64_t release;
    uint64_t actual_mpidr, sctlr_before, hcr_before, sctlr_after, hcr_after;
    uint64_t esr, elr, far, fault_sp;
    volatile uint64_t active;
    uint64_t frequency, primary_ctr, secondary_ctr, ready_counter, attempted;
    uint64_t primary[S_REGS], secondary[S_REGS], stable[S_REGS];
    uint64_t reset[S_REGS], reset_el;
    volatile uint64_t park_progress, park_daif, park_mpidr, park_el, park_pc;
    uint64_t extra_reset[M4_SMP_EXTRA_REGS],extra_stable[M4_SMP_EXTRA_REGS];
    uint64_t extra_reset_complete,extra_stable_complete;
    uint64_t extra_primary[M4_SMP_EXTRA_REGS],extra_primary_complete;
    uint64_t reserved[M4_SMP_PAGE / 8 - 30 - 4 * S_REGS - 3*M4_SMP_EXTRA_REGS - 3];
};
_Static_assert(sizeof(struct m4_direct_smp_state) == M4_SMP_PAGE, "owned record page");
_Static_assert(offsetof(struct m4_direct_smp_state, release) == 64, "release ABI");
_Static_assert(offsetof(struct m4_direct_smp_state, esr) == 112, "fault ABI");
_Static_assert(offsetof(struct m4_direct_smp_state, reset) == 696, "reset bank ABI");
_Static_assert(offsetof(struct m4_direct_smp_state, reset_el) == 864, "reset EL ABI");
_Static_assert(offsetof(struct m4_direct_smp_state, park_progress) == 872, "failed park ABI");
_Static_assert(offsetof(struct m4_direct_smp_state, extra_reset) == 912, "E extra-bank ABI");
_Static_assert(offsetof(struct m4_direct_smp_state, extra_primary) == 1184, "primary extra-bank ABI");

/* Identity is supplied explicitly; raw ADT reg is never converted into MPIDR.
 * Thirteen peers have independent current-board entry profiles and storage. */
struct m4_direct_peer_plan { uint64_t cpu_id, adt_reg, affinity; };
struct m4_direct_peer_record {
    uint64_t cpu_id, adt_reg, affinity, state_base, state_bytes;
    uint64_t stack_base, stack_bytes, release_addr;
};
_Static_assert(sizeof(struct m4_direct_peer_record) == 64, "indexed peer wire ABI");
extern struct m4_direct_smp_state m4_direct_smp_states[M4_SMP_MAX_PEERS];
extern struct m4_direct_smp_state m4_direct_smp_state; /* exact first-record alias */
extern struct m4_direct_peer_record m4_direct_smp_peers[M4_SMP_MAX_PEERS];
extern uint64_t m4_direct_smp_peer_count;
/* Every retained record belongs to one guarded Linux spin-table endpoint. */
extern uint64_t m4_direct_smp_record_count;
extern const uint64_t m4_direct_indexed_native_gate;
int m4_direct_indexed_admitted(void);
/* RAM ownership only; it does not initialize, reset, capture or release a CPU. */
int m4_direct_smp_prepare_owned(const struct m4_memory *, struct m4_range,
    struct m4_range, struct m4_range, const struct m4_direct_peer_plan *, unsigned);
const struct m4_direct_peer_record *m4_direct_smp_peer_records(unsigned *);
int m4_direct_smp_launched(void);
int m4_direct_smp_diagnostics_safe(void);
void m4_direct_smp_failed_park(void) __attribute__((noreturn));
extern char m4_direct_smp_failed_loop[];

/* prepare validates ownership/metadata and applies guarded primary init_cpu;
 * start is the sole PMGR launch. Neither allocation is reclaimed on failure. */
int m4_direct_smp_kernel_memory(uint64_t size);
int m4_direct_smp_prepare(const struct m4_memory *, struct m4_range loader,
                         struct m4_range arena, struct m4_range protected);
int m4_direct_smp_start(void);
void m4_direct_smp_report(void);
/* Fixed current prepared allocation; no caller identity/address override. */
const struct m4_range *m4_direct_smp_regions(unsigned *count, uint64_t *release);
/* States, stacks and peer table pages (fixed storage); valid before prepare. */
unsigned m4_direct_smp_owned_pages(struct m4_range *out, unsigned capacity);
void m4_direct_smp_reset(void);
void m4_direct_smp_secondary(struct m4_direct_smp_state *) __attribute__((noreturn));
void m4_direct_smp_linux_wait(struct m4_direct_smp_state *) __attribute__((noreturn));
#endif
#endif
