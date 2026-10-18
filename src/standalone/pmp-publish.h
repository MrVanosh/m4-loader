/* SPDX-License-Identifier: MIT */
/* rev 13: the PMP firmware's ADT properties for the kernel's PMP node
 * (claude-clpc-loader-pmp-request.md, 2026-10-18).
 *
 * On the node compatible "apple,t6040-pmp-v2" (if the kernel DT has one), it adds:
 *   apple,board-id, apple,dram-vendor-id        /chosen board-id, dram-vendor-id (required; u32)
 *   apple,dram-channel-disable, apple,dram-capacity,
 *   apple,pmc-msg-disabled, apple,soc-chip-variant   the iop-pmp-nub property of that name, when it is a u32
 *   apple,tunable-<name>                         every other iop-pmp-nub property, bytes verbatim,
 *                                                except the host-only ones (pmp_skip in pmp-publish.c)
 * Adds properties only: "status" and every existing property stay as they are.
 * All or nothing: when the whole set does not fit (capacity, the window up to the kernel,
 * and a reserve for the records published after it), nothing is added.
 * apple,board-id and apple,dram-vendor-id are written last, so a tree without them carries no PMP data
 * the kernel would act on. Reads the ADT only; never touches the PMP, its SRAM or the PTD windows.
 */
#ifndef M4_STANDALONE_PMP_PUBLISH_H
#define M4_STANDALONE_PMP_PUBLISH_H
#include <stddef.h>

#define M4_PMP_COMPATIBLE "apple,t6040-pmp-v2"
#define M4_PMP_ADT_NUB "/arm-io/pmp/iop-pmp-nub"
#define M4_PMP_PROP_MAX 0x4000   /* largest nub property accepted (soc-device is 4,216 B) */
#define M4_PMP_RESERVE 0x2000    /* room kept free for the late records published after this one */

/* dt: an FDT in a buffer of `capacity` bytes; room: bytes available for the final tree
 * (the window from the DT address to the kernel). Returns the number of properties added
 * (0: the kernel DT has no PMP node), or a negative value when nothing was added:
 * -1 the tree does not open, -2 no /chosen or iop-pmp-nub in the ADT, -3 board-id or
 * dram-vendor-id missing, -4 a nub property is malformed or too large, -5 no room,
 * -6 an FDT edit failed after the check (the required pair is then absent). */
int m4_pmp_publish(void *dt, size_t capacity, size_t room);

#endif
