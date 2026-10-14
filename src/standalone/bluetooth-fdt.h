/* SPDX-License-Identifier: MIT */
#ifndef M4_BLUETOOTH_FDT_H
#define M4_BLUETOOTH_FDT_H
#include <stddef.h>
#define M4_BT_FDT_CAPACITY 65536
#define M4_BT_BF_BYTES 17852
#define M4_BT_HOST "/pcie@1cb0000000"
#define M4_BT_BRIDGE M4_BT_HOST "/pci@0,0"
#define M4_BT_PATH M4_BT_BRIDGE "/bluetooth@0,1"
/* Explicit native-experiment preparation. The caller supplies
 * validated current-boot calibration, never retained calibration from a file.
 * Discard workspace on any error. Neither source nor calibration is changed.
 * The endpoint has a private-probe marker; a separate guarded module binds it.
 * Publishing the changed iommu-map affects PCI enumeration/DART ownership. */
int m4_prepare_bluetooth_fdt(void *workspace, size_t capacity,
                             const void *source, size_t source_bytes,
                             const void *beamforming, size_t beamforming_bytes);
#endif
