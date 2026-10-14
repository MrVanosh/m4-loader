/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/j616s_screen.h"

static u8 font[95 * 16 * 32];

int main(void)
{
    const size_t width = 1024, height = 832, stride = 1040, guard = 64;
    const size_t words = stride * height;
    u32 *allocation = malloc((words + 2 * guard) * sizeof(u32));
    assert(allocation);
    for (size_t i = 0; i < words + 2 * guard; i++)
        allocation[i] = 0x12345678;
    memset(font, 255, sizeof(font));
    u64 base = (uintptr_t)(allocation + guard);
    u64 bytes = words * sizeof(u32);
    u64 dram = base - 0x100000, dram_size = bytes + 0x200000;
    struct boot_video video = {base, 0, stride * 4, width, height, 32};
    struct j616s_live_range live[3] = {
        {dram + 0x1000, 0x2000}, {dram + 0x4000, 0x1000}, {dram + 0x6000, 0x1000},
    };
    struct j616s_screen s = {0};
#define CONFIG(out, v, vr, vs, ranges, n, fs, scale) \
    j616s_screen_configure(out, v, dram, dram_size, vr, vs, ranges, n, font, fs, scale)
    assert(CONFIG(&s, &video, base, bytes, live, 3, sizeof(font), 1));
    assert(s.cols == 120 && s.rows == 43 && s.stride == stride);
    j616s_screen_char(&s, 'A');
    for (size_t y = 0; y < height; y++) {
        for (size_t x = 0; x < stride; x++) {
            u32 expected = y >= 64 && y < 80 && x >= 32 && x < 40 ?
                           0xffffff : 0x12345678;
            assert(allocation[guard + y * stride + x] == expected);
        }
    }
    /* Exercise 10-bit pixels, non-ASCII fallback, wrapping and bounded overflow. */
    video.depth = 30 | (1u << 16);
    assert(CONFIG(&s, &video, base, bytes, live, 3, sizeof(font), 1));
    j616s_screen_char(&s, 255);
    assert(allocation[guard + 64 * stride + 32] == 0x3fcff3fc);
    j616s_screen_char(&s, '\r');
    assert(s.col == 0 && s.row == 0);
    j616s_screen_char(&s, '\n');
    assert(s.col == 0 && s.row == 1);
    for (unsigned i = 0; i < s.cols * s.rows + 100; i++)
        j616s_screen_char(&s, 'Z');
    assert(s.truncated && s.row == s.rows);
    for (size_t y = 0; y < height; y++)
        for (size_t x = width; x < stride; x++)
            assert(allocation[guard + y * stride + x] == 0x12345678);
    for (size_t i = 0; i < guard; i++) {
        assert(allocation[i] == 0x12345678);
        assert(allocation[guard + words + i] == 0x12345678);
    }
    /* Failed configurations must not leave a partially usable output context. */
    struct j616s_screen unchanged;
    memset(&unchanged, 0x5a, sizeof(unchanged));
    struct boot_video good = video;
#define REJECT(expr) do { \
    memcpy(&s, &unchanged, sizeof(s)); \
    assert(!(expr)); \
    assert(!memcmp(&s, &unchanged, sizeof(s))); \
} while (0)
#define BAD_FIELD(field, value) do { \
    video = good; video.field = (value); \
    REJECT(CONFIG(&s, &video, base, bytes, live, 3, sizeof(font), 1)); \
} while (0)
    BAD_FIELD(base, 0);
    BAD_FIELD(base, base + 1);
    BAD_FIELD(base, UINT64_MAX - 3);
    BAD_FIELD(width, 0);
    BAD_FIELD(width, UINT64_MAX);
    BAD_FIELD(height, 0);
    BAD_FIELD(height, UINT64_MAX);
    BAD_FIELD(stride, 1);
    BAD_FIELD(stride, width * 4 - 4);
    BAD_FIELD(stride, UINT64_MAX);
    BAD_FIELD(depth, 16);
    video = good;
    REJECT(CONFIG(&s, &video, base, bytes - 1, live, 3, sizeof(font), 1));
    REJECT(CONFIG(&s, &video, base - 4, bytes, live, 3, sizeof(font), 1));
    REJECT(CONFIG(&s, &video, base, UINT64_MAX, live, 3, sizeof(font), 1));
    REJECT(CONFIG(&s, &video, base, bytes, live, 2, sizeof(font), 1));
    REJECT(CONFIG(&s, &video, base, bytes, live, 3, 95 * 8 * 16 - 1, 1));
    REJECT(CONFIG(&s, &video, base, bytes, live, 3, sizeof(font), 0));
    REJECT(CONFIG(&s, &video, base, bytes, live, 3, sizeof(font), 3));
    for (unsigned i = 0; i < 3; i++) {
        struct j616s_live_range saved = live[i];
        live[i] = (struct j616s_live_range){base, 4};
        REJECT(CONFIG(&s, &video, base, bytes, live, 3, sizeof(font), 1));
        live[i] = (struct j616s_live_range){UINT64_MAX - 3, 8};
        REJECT(CONFIG(&s, &video, base, bytes, live, 3, sizeof(font), 1));
        live[i] = saved;
    }
    /* Small-buffer validation and format selection do not touch framebuffer. */
    video = (struct boot_video){base, 0, 3456 * 4, 3456, 2234, 30};
    u64 big_bytes = video.stride * video.height;
    struct j616s_screen large;
    assert(j616s_screen_configure(&large, &video, dram, big_bytes + 0x200000,
        base, big_bytes, live, 3, font, sizeof(font), 2));
    assert(large.width == 16 && large.height == 32);
    /* No stores via this synthetic larger framebuffer: only the geometry is tested. */
    free(allocation);
    puts("screen bounds, live-range rejection, pixels, stride padding, wrapping: PASS");
    return 0;
}
