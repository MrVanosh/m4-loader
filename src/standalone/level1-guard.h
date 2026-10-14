/* SPDX-License-Identifier: MIT */
/* R41 Level 1 v2: the abort guard (level1-guard.S). See that file for the contract. */
#ifndef M4_STANDALONE_LEVEL1_GUARD_H
#define M4_STANDALONE_LEVEL1_GUARD_H
#include <stdint.h>

struct m4_l1_guard {
    uint64_t status;   /* 0 ran, 1 aborted inside fn, 2 an SError pending at entry was taken, 3 refused */
    uint64_t value;    /* fn's return value when status == 0 */
    uint64_t esr, elr, far;
    uint64_t vector;   /* current-EL slot 0..7 (4 = SPx sync, 7 = SPx SError); 0xff none */
    uint64_t isr;      /* ISR_EL1 at entry */
    uint64_t daif;     /* DAIF at entry */
};
/* 0 = the guard operated (read g->status); -1 = refused (g->status = 3, fn not run) */
int m4_l1_guard_call(int (*fn)(void *), void *arg, struct m4_l1_guard *g);
#endif
