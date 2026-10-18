/* SPDX-License-Identifier: MIT */
/* rev 13: the PMP firmware's ADT properties for the kernel's PMP node; see pmp-publish.h. */
#include "pmp-publish.h"
#include "adt.h"
#include "string.h"
#include "libfdt/libfdt.h"

#define TUNABLE "apple,tunable-"

/* iop-pmp-nub properties that are not firmware tunables: host-only, or published as
 * a boot argument below. One NUL-separated string, so there is no pointer table. */
static const char pmp_skip[] =
    "compatible\0name\0AAPL,phandle\0region-base\0region-size\0segment-names\0segment-ranges\0"
    "uuid\0pre-loaded\0running\0no-shutdown\0user-power-managed\0firmware-name\0coredump-enable\0"
    "KDebugCoreID\0ptd-driver-version\0sram-index\0dram_stats\0dvfs-domain-afc-bin\0"
    "dram-channel-disable\0dram-capacity\0pmc-msg-disabled\0soc-chip-variant\0";

static int skipped(const char *name)
{
    for (const char *s = pmp_skip; *s; s += strlen(s) + 1)
        if (!strcmp(name, s))
            return 1;
    return 0;
}

static size_t prop_cost(size_t name_len, size_t len)
{
    /* libfdt: 12-byte property header, value padded to 4, the name added to the strings block */
    return 12 + ((len + 3) & ~(size_t)3) + name_len + 1;
}

static int adt_u32(int node, const char *name, u32 *out)
{
    u32 len = 0;
    const void *p = adt_getprop(adt, node, name, &len);
    if (!p || len != 4)
        return 0;
    memcpy(out, p, 4);
    return 1;
}

/* One optional boot argument: counted (pass 1, dt NULL) or written (pass 2). */
static int boot_arg(void *dt, int fnode, int nub, const char *adt_name, const char *fdt_name,
                    size_t *need)
{
    u32 v;
    if (!adt_u32(nub, adt_name, &v))
        return 0;
    if (!dt) {
        *need += prop_cost(strlen(fdt_name), 4);
        return 1;
    }
    return fdt_setprop_u32(dt, fnode, fdt_name, v) ? -1 : 1;
}

/* The optional boot arguments: the number present, or -1 when a write fails. */
static int boot_args(void *dt, int fnode, int nub, size_t *need)
{
    int n = 0, r;
    if ((r = boot_arg(dt, fnode, nub, "dram-channel-disable", "apple,dram-channel-disable", need)) < 0)
        return r;
    n += r;
    if ((r = boot_arg(dt, fnode, nub, "dram-capacity", "apple,dram-capacity", need)) < 0)
        return r;
    n += r;
    if ((r = boot_arg(dt, fnode, nub, "pmc-msg-disabled", "apple,pmc-msg-disabled", need)) < 0)
        return r;
    n += r;
    if ((r = boot_arg(dt, fnode, nub, "soc-chip-variant", "apple,soc-chip-variant", need)) < 0)
        return r;
    return n + r;
}

int m4_pmp_publish(void *dt, size_t capacity, size_t room)
{
    if (fdt_open_into(dt, dt, (int)capacity))
        return -1;
    int fnode = fdt_node_offset_by_compatible(dt, -1, M4_PMP_COMPATIBLE);
    if (fnode < 0) {
        fdt_pack(dt);
        return 0;
    }
    int chosen = adt_path_offset(adt, "/chosen");
    int nub = adt_path_offset(adt, M4_PMP_ADT_NUB);
    u32 board, vendor;
    int rc = 0;
    if (chosen < 0 || nub < 0)
        rc = -2;
    else if (!adt_u32(chosen, "board-id", &board) || !adt_u32(chosen, "dram-vendor-id", &vendor))
        rc = -3;

    /* Pass 1: validate every property and add up what the whole set needs. */
    size_t need = prop_cost(strlen("apple,board-id"), 4) + prop_cost(strlen("apple,dram-vendor-id"), 4);
    int props = rc ? 0 : adt_get_property_count(adt, nub);
    int off = props > 0 ? adt_first_property_offset(adt, nub) : -1;
    for (int i = 0; !rc && i < props; i++, off = adt_next_property_offset(adt, off)) {
        const char *name = NULL;
        u32 len = 0;
        const void *value = adt_getprop_by_offset(adt, off, &name, &len);
        if (!value || !name || strnlen(name, 32) >= 32 || len > M4_PMP_PROP_MAX)
            rc = -4;
        else if (!skipped(name))
            need += prop_cost(strlen(TUNABLE) + strlen(name), len);
    }
    if (!rc) {
        boot_args(NULL, fnode, nub, &need);
        size_t used = fdt_off_dt_strings(dt) + fdt_size_dt_strings(dt);
        if (used + need + M4_PMP_RESERVE > capacity || used + need + M4_PMP_RESERVE > room)
            rc = -5;
    }
    if (rc) {
        fdt_pack(dt);
        return rc;
    }

    /* Pass 2: the tunables and optional boot arguments, then the required pair. */
    int added = 0;
    off = adt_first_property_offset(adt, nub);
    for (int i = 0; i < props; i++, off = adt_next_property_offset(adt, off)) {
        const char *name = NULL;
        u32 len = 0;
        const void *value = adt_getprop_by_offset(adt, off, &name, &len);
        if (skipped(name))
            continue;
        char key[64];
        size_t n = strlen(TUNABLE);
        memcpy(key, TUNABLE, n);
        memcpy(key + n, name, strlen(name) + 1);
        if (fdt_setprop(dt, fnode, key, value, (int)len)) {
            fdt_pack(dt);
            return -6;
        }
        added++;
    }
    int r = boot_args(dt, fnode, nub, &need);
    if (r < 0) {
        fdt_pack(dt);
        return -6;
    }
    added += r;
    if (fdt_setprop_u32(dt, fnode, "apple,dram-vendor-id", vendor) ||
        fdt_setprop_u32(dt, fnode, "apple,board-id", board)) {
        fdt_delprop(dt, fnode, "apple,dram-vendor-id");
        fdt_pack(dt);
        return -6;
    }
    fdt_pack(dt);
    return added + 2;
}
