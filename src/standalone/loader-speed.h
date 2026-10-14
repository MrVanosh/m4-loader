/* SPDX-License-Identifier: MIT */
/* Boot-speed bookkeeping shared by the loader, SMP start and publication.
 * All state lives here, linked after every pinned object, so the GPU
 * capture ABI offsets of the loader image do not move. */
#ifndef M4_STANDALONE_LOADER_SPEED_H
#define M4_STANDALONE_LOADER_SPEED_H
#include <stdint.h>
#include <stddef.h>

#define M4_LOADER_SPEED_PROPERTY "m4,loader-speed-v1"
#define M4_LOADER_SPEED_MAGIC UINT64_C(0x4d344c5350454431)
#define M4_LOADER_SPEED_WORDS 32
/* The DT keeps its GPU-pinned address arena+0x10000. The arena is placed so
 * that the 2 MiB-aligned kernel lands at arena+0x50000, which leaves a fixed
 * 256 KiB DT window instead of the former 64 KiB slot. */
#define M4_FDT_WINDOW 0x40000UL
#define M4_ARENA_KERNEL_OFFSET 0x50000UL

enum m4_copy_check {
    M4_COPY_NOT_RUN = 0,
    M4_COPY_MATCHED = 1,
    M4_COPY_RECOPIED = 2, /* sampled mismatch after the clean; uncached copy redone */
};

struct m4_loader_speed {
    void *late_tree, *late_scratch;
    size_t late_capacity;
    uint64_t rng_seed_bytes;
    uint64_t payload_check, payload_mismatches, dt_check, dt_mismatches;
    uint64_t smp_mode, smp_first_launch, smp_last_running, smp_all_ready;
    uint64_t bringup_export, bringup_ticks;
    uint64_t dt_bytes, kernel_offset, table_bytes;
    uint64_t *record; /* in-place view of the published record inside the final DT */
};
extern struct m4_loader_speed m4_speed;

void *m4_arena_place(size_t bytes);
int m4_loader_speed_publish(void *tree, size_t capacity, uint64_t frequency);
void m4_loader_speed_finish(void *dt);
#endif
