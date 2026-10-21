/* SPDX-License-Identifier: MIT */
/* rev 14: the AOP DART's DAPF (address filter), programmed by the loader from the ADT
 * (claude-aurora-loader-dapf-aop-request.md, 2026-10-20).
 *
 * m4_aop_dapf_collect()  in the cached ADT window: /arm-io/dart-aop must have instance record 1 = "DAPF" (FPAD),
 *                        reg[1] = the DAPF block (0x5_10f10000 on J616s, else refused) and dapf-instance-0 in m1n1's
 *                        t8110a layout (52-byte records, at most 32). Reads the ADT only.
 * m4_aop_dapf_program()  in the uncached MMIO window next to the MTP DAPF: only while the AOP CPU is stopped
 *                        (ASC +0x44 bit 4 clear). Each slice is written in m1n1's order (+0x04, +0x08, +0x10, +0x00,
 *                        +0x20) and read back (end compared without its low two bits). On any mismatch every slice
 *                        gets its previous values back.
 * m4_aop_dapf_publish()  late: /chosen/m4,dapf-programmed = "/iommu@510f00000" when programmed and the kernel DT has
 *                        that node; otherwise /chosen/m4,dapf-aop-refused = the reason. The kernel keeps its own
 *                        DAPF code until it sees the marker. Never touches dart-pmp or any other DART.
 */
#ifndef M4_STANDALONE_AOP_DAPF_H
#define M4_STANDALONE_AOP_DAPF_H
#include <stddef.h>
#include <stdint.h>

#define M4_AOP_DAPF_BASE 0x510f10000ULL    /* dart-aop reg[1] */
#define M4_AOP_ASC_BASE 0x510600000ULL     /* aop reg[0]; +0x44 bit 4 = CPU_RUN */
#define M4_AOP_DAPF_FDT_NODE "/iommu@510f00000"
#define M4_AOP_DAPF_MAX 32

enum m4_aop_dapf_state {
    M4_AOP_DAPF_NOT_RUN,
    M4_AOP_DAPF_COLLECTED,
    M4_AOP_DAPF_PROGRAMMED,
    M4_AOP_DAPF_NO_ADT,
    M4_AOP_DAPF_NO_DAPF,
    M4_AOP_DAPF_BAD_BASE,
    M4_AOP_DAPF_BAD_TABLE,
    M4_AOP_DAPF_AOP_RUNNING,
    M4_AOP_DAPF_MISMATCH,
};

int m4_aop_dapf_collect(void);
int m4_aop_dapf_program(void);
int m4_aop_dapf_state(void);
int m4_aop_dapf_publish(void *dt, size_t capacity);

#endif
