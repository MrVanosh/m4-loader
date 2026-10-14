/* SPDX-License-Identifier: MIT */
#ifndef M4_STANDALONE_BOOT_TIMING_H
#define M4_STANDALONE_BOOT_TIMING_H
#include <stdint.h>
#include <stddef.h>

#define M4_BOOT_TIMING_PROPERTY "m4,boot-timing-v1"
#define M4_BOOT_TIMING_MAGIC UINT64_C(0x4d344254494d4531)
enum m4_boot_timing_stage {
    M4_BOOT_BEFORE_BSS,
    M4_BOOT_BSS_READY,
    M4_BOOT_PREPARE_START,
    M4_BOOT_DEVICES_READY,
    M4_BOOT_KERNEL_TAIL_ZEROED,
    M4_BOOT_KERNEL_COPIED,
    M4_BOOT_INITRD_COPIED,
    M4_BOOT_INITIAL_DT_COPIED,
    M4_BOOT_PEERS_READY,
    M4_BOOT_DISPLAY_CAPTURE_FINISHED,
    M4_BOOT_DISPLAY_PUBLISHED,
    M4_BOOT_BEFORE_HANDOFF_PUBLISH,
    M4_BOOT_TIMING_COUNT
};
#define M4_BOOT_TIMING_WORDS (8 + M4_BOOT_TIMING_COUNT)
extern uint64_t m4_boot_ticks[M4_BOOT_TIMING_COUNT];

static inline uint64_t m4_boot_counter(void)
{
    uint64_t value;
    __asm__ volatile("dsb sy; isb; mrs %0, cntpct_el0" : "=r"(value) : : "memory");
    return value;
}
int m4_boot_timing_publish(void *tree, void *scratch, size_t capacity,
                           uint64_t frequency, uint64_t mpidr);
#endif
