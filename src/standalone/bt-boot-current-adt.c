/* SPDX-License-Identifier: MIT */
#include "bt-boot-admission.h"
#include "adt.h"
#include <libfdt.h>
#include <string.h>
extern int m4_dart_adt_bounds(const void *, uint32_t);

static int exact(int node, const char *key, const void *value, uint32_t length)
{
    uint32_t size = 0, total = adt_get_size();
    const void *p = adt_getprop(adt, node, key, &size);
    uintptr_t start = (uintptr_t)adt, address = (uintptr_t)p;
    return p && size == length && address >= start && size <= total &&
        address - start <= total - size && !memcmp(p, value, size);
}

int m4_bt_boot_admit_current(const void *fdt, size_t bytes,
                             m4_bt_config_read32 read, void *opaque,
                             struct m4_bt_boot_report *report)
{
    if (!report) return -10;
    memset(report, 0, sizeof(*report));
    uint32_t total = adt_get_size(), chip = 0x6040;
    if (m4_dart_adt_bounds(adt, total) || adt_check_header(adt) || !fdt ||
        bytes < sizeof(struct fdt_header) || bytes > 131072 || fdt_check_full(fdt, bytes)) /* rev 12: the 128 KiB slot */
        return -10;
    int chosen = adt_path_offset(adt, "/chosen"), path[8];
    int host = adt_path_offset_trace(adt, "/arm-io/apcie0", path);
    u64 ecam, ecam_bytes;
    if (chosen < 0 || host < 0 || !exact(0, "model", "Mac16,7", 8) ||
        !exact(0, "target-type", "J616s", 6) || !exact(chosen, "chip-id", &chip, 4) ||
        !adt_is_compatible(adt, host, "apcie,t6040") ||
        adt_get_reg(adt, path, "reg", 0, &ecam, &ecam_bytes) ||
        ecam != 0x1cb0000000ULL || ecam_bytes != 0x10000000) return -11;
    int node = fdt_path_offset(fdt, "/pcie@1cb0000000"), length;
    if (node < 0 || fdt_node_check_compatible(fdt, 0, "apple,j616s")) return -12;
    const fdt32_t *reg = fdt_getprop(fdt, node, "reg", &length);
    if (!reg || length < 64 || length % 16 || /* rev 14: more reg entries (port 1) are fine */
        (((uint64_t)fdt32_to_cpu(reg[0]) << 32) | fdt32_to_cpu(reg[1])) != ecam ||
        (((uint64_t)fdt32_to_cpu(reg[2]) << 32) | fdt32_to_cpu(reg[3])) != 0x1000000)
        return -12;
    const fdt32_t *buses = fdt_getprop(fdt, node, "bus-range", &length);
    if (!buses || length != 8 || fdt32_to_cpu(buses[0]) || fdt32_to_cpu(buses[1]) < 1) /* rev 14: <0 n>, n >= 1 */
        return -12;
    /* The accepted FDT exposes a 16 MiB subset of the 256 MiB ADT ECAM. */
    return m4_bt_boot_admit(ecam, 0x1000000, read, opaque, report);
}
