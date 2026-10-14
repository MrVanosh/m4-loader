/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef M4_DIRECT_PERIPHERAL_ADT_H
#define M4_DIRECT_PERIPHERAL_ADT_H
#include "direct-peripheral-fdt.h"

struct m4_peripheral_adt_ctx { void *adt; u32 count; };
static const void *m4_peripheral_adt_get(void *ptr, const char *path,
                                      const char *name, unsigned *size)
{
    struct m4_peripheral_adt_ctx *c = ptr;
    int node = adt_path_offset(c->adt, path);
    if (node < 0) return NULL;
    if (!strcmp(name, "@child-count")) {
        c->count = adt_get_child_count(c->adt, node);
        *size = sizeof(c->count);
        return &c->count;
    }
    if (!strcmp(name, "@running-fixed-frequency")) {
        int child = node, count = 0;
        const void *fixed = NULL;
        u32 length;
        ADT_FOREACH_CHILD(c->adt, child) {
            const char *state = adt_getprop(c->adt, child, "state", &length);
            if (state && length == 8 && !memcmp(state, "running", 8)) {
                fixed = adt_getprop(c->adt, child, "fixed-frequency", size);
                count++;
            }
        }
        return count == 1 ? fixed : NULL;
    }
    return adt_getprop(c->adt, node, name, size);
}

static int m4_prepare_direct_peripherals(void *native_adt, size_t adt_bytes,
                                         void *dt, size_t capacity)
{
    struct m4_peripheral_adt_ctx c = { .adt = native_adt };
    if (m4_dart_adt_bounds(native_adt, adt_bytes)) return -1;
    return m4_direct_peripheral_fdt(dt, capacity, m4_peripheral_adt_get, &c);
}
#endif
