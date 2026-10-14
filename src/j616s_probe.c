/* SPDX-License-Identifier: GPL-2.0-only */
#include "../build/build_cfg.h"
#ifdef J616S_SCREEN_DIAGNOSTIC
#include "j616s_probe.h"
#include "j616s_screen.h"

_Static_assert(sizeof(struct j616s_probe_result) == 32, "probe assembly ABI");
static unsigned raw_count;
static u64 raw[2];
bool j616s_probe_bounds(u64 *lower, u64 *upper)
{
    if (raw_count != 2) return false;
    *lower = raw[0]; *upper = raw[1];
    return true;
}

void j616s_probe_unexpected(u64 esr, u64 elr, u64 spsr, u64 vector)
{
    j616s_screen_printf("PROBE FATAL vector=%lu ESR=0x%lx ELR=0x%lx SPSR=0x%lx\n",
                        vector, esr, elr, spsr);
    j616s_screen_puts("PROBE STOP: unexpected exception; no recovery or further reads");
    j616s_screen_flush();
    for (;;)
        __asm__ volatile("nop");
}

void j616s_probe_report(void)
{
    raw_count = 0;
    struct j616s_probe_result r;
    j616s_screen_puts("REGISTER PROBE: testing temporary EL2 exception handler");
    j616s_screen_flush();
    if (j616s_probe_once(J616S_PROBE_EL, &r) || r.trapped || r.value != 8) {
        j616s_screen_puts("PROBE REFUSED: requires EL2h and a successful CurrentEL control read");
        return;
    }
    if (j616s_probe_once(J616S_PROBE_BRK, &r) || r.trapped != 1 ||
        (r.esr >> 26) != 0x3c || (r.esr & 0x1ffffff) != 0x616) {
        j616s_screen_puts("PROBE REFUSED: BRK self-test failed; Apple registers not read");
        return;
    }
    if (j616s_probe_once(J616S_PROBE_UDF, &r) || r.trapped != 1 || (r.esr >> 26) != 0) {
        j616s_screen_puts("PROBE REFUSED: undefined-instruction self-test failed; Apple registers not read");
        return;
    }
    j616s_screen_puts("PROBE SELF-TEST PASS: control read, BRK and undefined instruction recovered");
    for (unsigned op = J616S_PROBE_LOWER; op <= J616S_PROBE_UPPER; op++) {
        const char *name = op == J616S_PROBE_LOWER ? "S3_0_C11_C0_0" : "S3_0_C11_C0_1";
        j616s_screen_printf("PROBE next=%s (pinned CTRR %s encoding)\n", name,
                            op == J616S_PROBE_LOWER ? "LOWER" : "UPPER");
        j616s_screen_flush();
        if (j616s_probe_once(op, &r)) {
            j616s_screen_puts("PROBE REFUSED: execution state changed; remaining reads skipped");
            return;
        }
        if (r.trapped)
            j616s_screen_printf("PROBE %s TRAPPED ESR=0x%lx ELR=0x%lx; no value\n", name, r.esr, r.elr);
        else {
            raw[op - J616S_PROBE_LOWER] = r.value;
            raw_count++;
            j616s_screen_printf("PROBE %s RAW=0x%lx\n", name, r.value);
        }
    }
    j616s_screen_puts("PROBE COMPLETE: raw observations only; no bound interpretation or protection writes");
    j616s_screen_puts("EL2 exception state restored after each probe; MMU/controller state unchanged");
}
#endif
