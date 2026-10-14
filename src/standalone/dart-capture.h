/* SPDX-License-Identifier: MIT */
#ifndef M4_DART_CAPTURE_H
#define M4_DART_CAPTURE_H
#include <stdint.h>
#include <stddef.h>
#define M4_DART_COUNT 16
#define M4_DART_CPU_ACCEPTED UINT64_C(0x4350555041535335)
enum { DART_EMPTY, DART_VALIDATING, DART_READY, DART_READING, DART_COMPLETE, DART_REFUSED };
struct m4_dart_read {
 uint64_t address, before, after;
 uint32_t value, done;
};
struct m4_dart_capture {
 uint64_t phase, adt_address, adt_bytes, frequency, begin, end, mpidr;
 uint64_t base[2], span[2], mapper[3];
 uint32_t active, completed;
 int32_t error;
 uint32_t reserved;
 struct m4_dart_read read[M4_DART_COUNT];
};
extern volatile uint64_t m4_smp_cpu_result_accepted;
extern struct m4_dart_capture m4_dart_result;
int m4_dart_adt_bounds(const void *, uint32_t);
int m4_dart_prepare(struct m4_dart_capture *, const void *, uint32_t);
int m4_dart_collect(struct m4_dart_capture *);
int m4_dart_photo_hold(uint64_t);
uint64_t m4_dart_counter(void);
uint32_t m4_dart_read32(uint64_t);
int m4_dart_before(unsigned, const struct m4_dart_read *);
void m4_dart_after(unsigned, const struct m4_dart_read *);
struct j616s_call_result;
int m4_dart_after_cpu(int, const struct j616s_call_result *);
#endif
