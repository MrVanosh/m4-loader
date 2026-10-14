/* SPDX-License-Identifier: MIT */
/* Arena placement for the 256 KiB DT window and the inert m4,loader-speed-v1
 * record. Nothing here admits a device, a CPU or a payload. */
#include "loader-speed.h"
#include "loader-cache.h"
#include "libfdt.h"
#include <string.h>
#ifdef M4_CACHE_TEST
extern uint64_t m4_cache_test_counter(void);
extern void *memalign(size_t, size_t);
#define COUNTER() m4_cache_test_counter()
#else
#include "boot-timing.h"
#include "malloc.h"
#define COUNTER() m4_boot_counter()
#endif

struct m4_loader_speed m4_speed;
_Static_assert(M4_LOADER_SPEED_WORDS * 8 == 256, "bounded speed record");
_Static_assert(M4_ARENA_KERNEL_OFFSET == 0x10000 + M4_FDT_WINDOW, "DT window ends at the kernel");

/* The GPU capture contract fixes kernel = (arena+0x20000+2MiB-1)&~(2MiB-1),
 * DT = arena+0x10000 and initrd = arena+16MiB. Choosing the arena modulo 2 MiB
 * inside a 2 MiB larger block makes that kernel address arena+0x50000. */
void *m4_arena_place(size_t bytes)
{
    uint64_t block = (uint64_t)memalign(16384, bytes + 0x200000);
    if (!block) return NULL;
    uint64_t arena = block + ((0 - (block + M4_ARENA_KERNEL_OFFSET)) & 0x1fffff);
    if (((arena + 0x20000 + 0x1fffff) & ~UINT64_C(0x1fffff)) != arena + M4_ARENA_KERNEL_OFFSET ||
        arena & 16383 || arena + bytes > block + bytes + 0x200000) return NULL;
    m4_speed.kernel_offset = M4_ARENA_KERNEL_OFFSET;
    return (void *)arena;
}

static uint64_t word(unsigned index)
{
    const struct m4_cache_state *c = &m4_cache;
    switch (index) {
    case 0: return M4_LOADER_SPEED_MAGIC;
    case 1: return 1;
    case 2: return M4_LOADER_SPEED_WORDS * 8;
    case 4: return (uint64_t)(int64_t)c->status;
    case 5: return c->enters;
    case 6: return c->leaves;
    case 7: return c->build_ticks;
    case 8: return c->enter_ticks;
    case 9: return c->leave_ticks;
    case 10: return c->max_leave_ticks;
    case 11: return c->normal_bytes;
    case 12: return m4_speed.table_bytes;
    case 13: return m4_speed.payload_check;
    case 14: return m4_speed.payload_mismatches;
    case 15: return m4_speed.dt_check;
    case 16: return m4_speed.dt_mismatches;
    case 17: return m4_speed.rng_seed_bytes;
    case 18: return m4_speed.late_capacity;
    case 19: return m4_speed.dt_bytes;
    case 20: return m4_speed.kernel_offset;
    case 21: return m4_speed.smp_mode;
    case 22: return m4_speed.smp_first_launch;
    case 23: return m4_speed.smp_last_running;
    case 24: return m4_speed.smp_all_ready;
    case 25: return m4_speed.bringup_export;
    case 26: return m4_speed.bringup_ticks;
    default: return 0;
    }
}

int m4_loader_speed_publish(void *tree, size_t capacity, uint64_t frequency)
{
    uint64_t wire[M4_LOADER_SPEED_WORDS];
    for (unsigned i = 0; i < M4_LOADER_SPEED_WORDS; i++) wire[i] = cpu_to_fdt64(word(i));
    wire[3] = cpu_to_fdt64(frequency);
    wire[27] = cpu_to_fdt64(COUNTER());
    if (!tree || fdt_check_full(tree, capacity)) return -1;
    int chosen = fdt_path_offset(tree, "/chosen");
    if (chosen < 0 || fdt_getprop(tree, chosen, M4_LOADER_SPEED_PROPERTY, NULL)) return -2;
    int ret = fdt_setprop(tree, chosen, M4_LOADER_SPEED_PROPERTY, wire, sizeof(wire));
    if (ret || (ret = fdt_pack(tree)) || fdt_check_full(tree, capacity)) return ret ? ret : -3;
    return 0;
}

/* Runs with the MMU off on the copy Linux will read: fields that change in the
 * last window and the final check are patched in place, size unchanged. */
void m4_loader_speed_finish(void *dt)
{
    int chosen = fdt_path_offset(dt, "/chosen"), len = 0;
    uint64_t *wire = chosen < 0 ? NULL : fdt_getprop_w(dt, chosen, M4_LOADER_SPEED_PROPERTY, &len);
    if (!wire || len != M4_LOADER_SPEED_WORDS * 8 || fdt64_ld((const fdt64_t *)wire) != M4_LOADER_SPEED_MAGIC) return;
    const unsigned fields[] = {6, 9, 10, 15, 16, 19};
    for (unsigned i = 0; i < sizeof(fields) / sizeof(*fields); i++)
        fdt64_st(wire + fields[i], word(fields[i]));
    fdt64_st(wire + 28, COUNTER());
}
