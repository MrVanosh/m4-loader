/* SPDX-License-Identifier: MIT */
/* Inert, separate timing property; it never admits devices or CPUs. */
#include "boot-timing.h"
#include "libfdt.h"
#include "loader-speed.h" /* 256 KiB DT window bound */
#include <string.h>

uint64_t m4_boot_ticks[M4_BOOT_TIMING_COUNT];
_Static_assert(M4_BOOT_TIMING_WORDS * 8 == 160, "bounded boot timing ABI");

int m4_boot_timing_publish(void *tree, void *scratch, size_t capacity,
                           uint64_t frequency, uint64_t mpidr)
{
    if (!tree || !scratch || capacity < 16384 || capacity > M4_FDT_WINDOW ||
        frequency != UINT64_C(1000000000) || (mpidr & UINT64_C(0xff00ffffff)) != 0x10100 ||
        fdt_check_full(tree, capacity)) return -1;
    uintptr_t a = (uintptr_t)tree, b = (uintptr_t)scratch;
    size_t bytes = fdt_totalsize(tree);
    if (a > UINTPTR_MAX - bytes || b > UINTPTR_MAX - capacity ||
        (a < b + capacity && b < a + bytes)) return -1;
    for (unsigned i = 0; i < M4_BOOT_TIMING_COUNT; i++)
        if (!m4_boot_ticks[i] || (i && m4_boot_ticks[i] < m4_boot_ticks[i - 1])) return -2;
    if (m4_boot_ticks[M4_BOOT_TIMING_COUNT - 1] - m4_boot_ticks[0] > frequency * 86400)
        return -2;
    uint64_t wire[M4_BOOT_TIMING_WORDS] = {
        cpu_to_fdt64(M4_BOOT_TIMING_MAGIC), cpu_to_fdt64(1), cpu_to_fdt64(sizeof(wire)),
        cpu_to_fdt64(1), /* Final mark precedes publication, last DT copy and branch. */
        cpu_to_fdt64(frequency), cpu_to_fdt64(mpidr & UINT64_C(0xff00ffffff)),
        cpu_to_fdt64(M4_BOOT_TIMING_COUNT), 0
    };
    for (unsigned i = 0; i < M4_BOOT_TIMING_COUNT; i++) wire[8 + i] = cpu_to_fdt64(m4_boot_ticks[i]);
    int ret = fdt_open_into(tree, scratch, capacity);
    if (ret) return ret;
    int chosen = fdt_path_offset(scratch, "/chosen");
    if (chosen < 0 || fdt_getprop(scratch, chosen, M4_BOOT_TIMING_PROPERTY, NULL)) return -3;
    if ((ret = fdt_setprop(scratch, chosen, M4_BOOT_TIMING_PROPERTY, wire, sizeof(wire))) ||
        (ret = fdt_pack(scratch)) || fdt_check_full(scratch, capacity)) return ret ? ret : -4;
    memcpy(tree, scratch, fdt_totalsize(scratch));
    return 0;
}
