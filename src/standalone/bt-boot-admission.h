/* SPDX-License-Identifier: MIT */
#ifndef M4_BT_BOOT_ADMISSION_H
#define M4_BT_BOOT_ADMISSION_H
#include <stddef.h>
#include <stdint.h>
#define M4_BT_BOOT_DEFERRED 1
typedef int (*m4_bt_config_read32)(void *, uint64_t, uint32_t *);
struct m4_bt_boot_report {
    uint32_t reads, root_id, bus_numbers, bt_id, wifi_id;
    uint32_t bt_command, wifi_command, pcie_offset, device_status;
    uint32_t root_command, root_header;
};
/* No write/reset callback exists. Return0 means observed endpoints;
 * positive DEFERRED means exact unconfigured root, requiring actual
 * Linux cold-reset/RID lifecycle and late function guards. Negative
 * errors retain the baseline desktop graph. */
int m4_bt_boot_admit(uint64_t, uint64_t, m4_bt_config_read32, void *,
                     struct m4_bt_boot_report *);
int m4_bt_boot_admit_current(const void *, size_t, m4_bt_config_read32,
                             void *, struct m4_bt_boot_report *);
#endif
