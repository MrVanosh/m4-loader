/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef J616S_PROXY_H
#define J616S_PROXY_H
#include "types.h"
#define J616S_UPLOAD_SIZE (32UL*1024*1024)
#define P_WORKBENCH_INFO 0x7000
#define P_WORKBENCH_RESET 0x7001
#define P_WORKBENCH_FAULT 0x7002
struct j616s_stream {
    void *opaque;
    ssize_t (*read)(void *,void *,size_t);
    ssize_t (*write)(void *,const void *,size_t);
    u64 (*milliseconds)(void *);
    u64 (*execute)(u64,const u64 *);
    u64 (*fault)(unsigned);
};
struct j616s_proxy_memory { u64 boot,boot_size,adt,adt_size,base,heap,heap_size; };
struct j616s_proxy_stats { u64 commands,reads,writes,calls,rejected; };
int j616s_proxy_run(const struct j616s_stream *io,const struct j616s_proxy_memory *m,
                   struct j616s_proxy_stats *stats);
/* Smoke-test programs. V15 also permits trusted code in allocated upload RAM.
 * Machine code executes at EL2: protocol bounds are NOT a code sandbox. */
static const u32 j616s_probe_arithmetic[]={0x8b010000,0xca020000,0x8b030000,0x8b040000,0xd65f03c0};
static const u32 j616s_probe_currentel[]={0xd5384240,0xd65f03c0};
#endif
