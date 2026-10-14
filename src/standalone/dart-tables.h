/* SPDX-License-Identifier: MIT */
#ifndef M4_DART_TABLES_H
#define M4_DART_TABLES_H
#include <stdint.h>
#include <stddef.h>
#include "dart-capture.h"

#define M4_DT_PAGE 16384u
#define M4_DT_WORDS 2048u
#define M4_DT_TABLES 32u
#define M4_DT_SPANS 80u
#define M4_DT_SEGMENTS 8u
#define M4_DT_WINDOW (UINT64_C(1) << 36)
struct m4_dt_range { uint64_t base, size; };
struct m4_dt_input {
    struct m4_dt_range dram, tables, window;
    struct m4_dt_range excluded[6];
    uint64_t root, controller;
    uint32_t sid, params[4], protect, tcr, ttbr, excluded_count;
};
struct m4_dt_segment { uint64_t physical, legacy, remap; uint32_t size, flags; };
struct m4_dt_span { uint64_t iova, physical, size, attributes; };
struct m4_dt_segment_result { uint32_t pages, same, different, absent; uint64_t first_pa; };
struct m4_dt_result {
    uint64_t phase, active_address, begin, end, checksum;
    uint32_t loads, root_entries, leaf_entries, spans, invalid_nonzero, outside_dram;
    int32_t error;
    uint32_t stable;
    struct m4_dt_span map[M4_DT_SPANS];
    struct m4_dt_segment_result segment[M4_DT_SEGMENTS];
};
/* One declared current-boot table pool, never a translated device/buffer page.
 * ponytail: 32 table pages / 80 output spans; refuse larger inventories rather
 * than silently dropping mappings. Raise only with a reviewed output format. */
struct m4_dt_workspace { uint64_t words[M4_DT_TABLES * M4_DT_WORDS]; };

int m4_dt_prepare(const struct m4_dart_capture *, struct m4_dt_input[3],
                  struct m4_dt_segment[M4_DT_SEGMENTS]);
int m4_dt_collect(const struct m4_dt_input *, const struct m4_dt_segment *,
                  struct m4_dt_workspace *, struct m4_dt_result *);
/* Native read-only callbacks. Tests supply strict fakes. */
uint64_t m4_dt_read64(uint64_t);
uint32_t m4_dt_read32(uint64_t);
uint64_t m4_dt_counter(void);
int m4_dt_progress(unsigned, uint64_t);
int m4_dart_tables_after_cpu(int, const struct j616s_call_result *);
#endif
