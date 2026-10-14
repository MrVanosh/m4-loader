/* SPDX-License-Identifier: MIT */
/* Current ADT RAM facts, kept separate from live OF resources. */
#ifndef M4_BRINGUP_HANDOFF_H
#define M4_BRINGUP_HANDOFF_H
#include "libfdt.h"
#include <stdint.h>
#include <string.h>

typedef const void *(*m4_bringup_getprop_fn)(void *, const char *, const char *, unsigned *);

static int m4_bringup_disjoint(const void *a, size_t na, const void *b, size_t nb)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return x <= UINTPTR_MAX-na && y <= UINTPTR_MAX-nb && (x+na <= y || y+nb <= x);
}

static int m4_bringup_extent(const void *base, size_t bytes, const void *value, size_t length)
{
    uintptr_t a = (uintptr_t)base, b = (uintptr_t)value;
    return a <= UINTPTR_MAX-bytes && b >= a && length <= bytes && b-a <= bytes-length;
}

static unsigned m4_bringup_le16(const unsigned char *p)
{ return p[0] | ((unsigned)p[1]<<8); }

static int m4_bringup_pmgr_find(const unsigned char *data, unsigned count, unsigned id)
{
    int found = -1;
    for (unsigned i = 0; i < count; i++) if (m4_bringup_le16(data+i*48+26) == id) {
        if (found >= 0) return -FDT_ERR_BADVALUE;
        found = i;
    }
    return found;
}

static int m4_bringup_pmgr_add(unsigned indices[64], unsigned *count, unsigned index)
{
    for (unsigned i = 0; i < *count; i++) if (indices[i] == index) return 0;
    if (*count == 64) return -FDT_ERR_BADVALUE;
    indices[(*count)++] = index;
    return 0;
}

static int m4_bringup_pmgr_select(const unsigned char *data, unsigned length, unsigned indices[64],
                                 const void *source, size_t bytes, m4_bringup_getprop_fn get, void *ctx,
                                 unsigned *missing_names)
{
    /* Keep source records/parents exact; the full table crowds out live BT calibration. */
    static const char *paths[] = {"/arm-io/dcp", "/arm-io/disp0", "/arm-io/isp0",
                                  "/arm-io/atc-phy2", "/arm-io/usb-drd2"};
    static const char *keys[] = {"clock-ids", "clock-gates", "power-gates", "function-bw_req_interrupt0"};
    static const char *names[] = {"AFI", "FAB2_SOC", "ATC2_COMMON", "ATC2_PHYMXWRAP", "ATC2_USB_AON", "ATC2_USB"};
    if (length < 96 || length%48 || data[3] != data[51]) return -FDT_ERR_BADVALUE;
    unsigned rows = length/48, count = 0;
    *missing_names = 0;
    for (unsigned i = 0; i < sizeof(paths)/sizeof(*paths); i++) {
        for (unsigned j = 0; j < sizeof(keys)/sizeof(*keys); j++) {
            unsigned n = 0; const unsigned char *value = get(ctx, paths[i], keys[j], &n);
            if (!value) continue;
            if (n > 128 || n%4 || !m4_bringup_extent(source, bytes, value, n)) return -FDT_ERR_BADVALUE;
            unsigned first = 0;
            int required = j == 1 || j == 2;
            if (j == 3) {
                unsigned size = 0; const unsigned char *handle = get(ctx, "/arm-io/pmgr", "AAPL,phandle", &size);
                static const unsigned char birq[] = {0x51, 0x52, 0x49, 0x42};
                if (n != 12 || !handle || size != 4 || !m4_bringup_extent(source, bytes, handle, size) ||
                    memcmp(value, handle, 4) || memcmp(value+4, birq, 4)) return -FDT_ERR_BADVALUE;
                first = 8; required = 1;
            }
            for (unsigned k = first; k < n; k += 4) {
                uint32_t id = value[k] | ((uint32_t)value[k+1]<<8) |
                              ((uint32_t)value[k+2]<<16) | ((uint32_t)value[k+3]<<24);
                if (!id || id > UINT16_MAX) {
                    if (required) return -FDT_ERR_BADVALUE;
                    continue;
                }
                int row = m4_bringup_pmgr_find(data, rows, id);
                if (row == -1 && !required) continue;
                if (row < 0 || m4_bringup_pmgr_add(indices, &count, row)) return -FDT_ERR_BADVALUE;
            }
        }
    }
    for (unsigned i = 0; i < sizeof(names)/sizeof(*names); i++) {
        int found = -1;
        for (unsigned j = 0; j < rows; j++) if (!memcmp(data+j*48+32, names[i], strlen(names[i])+1)) {
            if (found >= 0) return -FDT_ERR_BADVALUE;
            found = j;
        }
        if (found >= 0 && m4_bringup_pmgr_add(indices, &count, found)) return -FDT_ERR_BADVALUE;
        if (found < 0) *missing_names |= 1U<<i;
    }
    for (unsigned i = 0; i < count; i++) for (unsigned j = 0; j < 2; j++) {
        unsigned id = m4_bringup_le16(data+indices[i]*48+4+j*2);
        if (!id) continue;
        int row = m4_bringup_pmgr_find(data, rows, id);
        if (row < 0 || m4_bringup_pmgr_add(indices, &count, row)) return -FDT_ERR_BADVALUE;
    }
    return count ? (int)count : -FDT_ERR_BADVALUE;
}

static int m4_bringup_export_begin(void *tree, size_t bytes, void *scratch, size_t capacity,
                                   int bounds_result)
{
    if (!tree || !scratch || bytes < sizeof(struct fdt_header) || bytes > INT32_MAX ||
        capacity > INT32_MAX || !m4_bringup_disjoint(tree, bytes, scratch, capacity) ||
        fdt_check_full(tree, bytes)) return -FDT_ERR_BADVALUE;
    int chosen = fdt_path_offset(tree, "/chosen");
    if (chosen < 0) return chosen;
    if (fdt_getprop(tree, chosen, "m4-bringup-export-status", NULL)) return -FDT_ERR_EXISTS;
    int ret = fdt_open_into(tree, scratch, capacity);
    if (ret) return ret;
    fdt32_t status[] = {cpu_to_fdt32(1), cpu_to_fdt32(bounds_result),
                        cpu_to_fdt32(-FDT_ERR_BADSTATE), cpu_to_fdt32(-FDT_ERR_BADSTATE)};
    chosen = fdt_path_offset(scratch, "/chosen");
    if ((ret = fdt_setprop(scratch, chosen, "m4-bringup-export-status", status, sizeof(status))) ||
        (ret = fdt_pack(scratch)) || (ret = fdt_check_full(scratch, capacity))) return ret;
    if (fdt_totalsize(scratch) > bytes) return -FDT_ERR_NOSPACE;
    memcpy(tree, scratch, fdt_totalsize(scratch));
    return 0;
}

static int m4_bringup_handoff_write(void *tree, size_t bytes, void *scratch, size_t capacity,
                                  const void *source_adt, size_t adt_bytes,
                                  m4_bringup_getprop_fn get, void *ctx)
{
    static const struct { const char *name, *path, *keys; } nodes[] = {
        {"board", "/", "model\0target-type\0"},
        {"firmware", "/chosen", "chip-id\0board-id\0firmware-version\0system-firmware-version\0"},
        {"arm-io", "/arm-io", "compatible\0ranges\0clock-frequencies\0"},
        {"dcp", "/arm-io/dcp", "compatible\0reg\0interrupt-parent\0interrupts\0iommu-parent\0clock-gates\0power-gates\0role\0"},
        {"dcp-firmware", "/arm-io/dcp/iop-dcp-nub", "compatible\0uuid\0segment-names\0segment-ranges\0pre-loaded\0running\0quiesced\0"},
        {"disp0", "/arm-io/disp0", "compatible\0reg\0interrupt-parent\0interrupts\0clock-ids\0clock-gates\0power-gates\0iommu-parent\0function-bw_req_interrupt0\0"},
        {"dart-dcp", "/arm-io/dart-dcp", "compatible\0reg\0interrupt-parent\0interrupts\0AAPL,phandle\0sid\0page-size\0vm-base\0vm-size\0pt-region-23\0l2-tt-23\0"},
        {"dart-disp0", "/arm-io/dart-disp0", "compatible\0reg\0interrupt-parent\0interrupts\0AAPL,phandle\0sid\0page-size\0vm-base\0vm-size\0pt-region-0\0pt-region-16\0l2-tt-0\0l2-tt-16\0"},
        {"dcp-mapper", "/arm-io/dart-dcp/mapper-dcp", "compatible\0AAPL,phandle\0reg\0"},
        {"disp-mapper", "/arm-io/dart-disp0/mapper-disp0", "compatible\0AAPL,phandle\0reg\0"},
        {"disp-piodma-mapper", "/arm-io/dart-disp0/mapper-disp0-piodma", "compatible\0AAPL,phandle\0reg\0"},
        {"backlight", "/backlight", "user-accessible-max-nits\0LminProduct\0LmidProduct\0LmaxProduct\0dcp-brightness-node\0backlight-calibration\0backlight-marketing-table\0calibratedMidCurrent\0calibratedMaxCurrent\0"},
        {"pmgr", "/arm-io/pmgr", "AAPL,phandle\0reg\0ps-groups\0devices\0"},
        {"isp0", "/arm-io/isp0", "compatible\0reg\0interrupt-parent\0interrupts\0iommu-parent\0clock-ids\0clock-gates\0power-gates\0isp-version\0sensor-type\0cam-connections-scheme\0camera-front\0pre-loaded\0uuid\0segment-names\0segment-ranges\0"},
        {"dart-isp0", "/arm-io/dart-isp0", "compatible\0reg\0AAPL,phandle\0sid\0page-size\0vm-base\0vm-size\0bypass-15\0remap\0"},
        {"isp-mapper", "/arm-io/dart-isp0/mapper-isp0", "compatible\0AAPL,phandle\0reg\0"}
    };
    if (!tree || !scratch || !source_adt || !get || bytes < sizeof(struct fdt_header) ||
        bytes > INT32_MAX || capacity > INT32_MAX || !adt_bytes || adt_bytes > 4*1024*1024 ||
        !m4_bringup_disjoint(tree, bytes, scratch, capacity) ||
        !m4_bringup_disjoint(source_adt, adt_bytes, scratch, capacity) ||
        !m4_bringup_disjoint(source_adt, adt_bytes, tree, bytes) || fdt_check_full(tree, bytes) ||
        fdt_node_check_compatible(tree, 0, "apple,j616s") ||
        fdt_node_check_compatible(tree, 0, "apple,t6040")) return -FDT_ERR_BADVALUE;
    unsigned length = 0;
    const unsigned char *value = get(ctx, "/", "target-type", &length);
    if (!value || !m4_bringup_extent(source_adt, adt_bytes, value, length) ||
        length != 6 || memcmp(value, "J616s", 6)) return -FDT_ERR_BADVALUE;
    value = get(ctx, "/chosen", "chip-id", &length);
    if (!value || !m4_bringup_extent(source_adt, adt_bytes, value, length) ||
        length != 4 || value[0] != 0x40 || value[1] != 0x60 || value[2] || value[3])
        return -FDT_ERR_BADVALUE;
    value = get(ctx, "/chosen", "board-id", &length);
    if (!value || !m4_bringup_extent(source_adt, adt_bytes, value, length) ||
        length != 4 || value[0] != 6 || value[1] || value[2] || value[3]) return -FDT_ERR_BADVALUE;
    int parent = fdt_path_offset(tree, "/chosen");
    if (parent < 0) return parent;
    if (fdt_subnode_offset(tree, parent, "m4-bringup-handoff") != -FDT_ERR_NOTFOUND)
        return -FDT_ERR_EXISTS;
    int ret = fdt_open_into(tree, scratch, capacity);
    if (ret) return ret;
    parent = fdt_path_offset(scratch, "/chosen");
    int top = fdt_add_subnode(scratch, parent, "m4-bringup-handoff");
    if (top < 0) return top;
    if ((ret = fdt_setprop_u32(scratch, top, "format-version", 1)) ||
        (ret = fdt_setprop_u32(scratch, top, "source-adt-bytes", adt_bytes)) ||
        (ret = fdt_setprop_string(scratch, top, "source-kind", "current-loader-adt-ram")) ||
        (ret = fdt_setprop_string(scratch, top, "adt-numeric-endianness", "little"))) return ret;
    unsigned total = 0, fields = 0;
    for (unsigned i = 0; i < sizeof(nodes)/sizeof(*nodes); i++) {
        top = fdt_path_offset(scratch, "/chosen/m4-bringup-handoff");
        int node = fdt_add_subnode(scratch, top, nodes[i].name);
        if (node < 0) return node;
        if ((ret = fdt_setprop_string(scratch, node, "source-path", nodes[i].path))) return ret;
        length = 0;
        value = get(ctx, nodes[i].path, "name", &length);
        if (!value) {
            if ((ret = fdt_setprop_empty(scratch, node, "source-absent"))) return ret;
            continue;
        }
        if (!length || length > 64 || !m4_bringup_extent(source_adt, adt_bytes, value, length) || value[length-1])
            return -FDT_ERR_BADVALUE;
        for (const char *key = nodes[i].keys; *key; key += strlen(key)+1) {
            length = 0;
            value = get(ctx, nodes[i].path, key, &length);
            if (!value) continue;
            unsigned maximum = !strcmp(key, "devices") ? 32768 : 4096;
            if (length > maximum || !m4_bringup_extent(source_adt, adt_bytes, value, length) ||
                total > 49152-length || ++fields > 256) return -FDT_ERR_BADVALUE;
            /* Raw malformed/empty fields remain observations for the native decoder. */
            char name[64] = "adt-";
            size_t n = strlen(key);
            if (n > sizeof(name)-5) return -FDT_ERR_BADVALUE;
            memcpy(name+4, key, n+1);
            if (!strcmp(key, "devices")) {
                unsigned indices[64], missing_names;
                int count = m4_bringup_pmgr_select(value, length, indices, source_adt, adt_bytes, get, ctx, &missing_names);
                if (count < 0) return count;
                void *selected;
                if ((ret = fdt_setprop_string(scratch, node, "devices-selection", "bringup-parent-closure-v1")) ||
                    (ret = fdt_setprop_u32(scratch, node, "devices-original-bytes", length)) ||
                    (ret = fdt_setprop_u32(scratch, node, "devices-record-bytes", 48)) ||
                    (ret = fdt_setprop_u32(scratch, node, "devices-missing-usb-name-mask", missing_names)) ||
                    (ret = fdt_setprop_string(scratch, node, "devices-source-id-format", "u16")) ||
                    (ret = fdt_setprop_placeholder(scratch, node, name, count*48, &selected))) return ret;
                for (int k = 0; k < count; k++) memcpy((unsigned char *)selected+k*48, value+indices[k]*48, 48);
                length = count*48;
            } else if ((ret = fdt_setprop(scratch, node, name, value, length))) return ret;
            total += length;
        }
    }
    top = fdt_path_offset(scratch, "/chosen/m4-bringup-handoff");
    if ((ret = fdt_setprop_u32(scratch, top, "raw-property-bytes", total)) ||
        (ret = fdt_setprop_u32(scratch, top, "raw-property-count", fields)) ||
        (ret = fdt_pack(scratch)) || (ret = fdt_check_full(scratch, capacity))) return ret;
    if (fdt_totalsize(scratch) > bytes) return -FDT_ERR_NOSPACE;
    memcpy(tree, scratch, fdt_totalsize(scratch));
    return 0;
}
#endif
