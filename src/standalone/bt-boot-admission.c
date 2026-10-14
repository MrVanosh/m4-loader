/* SPDX-License-Identifier: MIT
 * PCI fields/ECAM layout from the exact production Linux pci_regs.h/ecam.c.
 * Observe inherited state only; BME clear is admission, never a drain claim.
 */
#include "bt-boot-admission.h"
#include <string.h>

static int rd(m4_bt_config_read32 read, void *opaque, uint64_t address,
              uint32_t *out, struct m4_bt_boot_report *report)
{
    if (++report->reads > 96 || read(opaque, address, out) || *out == UINT32_MAX)
        return -1;
    return 0;
}

int m4_bt_boot_admit(uint64_t ecam, uint64_t bytes,
                     m4_bt_config_read32 read, void *opaque,
                     struct m4_bt_boot_report *r)
{
    if (!r) return -1;
    memset(r, 0, sizeof(*r));
    if (!read || ecam != 0x1cb0000000ULL || bytes != 0x1000000)
        return -2;
    uint64_t wifi = ecam + 0x100000, bt = wifi + 0x1000;
    uint32_t v, command, peer_command, pointer, found = 0;
#define READ(address, out) do { if (rd(read, opaque, address, &(out), r)) return -3; } while (0)
    READ(ecam, r->root_id);
    if (r->root_id != 0x100c106b) return -4;
    READ(ecam + 0x0c, r->root_header);
    if (((r->root_header >> 16) & 0x7f) != 1) return -4;
    READ(ecam + 0x18, r->bus_numbers);
    READ(ecam + 4, v); r->root_command = v & 0xffff;
    /* Native R9 has bus0 before Linux performs enumeration. Do not
     * issue endpoint reads through an unconfigured bridge, and do
     * not describe this declarative graph as DMA admission. */
    if (r->bus_numbers == 0) {
        if (r->root_command != 0) return -4;
        return M4_BT_BOOT_DEFERRED;
    }
    if ((r->bus_numbers & 0xffffff) != 0x010100) return -4;
    READ(wifi, r->wifi_id); READ(bt, r->bt_id);
    if (r->wifi_id != 0x443414e4 || r->bt_id != 0x5f7214e4) return -5;
    READ(wifi + 8, v); if (v != 0x02800006) return -5;
    READ(bt + 8, v); if (v != 0x02800006) return -5;
    READ(wifi + 0x0c, v);
    if (((v >> 16) & 0xff) != 0x80) return -5;
    READ(bt + 0x0c, v);
    if (((v >> 16) & 0x7f) != 0) return -5;
    READ(bt + 0x2c, v); if (v != 0x4388106b) return -5;
    READ(wifi + 4, peer_command); peer_command &= 0xffff;
    READ(bt + 4, command);
    r->bt_command = command & 0xffff; r->wifi_command = peer_command;
    if ((command & 4) || !(command & (1U << 20))) return -6;
    READ(bt + 0x34, pointer); pointer &= 0xff;
    uint64_t visited = 0;
    while (pointer) {
        if (pointer < 0x40 || pointer > 0xfc || (pointer & 3) ||
            (visited & (1ULL << (pointer >> 2)))) return -7;
        visited |= 1ULL << (pointer >> 2);
        READ(bt + pointer, v);
        if ((v & 0xff) == 0x10) {
            /* PCIe endpoint, capability version 1 or 2; DeviceStatus fits. */
            if (found || pointer > 0xf4 || ((v >> 20) & 0xf) ||
                (((v >> 16) & 0xf) != 1 && ((v >> 16) & 0xf) != 2)) return -7;
            found = pointer;
        }
        pointer = (v >> 8) & 0xff;
    }
    if (!found) return -7;
    r->pcie_offset = found;
    READ(bt + found + 8, v); r->device_status = v >> 16;
    if (r->device_status & 0x20) return -8;
    /* Fresh final observations, with no config mutation in between. */
    READ(bt + 4, v);
    if ((v & 0xffff) != r->bt_command || (v & 4)) return -9;
    READ(wifi + 4, v); if ((v & 0xffff) != peer_command) return -9;
    READ(bt + found + 8, v); r->device_status = v >> 16;
    if (r->device_status & 0x20) return -8;
#undef READ
    return 0;
}
