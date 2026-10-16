/* SPDX-License-Identifier: MIT */
#ifndef M4_DIRECT_BLUETOOTH_H
#define M4_DIRECT_BLUETOOTH_H
#include <stddef.h>
#define M4_FDT_CAPACITY 131072U
int m4_direct_bluetooth_fdt(void *tree, void *workspace, size_t capacity);
#endif
