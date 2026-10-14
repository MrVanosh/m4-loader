/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef J616S_DIAG_H
#define J616S_DIAG_H

#include <stdbool.h>
#include <stdint.h>

/* Half-open physical intervals. No rounding: malformed input stays invalid. */
static inline bool j616s_range_valid(uint64_t start, uint64_t size)
{
    return size && size <= UINT64_MAX - start;
}

static inline bool j616s_range_aligned(uint64_t start, uint64_t size)
{
    return j616s_range_valid(start, size) && !((start | size) & 0x3fff);
}

static inline bool j616s_range_contains(uint64_t base, uint64_t size, uint64_t start,
                                        uint64_t length)
{
    return j616s_range_valid(base, size) && j616s_range_valid(start, length) && start >= base &&
           length <= size && start - base <= size - length;
}

static inline bool j616s_range_overlaps(uint64_t a, uint64_t asize, uint64_t b, uint64_t bsize)
{
    return j616s_range_valid(a, asize) && j616s_range_valid(b, bsize) && a < b + bsize &&
           b < a + asize;
}

void j616s_diag_run(void) __attribute__((noreturn));

#endif
