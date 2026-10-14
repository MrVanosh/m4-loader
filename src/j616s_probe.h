/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef J616S_PROBE_H
#define J616S_PROBE_H
#include "types.h"

enum j616s_probe_op { J616S_PROBE_EL, J616S_PROBE_BRK, J616S_PROBE_UDF,
                      J616S_PROBE_LOWER, J616S_PROBE_UPPER };
/* Assembly ABI: four consecutive 64-bit fields, offsets 0, 8, 16, 24. */
struct j616s_probe_result { u64 trapped, value, esr, elr; };
int j616s_probe_once(unsigned op, struct j616s_probe_result *result);
void j616s_probe_report(void);
bool j616s_probe_bounds(u64 *lower, u64 *upper);
void j616s_probe_unexpected(u64 esr, u64 elr, u64 spsr, u64 vector)
    __attribute__((noreturn));
#endif
