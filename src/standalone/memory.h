/* SPDX-License-Identifier: MIT */
#ifndef M4_STANDALONE_MEMORY_H
#define M4_STANDALONE_MEMORY_H
#include <stdint.h>
#include <stddef.h>
#define M4_MAX_RESERVED 512
#define M4_MAX_FREE 32
#define M4_PAGE 16384ULL
struct m4_range { uint64_t base, size; };
struct m4_memory {
    struct m4_range usable, dram;
    uint64_t top, ctrr_low, ctrr_high;
    struct m4_range reserved[M4_MAX_RESERVED], free[M4_MAX_FREE];
    unsigned reserved_count, free_count;
    uint64_t free_bytes;
};
int m4_range_valid(struct m4_range r);
int m4_contains(struct m4_range outer, struct m4_range inner);
int m4_overlaps(struct m4_range a, struct m4_range b);
int m4_reserve(struct m4_memory *m, uint64_t base, uint64_t size);
int m4_memory_finish(struct m4_memory *m);
#endif
