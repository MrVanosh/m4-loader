/* SPDX-License-Identifier: MIT */
/* R36 multicore lane, E0: see cpu-idle-regs.h. Read-only, primary CPU only, every read guarded. */
#pragma GCC optimize("Os")
#include "cpu-idle-regs.h"
#include "cpu_regs.h"
#include "utils.h"
#include "j616s_usb.h"
#include "libfdt.h"
#include <string.h>

struct idle_record { uint64_t state, value, esr, elr, far; };
static struct idle_record records[M4_CPU_IDLE_REGS_COUNT];
static uint64_t read_value, mpidr, midr, current_el;
static unsigned attempted;

static int read_cyc_ovrd(void) { read_value = mrs(SYS_IMP_APL_CYC_OVRD); return 0; }
static int read_acc_cfg(void) { read_value = mrs(SYS_IMP_APL_ACC_CFG); return 0; }
static int read_acc_ovrd(void) { read_value = mrs(SYS_IMP_APL_ACC_OVRD); return 0; }

void m4_cpu_idle_regs_read(void)
{
#ifndef M4_NO_CPU_IDLE_READ
    static int (*const readers[M4_CPU_IDLE_REGS_COUNT])(void) = {read_cyc_ovrd, read_acc_cfg, read_acc_ovrd};
    if (attempted) return;
    attempted = 1;
    mpidr = mrs(MPIDR_EL1);
    midr = mrs(MIDR_EL1);
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(current_el));
    current_el >>= 2;
    for (unsigned i = 0; i < M4_CPU_IDLE_REGS_COUNT; i++) {
        struct j616s_call_result result = {0};
        read_value = 0;
        int rc = j616s_exec_call(readers[i], &result);
        if (rc) {
            records[i].state = 2;
        } else if (result.status) {
            records[i].state = 1;
            records[i].esr = result.esr;
            records[i].elr = result.elr;
            records[i].far = result.far;
        } else {
            records[i].value = read_value;
        }
    }
#endif
}

int m4_cpu_idle_regs_publish(void *tree, size_t capacity)
{
    uint64_t wire[M4_CPU_IDLE_REGS_WORDS];
    if (!attempted) return 1;
    wire[0] = cpu_to_fdt64(M4_CPU_IDLE_REGS_MAGIC);
    wire[1] = cpu_to_fdt64(M4_CPU_IDLE_REGS_VERSION);
    wire[2] = cpu_to_fdt64(sizeof(wire));
    wire[3] = cpu_to_fdt64(3);
    wire[4] = cpu_to_fdt64(mpidr);
    wire[5] = cpu_to_fdt64(midr);
    wire[6] = cpu_to_fdt64(current_el);
    wire[7] = cpu_to_fdt64(M4_CPU_IDLE_REGS_COUNT);
    for (unsigned i = 0; i < M4_CPU_IDLE_REGS_COUNT; i++) {
        uint64_t *w = wire + 8 + 6 * i;
        w[0] = cpu_to_fdt64(i);
        w[1] = cpu_to_fdt64(records[i].state);
        w[2] = cpu_to_fdt64(records[i].value);
        w[3] = cpu_to_fdt64(records[i].esr);
        w[4] = cpu_to_fdt64(records[i].elr);
        w[5] = cpu_to_fdt64(records[i].far);
    }
    if (!tree || fdt_check_full(tree, capacity)) return -1;
    int chosen = fdt_path_offset(tree, "/chosen");
    if (chosen < 0 || fdt_getprop(tree, chosen, M4_CPU_IDLE_REGS_PROPERTY, NULL)) return -2;
    int ret = fdt_setprop(tree, chosen, M4_CPU_IDLE_REGS_PROPERTY, wire, sizeof(wire));
    if (ret || (ret = fdt_pack(tree)) || fdt_check_full(tree, capacity)) return ret ? ret : -3;
    return 0;
}
