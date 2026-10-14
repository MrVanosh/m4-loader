/* SPDX-License-Identifier: MIT */
/* Inert, current-loader ADT RAM metadata; no peripheral access or publication. */
#ifndef M4_USB_C_METADATA_H
#define M4_USB_C_METADATA_H
#include "libfdt.h"
#include <stdint.h>
#include <string.h>

typedef const void *(*m4_usb_c_getprop_fn)(void *, const char *, const char *, unsigned *);

static int m4_usb_c_extent(const void *base, size_t bytes, const void *value, size_t length)
{
    uintptr_t a = (uintptr_t)base, b = (uintptr_t)value;
    return a <= UINTPTR_MAX - bytes && b >= a && length <= bytes && b - a <= bytes - length;
}

static int m4_usb_c_disjoint(const void *a, size_t na, const void *b, size_t nb)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return x <= UINTPTR_MAX - na && y <= UINTPTR_MAX - nb && (x + na <= y || y + nb <= x);
}

static int m4_usb_c_metadata_write(void *tree, size_t bytes, void *scratch, size_t capacity,
                                    const void *source_adt, size_t adt_bytes,
                                    m4_usb_c_getprop_fn get, void *ctx)
{
    static const struct { const char *key; unsigned multiple, maximum; } keys[] = {
        {"model",1,64}, {"target-type",1,16}, {"chip-id",4,4}, {"board-id",4,4},
        {"firmware-version",1,256}, {"system-firmware-version",1,256},
        {"compatible",1,128}, {"ranges",24,1024}, {"reg",4,1024},
        {"interrupts",4,128}, {"interrupt-parent",4,32}, {"AAPL,phandle",4,4},
        {"gen",4,4}, {"queue-depth",4,8}, {"rid",4,4}, {"port-number",4,4},
        {"port-location",1,32}, {"port-type",4,4}, {"atc-phy-parent",4,4},
        {"iommu-parent",4,4}, {"clock-gates",4,32}, {"sid",4,32}, {"page-size",4,4}
    };
#define M4_USB_K(n) (UINT32_C(1) << (n))
#define M4_USB_SPMI (M4_USB_K(6)|M4_USB_K(8)|M4_USB_K(9)|M4_USB_K(10)|M4_USB_K(11)|M4_USB_K(12)|M4_USB_K(13))
#define M4_USB_HPM (M4_USB_K(6)|M4_USB_K(8)|M4_USB_K(9)|M4_USB_K(10)|M4_USB_K(11)|M4_USB_K(14)|M4_USB_K(15)|M4_USB_K(16)|M4_USB_K(17))
#define M4_USB_DRD (M4_USB_K(6)|M4_USB_K(8)|M4_USB_K(15)|M4_USB_K(18)|M4_USB_K(19))
    static const struct { const char *name, *path; uint32_t mask; } nodes[] = {
        {"board","/",M4_USB_K(0)|M4_USB_K(1)},
        {"firmware","/chosen",M4_USB_K(2)|M4_USB_K(3)|M4_USB_K(4)|M4_USB_K(5)},
        {"arm-io","/arm-io",M4_USB_K(6)|M4_USB_K(7)},
        {"aic","/arm-io/aic",M4_USB_K(6)|M4_USB_K(11)},
        {"spmi-a0","/arm-io/nub-spmi-a0",M4_USB_SPMI},
        {"spmi-a1","/arm-io/nub-spmi-a1",M4_USB_SPMI},
        {"hpm0","/arm-io/nub-spmi-a0/hpm0",M4_USB_HPM},
        {"hpm1","/arm-io/nub-spmi-a0/hpm1",M4_USB_HPM},
        {"hpm2","/arm-io/nub-spmi-a1/hpm2",M4_USB_HPM},
        {"usb-drd0","/arm-io/usb-drd0",M4_USB_DRD},
        {"usb-drd1","/arm-io/usb-drd1",M4_USB_DRD},
        {"usb-drd2","/arm-io/usb-drd2",M4_USB_DRD|M4_USB_K(9)|M4_USB_K(20)},
        {"atc-phy2","/arm-io/atc-phy2",M4_USB_K(6)|M4_USB_K(8)|M4_USB_K(11)|M4_USB_K(15)|M4_USB_K(20)},
        {"dart-usb2","/arm-io/dart-usb2",M4_USB_K(6)|M4_USB_K(8)|M4_USB_K(11)|M4_USB_K(21)|M4_USB_K(22)},
        {"mapper-usb2","/arm-io/dart-usb2/mapper-usb2",M4_USB_K(8)|M4_USB_K(11)}
    };
#undef M4_USB_DRD
#undef M4_USB_HPM
#undef M4_USB_SPMI
#undef M4_USB_K
    if (!tree || !scratch || !source_adt || !get || bytes < sizeof(struct fdt_header) ||
        bytes > INT32_MAX || capacity > INT32_MAX || !adt_bytes || adt_bytes > 4 * 1024 * 1024 ||
        !m4_usb_c_disjoint(tree, bytes, scratch, capacity) ||
        !m4_usb_c_disjoint(source_adt, adt_bytes, scratch, capacity) ||
        !m4_usb_c_disjoint(source_adt, adt_bytes, tree, bytes) || fdt_check_full(tree, bytes) ||
        fdt_node_check_compatible(tree, 0, "apple,j616s") ||
        fdt_node_check_compatible(tree, 0, "apple,t6040"))
        return -FDT_ERR_BADVALUE;
    unsigned n = 0;
    const uint8_t *value = get(ctx, "/", "target-type", &n);
    if (!value || !m4_usb_c_extent(source_adt, adt_bytes, value, n) || n != 6 || memcmp(value, "J616s", 6))
        return -FDT_ERR_BADVALUE;
    value = get(ctx, "/chosen", "chip-id", &n);
    if (!value || !m4_usb_c_extent(source_adt, adt_bytes, value, n) || n != 4 ||
        value[0] != 0x40 || value[1] != 0x60 || value[2] || value[3])
        return -FDT_ERR_BADVALUE;
    value = get(ctx, "/chosen", "board-id", &n);
    if (!value || !m4_usb_c_extent(source_adt, adt_bytes, value, n) || n != 4 ||
        value[0] != 6 || value[1] || value[2] || value[3]) return -FDT_ERR_BADVALUE;
    int parent = fdt_path_offset(tree, "/chosen");
    if (parent < 0) return parent;
    if (fdt_subnode_offset(tree, parent, "m4-usb-c-metadata") != -FDT_ERR_NOTFOUND)
        return -FDT_ERR_EXISTS;
    int ret = fdt_open_into(tree, scratch, capacity);
    if (ret) return ret;
    if ((ret = fdt_pack(scratch))) return ret;
    size_t old_used = fdt_totalsize(scratch);
    if ((ret = fdt_open_into(scratch, scratch, capacity))) return ret;
    parent = fdt_path_offset(scratch, "/chosen");
    int top = fdt_add_subnode(scratch, parent, "m4-usb-c-metadata");
    if (top < 0) return top;
    uint64_t fingerprint = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < adt_bytes; i++)
        fingerprint = (fingerprint ^ ((const uint8_t *)source_adt)[i]) * UINT64_C(1099511628211);
    if ((ret = fdt_setprop_u32(scratch, top, "format-version", 1)) ||
        (ret = fdt_setprop_string(scratch, top, "source-kind", "current-loader-adt-ram")) ||
        (ret = fdt_setprop_u32(scratch, top, "source-adt-bytes", adt_bytes)) ||
        (ret = fdt_setprop_u64(scratch, top, "source-adt-fnv1a64", fingerprint))) return ret;
    unsigned total = 0, properties = 0;
    for (unsigned i = 0; i < sizeof(nodes) / sizeof(*nodes); i++) {
        top = fdt_path_offset(scratch, "/chosen/m4-usb-c-metadata");
        int node = fdt_add_subnode(scratch, top, nodes[i].name);
        if (node < 0) return node;
        if ((ret = fdt_setprop_string(scratch, node, "source-path", nodes[i].path))) return ret;
        n = 0;
        value = get(ctx, nodes[i].path, "name", &n);
        if (!value) {
            if ((ret = fdt_setprop_empty(scratch, node, "source-absent"))) return ret;
            continue;
        }
        if (!n || n > 64 || !m4_usb_c_extent(source_adt, adt_bytes, value, n) || value[n-1])
            return -FDT_ERR_BADVALUE;
        for (unsigned j = 0; j < sizeof(keys) / sizeof(*keys); j++) {
            if (!(nodes[i].mask & (UINT32_C(1) << j))) continue;
            n = 0;
            value = get(ctx, nodes[i].path, keys[j].key, &n);
            if (!value) continue;
            if (!n || n > keys[j].maximum || n % keys[j].multiple ||
                !m4_usb_c_extent(source_adt, adt_bytes, value, n) || total > 4096 - n || ++properties > 80)
                return -FDT_ERR_BADVALUE;
            char key[64] = "adt-";
            size_t length = strlen(keys[j].key);
            if (length > sizeof(key) - 5) return -FDT_ERR_BADVALUE;
            memcpy(key + 4, keys[j].key, length + 1);
            if ((ret = fdt_setprop(scratch, node, key, value, n))) return ret;
            total += n;
        }
    }
    top = fdt_path_offset(scratch, "/chosen/m4-usb-c-metadata");
    if ((ret = fdt_setprop_u32(scratch, top, "raw-property-bytes", total)) ||
        (ret = fdt_setprop_u32(scratch, top, "raw-property-count", properties)) ||
        (ret = fdt_pack(scratch)) || (ret = fdt_check_full(scratch, capacity))) return ret;
    size_t used = fdt_totalsize(scratch);
    if (used > bytes || used < old_used || used - old_used > 8192) return -FDT_ERR_NOSPACE;
    memcpy(tree, scratch, used);
    return 0;
}
#endif
