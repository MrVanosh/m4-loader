/* SPDX-License-Identifier: MIT */
#ifndef M4_FDT_RAM_H
#define M4_FDT_RAM_H
#include <stddef.h>
#include <stdint.h>

typedef uint64_t m4_fdt_u64 __attribute__((may_alias));
typedef uint32_t m4_fdt_u32 __attribute__((may_alias));

/* Only libfdt's admitted RAM extents; preserve overlap direction and exact tails. */
static inline void *m4_fdt_ram_move(void *dst, const void *src, size_t bytes)
{
    unsigned char *d=dst;
    const unsigned char *s=src;
    if (!bytes || d==s) return dst;
    uintptr_t different=(uintptr_t)d^(uintptr_t)s;
    unsigned width=!(different&7)?8:!(different&3)?4:1;
    if ((uintptr_t)d<(uintptr_t)s) {
        while (bytes && ((uintptr_t)d&(width-1))) { *d++=*s++; bytes--; }
        if (width==8)
            while (bytes>=8) { *(m4_fdt_u64 *)d=*(const m4_fdt_u64 *)s; d+=8; s+=8; bytes-=8; }
        else if (width==4)
            while (bytes>=4) { *(m4_fdt_u32 *)d=*(const m4_fdt_u32 *)s; d+=4; s+=4; bytes-=4; }
        while (bytes--) *d++=*s++;
    } else {
        d+=bytes; s+=bytes;
        while (bytes && ((uintptr_t)d&(width-1))) { *--d=*--s; bytes--; }
        if (width==8)
            while (bytes>=8) { d-=8; s-=8; bytes-=8; *(m4_fdt_u64 *)d=*(const m4_fdt_u64 *)s; }
        else if (width==4)
            while (bytes>=4) { d-=4; s-=4; bytes-=4; *(m4_fdt_u32 *)d=*(const m4_fdt_u32 *)s; }
        while (bytes--) *--d=*--s;
    }
    return dst;
}
#endif
