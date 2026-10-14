/* SPDX-License-Identifier: MIT */
#ifndef M4_STANDALONE_PUMP_H
#define M4_STANDALONE_PUMP_H
#include <stdint.h>
typedef uint64_t (*m4_runner)(uint64_t*,uint64_t,uint64_t,uint64_t,uint64_t);
typedef int (*m4_observer)(const uint64_t*,void*);
/* Observer 0 continues, positive stops a qualification interval, negative
 * reports failure. A fresh context must be zero except input sentinel. */
int m4_run_linux(uint64_t *ctx,m4_runner run,uint64_t kernel,uint64_t fdt,m4_observer observer,void *opaque);
#endif
