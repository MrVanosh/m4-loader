/* SPDX-License-Identifier: MIT */
#ifndef M4_STANDALONE_RAM_H
#define M4_STANDALONE_RAM_H
#include "utils.h"

/* Only for the loader's admitted, nonoverlapping RAM payloads; never MMIO. */
static inline void m4_ram_copy(void *dst, const void *src, size_t bytes)
{
    size_t bulk = ((u64)dst | (u64)src) & 7 ? 0 : bytes & ~7UL;
    memcpy64(dst, (void *)src, bulk);
    memcpy8((u8 *)dst + bulk, (void *)((const u8 *)src + bulk), bytes - bulk);
}

static inline void m4_ram_zero(void *dst, size_t bytes)
{
    size_t bulk = (u64)dst & 7 ? 0 : bytes & ~7UL;
    memset64(dst, 0, bulk);
    memset8((u8 *)dst + bulk, 0, bytes - bulk);
}
#endif
