/* SPDX-License-Identifier: MIT */
#ifndef M4_BOOT_PROFILE_H
#define M4_BOOT_PROFILE_H
#include "boot-timing.h"
#define M4_BOOT_PROFILE_PROPERTY "m4,boot-profile-v1"
#define M4_BOOT_PROFILE_MAGIC UINT64_C(0x4d344250524f4631)
enum m4_boot_profile_stage {
    M4_PROFILE_PREPARE_START,
    M4_PROFILE_ADT_METADATA_READY,
    M4_PROFILE_MCC_READY,
    M4_PROFILE_ARENA_CPU_READY,
    M4_PROFILE_OWNERSHIP_READY,
    M4_PROFILE_BASE_FDT_READY,
    M4_PROFILE_DIRECT_FDT_READY,
    M4_PROFILE_PERIPHERAL_FDT_READY,
    M4_PROFILE_WATCHDOG_READY,
    M4_PROFILE_DAPF_READY,
    M4_PROFILE_BEFORE_PCIE,
    M4_PROFILE_AFTER_PCIE,
    M4_PROFILE_BLUETOOTH_READY,
    M4_PROFILE_DISPLAY_BEGIN,
    M4_PROFILE_DISPLAY_OWNERSHIP_READY,
    M4_PROFILE_DISPLAY_METADATA_READY,
    M4_PROFILE_DISPLAY_TABLE_ADT_READY,
    M4_PROFILE_DISPLAY_STREAM0_FINISHED,
    M4_PROFILE_DISPLAY_STREAM1_FINISHED,
    M4_PROFILE_DISPLAY_STREAM2_FINISHED,
    M4_PROFILE_DISPLAY_CAPTURE_RETURNED,
    M4_PROFILE_DISPLAY_WIRE_FINISHED,
    M4_PROFILE_MEDIA_ADT_READY,
    M4_PROFILE_MEDIA_EXPORT_FINISHED,
    M4_PROFILE_DISPLAY_MEDIA_FINISHED,
    M4_PROFILE_DISPLAY_DT_COPIED,
    M4_PROFILE_BEFORE_PROFILE_PUBLISH,
    M4_PROFILE_COUNT
};
#define M4_BOOT_PROFILE_WORDS (10+M4_PROFILE_COUNT)
extern uint64_t m4_boot_profile_ticks[M4_PROFILE_COUNT];
extern int64_t m4_boot_profile_display_rc, m4_boot_profile_media_rc;
static inline void m4_boot_profile_mark(enum m4_boot_profile_stage stage)
{
#ifdef M4_PROFILE_TEST
    extern uint64_t m4_profile_test_counter(void);
    m4_boot_profile_ticks[stage]=m4_profile_test_counter();
#else
    m4_boot_profile_ticks[stage]=m4_boot_counter();
#endif
}
int m4_boot_profile_publish(void *,void *,size_t,uint64_t,uint64_t);
#endif
