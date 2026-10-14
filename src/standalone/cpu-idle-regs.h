/* SPDX-License-Identifier: MIT */
/* R36 multicore lane, E0: read-only record of the three Apple idle/wake registers on the PRIMARY CPU.
 *
 *   CYC_OVRD s3_5_c15_c5_0   FIQ/IRQ wake mode, WFI mode, WFI retention
 *   ACC_CFG  s3_5_c15_c4_0   deep sleep / branch-predictor sleep
 *   ACC_OVRD s3_5_c15_c6_0   power-down overrides
 *
 * Each register is read once, in its own j616s_exec_call guard (undefined instruction, trapped sysreg and
 * aborts are recorded, never fatal). Nothing is written, no secondary CPU is touched, no SMP/reserved layout moves.
 *
 * /chosen/m4,cpu-idle-regs  (big-endian u64 words)
 *   [0] magic "M4IDLER1"  [1] version (1)  [2] total bytes  [3] flags: bit0 read attempted, bit1 no writes were made
 *   [4] MPIDR_EL1         [5] MIDR_EL1     [6] CurrentEL (2 = EL2)   [7] record count R (3)
 *   R x { id, state, value, esr, elr, far }
 *       id    0 CYC_OVRD, 1 ACC_CFG, 2 ACC_OVRD
 *       state 0 value read, 1 guard caught a fault (esr/elr/far valid), 2 guard refused to run, 3 not attempted
 */
#ifndef M4_STANDALONE_CPU_IDLE_REGS_H
#define M4_STANDALONE_CPU_IDLE_REGS_H
#include <stddef.h>
#include <stdint.h>

#define M4_CPU_IDLE_REGS_PROPERTY "m4,cpu-idle-regs"
#define M4_CPU_IDLE_REGS_MAGIC UINT64_C(0x4d3449444c455231)
#define M4_CPU_IDLE_REGS_VERSION 1
#define M4_CPU_IDLE_REGS_COUNT 3
#define M4_CPU_IDLE_REGS_WORDS (8 + 6 * M4_CPU_IDLE_REGS_COUNT)

/* Call once on the primary CPU at the guard-baseline SCTLR, before the SMP start. */
void m4_cpu_idle_regs_read(void);
/* Atomic /chosen edit; returns 0 on success, 1 when nothing was read, negative on refusal. */
int m4_cpu_idle_regs_publish(void *tree, size_t capacity);
#endif
