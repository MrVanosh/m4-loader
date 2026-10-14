/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef J616S_SCREEN_H
#define J616S_SCREEN_H

#include "types.h"
#include "xnuboot.h"
#include "j616s_diag.h"

struct j616s_live_range {
    u64 start, size;
};

struct j616s_screen {
    volatile u32 *pixels;
    const u8 *font;
    u64 stride;
    unsigned width, height, cols, rows, col, row, depth;
    bool truncated;
};

/* No output or MMIO occurs in this validator. Each extent is checked before
 * multiplication, narrowing, pointer conversion, or writing the result. */
static inline bool j616s_screen_configure(struct j616s_screen *out,
                                         const struct boot_video *video,
                                         u64 dram, u64 dram_size, u64 vram, u64 vram_size,
                                         const struct j616s_live_range *live, unsigned count,
                                         const u8 *font, size_t font_size, unsigned scale)
{
    if (!out || !video || !font || !live || count != 3 || (scale != 1 && scale != 2))
        return false;
    unsigned fw = 8 * scale, fh = 16 * scale;
    unsigned depth = video->depth & 0xff;
    if ((depth != 30 && depth != 32) || !video->base || (video->base & 3) ||
        video->width > 16384 || video->height > 16384 ||
        video->width < 100 * fw + 64 || video->height < 42 * fh + 128 ||
        video->stride > 65536 || (video->stride & 3) ||
        video->stride < video->width * 4 || font_size < 95 * fw * fh)
        return false;
    u64 bytes = video->stride * video->height;
    if (video->base != vram || !j616s_range_contains(dram, dram_size, vram, vram_size) ||
        !j616s_range_contains(vram, vram_size, video->base, bytes))
        return false;
    for (unsigned i = 0; i < count; i++) {
        if (!j616s_range_contains(dram, dram_size, live[i].start, live[i].size) ||
            j616s_range_overlaps(video->base, bytes, live[i].start, live[i].size))
            return false;
    }
    *out = (struct j616s_screen){
        .pixels = (volatile u32 *)(uintptr_t)video->base,
        .font = font, .stride = video->stride / 4,
        .width = fw, .height = fh,
        .cols = (video->width - 64) / fw,
        /* Leave the last row for an explicit truncation notice. */
        .rows = (video->height - 128) / fh - 1,
        .depth = depth,
    };
    return true;
}

static inline void j616s_screen_char(struct j616s_screen *s, unsigned char c)
{
    if (!s->pixels)
        return;
    if (c == '\r') {
        s->col = 0;
        return;
    }
    if (c == '\n') {
        s->col = 0;
        if (s->row < s->rows)
            s->row++;
        return;
    }
    if (s->col == s->cols) {
        s->col = 0;
        if (s->row < s->rows)
            s->row++;
    }
    if (s->row >= s->rows) {
        s->truncated = true;
        return;
    }
    if (c < 32 || c > 126)
        c = '?';
    unsigned x0 = 32 + s->col * s->width, y0 = 64 + s->row * s->height;
    const u8 *glyph = s->font + (c - 32) * s->width * s->height;
    for (unsigned y = 0; y < s->height; y++) {
        for (unsigned x = 0; x < s->width; x++) {
            u32 v = glyph[y * s->width + x];
            u32 pixel = s->depth == 32 ? v | (v << 8) | (v << 16) :
                                         (v << 2) | (v << 12) | (v << 22);
            s->pixels[(y0 + y) * s->stride + x0 + x] = pixel;
        }
    }
    s->col++;
}

unsigned j616s_screen_columns(void);
void j616s_screen_puts(const char *s);
int j616s_screen_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void j616s_screen_flush(void);
void j616s_screen_entry(void) __attribute__((noreturn));

#endif
