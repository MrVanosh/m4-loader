/* SPDX-License-Identifier: GPL-2.0-only */
/* Linux host check after make J616S_MEMORY_DIAGNOSTIC=1 build-cfg:
 * cc -std=c11 -Wall -Wextra -Werror tests/j616s_ranges.c -o /tmp/j616s-ranges
 * /tmp/j616s-ranges
 * Include the real diagnostic implementation to test its property reads too.
 */
#include <assert.h>

#include "../src/j616s_diag.c"

void *adt;
struct boot_args cur_boot_args;
char _base[1], _end[1];
static const void *property_value;
static u32 property_length;
static int property_reads;

const void *adt_getprop(const void *dt, int node, const char *name, u32 *len)
{
    (void)dt;
    (void)node;
    (void)name;
    property_reads++;
    *len = property_length;
    return property_value;
}

int adt_check_header(const void *dt)
{
    (void)dt;
    return 0;
}
int adt_path_offset(const void *dt, const char *path)
{
    (void)dt;
    (void)path;
    return 0;
}
int uart_printf(const char *fmt, ...)
{
    (void)fmt;
    return 0;
}
void uart_puts(const char *s)
{
    (void)s;
}
void uart_flush(void)
{
}

int main(void)
{
    const uint64_t base = UINT64_C(0x10000000000);
    const uint64_t size = UINT64_C(48) << 30;
    assert(j616s_range_aligned(base, size));
    assert(!j616s_range_valid(base, 0));
    assert(!j616s_range_valid(UINT64_MAX - 0x3fff, 0x4000));
    assert(j616s_range_valid(UINT64_MAX - 0x4000, 0x4000));
    assert(!j616s_range_aligned(base + 1, 0x4000));
    assert(!j616s_range_aligned(base, 0x4001));
    assert(j616s_range_contains(base, size, base, size));
    assert(j616s_range_contains(base, size, base + size - 0x4000, 0x4000));
    assert(!j616s_range_contains(base, size, base + size, 0x4000));
    assert(!j616s_range_contains(base, size, base - 0x4000, 0x8000));
    assert(!j616s_range_contains(base, size, base, size + 0x4000));
    assert(!j616s_range_contains(base, size, UINT64_MAX - 0x3fff, 0x4000));
    assert(!j616s_range_contains(UINT64_MAX, 1, base, size));
    assert(j616s_range_overlaps(base, 0x8000, base + 0x4000, 0x8000));
    assert(j616s_range_overlaps(base, 0x8000, base, 0x4000));
    assert(!j616s_range_overlaps(base, 0x4000, base + 0x4000, 0x4000));
    assert(!j616s_range_overlaps(base, 0, base, size));
    assert(!j616s_range_overlaps(UINT64_MAX - 0x3fff, 0x4000, base, size));
    /* A selected firmware region may contain usable RAM; overlap is only evidence. */
    assert(j616s_range_overlaps(base, size, base + 0x4000, size - 0x8000));

    /* A too-large ADT property must not overwrite the destination or its canary. */
    u64 source[2] = {0x12345678, 0xdeadbeef};
    u64 destination[2] = {0, 0x87654321};
    property_value = source;
    property_length = sizeof(source);
    assert(!diag_get(0, "dram-base", destination, sizeof(destination[0])));
    assert(destination[0] == 0 && destination[1] == 0x87654321);
    property_length = sizeof(source[0]);
    assert(diag_get(0, "dram-base", destination, sizeof(destination[0])));
    assert(destination[0] == source[0] && destination[1] == 0x87654321);
    property_length = 4;
    assert(!diag_get(0, "dram-base", destination, sizeof(destination[0])));
    property_value = NULL;
    assert(!diag_get(0, "dram-base", destination, sizeof(destination[0])));
    int reads = property_reads;
    assert(!diag_get(-1, "dram-base", destination, sizeof(destination[0])));
    assert(property_reads == reads);

    property_value = "J616s";
    property_length = 6;
    assert(!strcmp(diag_string(0, "target-type"), "J616s"));
    property_length = 5; /* no trailing NUL in the property */
    assert(diag_string(0, "target-type") == NULL);
    property_value = "J\0bad";
    property_length = 6;
    assert(diag_string(0, "target-type") == NULL);
    property_length = 129; /* reject before inspecting bytes */
    assert(diag_string(0, "target-type") == NULL);
    property_length = 0;
    assert(diag_string(0, "target-type") == NULL);
    return 0;
}
