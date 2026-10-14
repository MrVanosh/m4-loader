/* SPDX-License-Identifier: MIT */
#ifndef M4_DART_PRESERVE_H
#define M4_DART_PRESERVE_H
#include "dart-tables.h"

/* A firmware root slot covers 2048 16-KiB pages. Reserve the entire slot,
 * including currently invalid leaves, so Linux cannot allocate into a live
 * firmware table. These are low-36 IOVAs, not proof of a high alias. */
#define M4_DP_SLOT (UINT64_C(1) << 25)
#define M4_DP_RANGES 32u
#define M4_DP_MAX_OWNERS 512u
struct m4_dp_plan {
    struct m4_dt_range iova[M4_DP_RANGES];
    uint32_t count, slots, sid, read_only_pages, uncached_pages;
    uint64_t pages, table_root, table_checksum;
};

/* Call immediately after the accepted current-boot m4_dt_collect, while its
 * workspace is still available. All input table bytes are the copied snapshot;
 * this function never reads MMIO, live tables, or mapped device buffers.
 * owners are the current Linux physical reservations, not historical photos.
 * Output is cleared on every refusal. This is not an atomic firmware snapshot. */
int m4_dp_plan(const struct m4_dt_input *, const struct m4_dt_result *,
               const struct m4_dt_workspace *, const struct m4_dt_range *owners,
               size_t owner_count, struct m4_dp_plan *);

/* Validate one request against the captured mapping, preserving access/cache
 * restrictions. Flags: bit 0 device-read, bit 1 device-write, bit 2 cached.
 * High IOVAs are refused, never silently masked into apparent identity. */
int m4_dp_check_mapping(const struct m4_dt_result *, uint64_t iova,
                        uint64_t physical, uint64_t size, unsigned flags);

/* Append IOVA-only reserved-memory nodes to a DIFFERENT scratch FDT. The
 * targets must already be explicitly disabled and have #address/size-cells=2.
 * No reg property means Linux IOMMU_RESV_RESERVED, avoiding implicit RW remaps
 * of firmware's read-only pages. Physical ownership must pass m4_dp_plan first.
 * The scratch output must be discarded on any failure. No device is enabled. */
int m4_dp_write_reservations(const void *base, size_t base_bytes,
                             void *scratch, size_t capacity,
                             const char *device, const struct m4_dp_plan *);
#endif
