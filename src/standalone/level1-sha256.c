/* SPDX-License-Identifier: MIT */
/* Compact SHA-256 and CRC-32 (rolled loops: the loader keeps this in a small
 * executable section and runs it inside a cached window). */
#include "level1.h"

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void block(uint32_t h[8], const uint8_t *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, hh, t1, t2;
    for (unsigned i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (unsigned i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; hh = h[7];
    for (unsigned i = 0; i < 64; i++) {
        t1 = hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void l1_sha_init(struct l1_sha *s)
{
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    for (unsigned i = 0; i < 8; i++) s->h[i] = iv[i];
    s->bytes = 0;
    s->fill = 0;
}

void l1_sha_update(struct l1_sha *s, const void *data, size_t n)
{
    const uint8_t *p = data;
    s->bytes += n;
    if (s->fill) {
        while (n && s->fill < 64) { s->buf[s->fill++] = *p++; n--; }
        if (s->fill < 64) return;
        block(s->h, s->buf);
        s->fill = 0;
    }
    while (n >= 64) { block(s->h, p); p += 64; n -= 64; }
    while (n) { s->buf[s->fill++] = *p++; n--; }
}

void l1_sha_final(struct l1_sha *s, uint8_t out[32])
{
    uint64_t bits = s->bytes * 8;
    static const uint8_t pad80 = 0x80, zero = 0;
    uint64_t total = s->bytes;
    l1_sha_update(s, &pad80, 1);
    while (s->fill != 56) l1_sha_update(s, &zero, 1);
    uint8_t len[8];
    for (unsigned i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    l1_sha_update(s, len, 8);
    s->bytes = total;
    for (unsigned i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(s->h[i] >> 24); out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8); out[4 * i + 3] = (uint8_t)s->h[i];
    }
}

void l1_sha_hex(const uint8_t in[32], char out[65])
{
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; i++) { out[2 * i] = hex[in[i] >> 4]; out[2 * i + 1] = hex[in[i] & 15]; }
    out[64] = 0;
}

uint32_t l1_crc32(const void *data, size_t n)
{
    const uint8_t *p = data;
    uint32_t c = 0xffffffffu;
    while (n--) {
        c ^= *p++;
        for (unsigned k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
    }
    return ~c;
}

/* CRC-32 over the 45 MB the loader verifies each boot: the ARMv8 CRC32 instructions (general registers only, 8
 * bytes per instruction) where the build has them, else the bitwise loop. Same value as zlib.crc32. */
#if defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
uint32_t l1_crc32_fast(const void *data, size_t n)
{
    const uint8_t *p = data;
    uint32_t c = 0xffffffffu;
    while (n && ((uintptr_t)p & 7)) { c = __crc32b(c, *p++); n--; }
    const uint64_t *q = (const uint64_t *)p;
    while (n >= 32) {
        c = __crc32d(c, q[0]); c = __crc32d(c, q[1]); c = __crc32d(c, q[2]); c = __crc32d(c, q[3]);
        q += 4; n -= 32;
    }
    while (n >= 8) { c = __crc32d(c, *q++); n -= 8; }
    p = (const uint8_t *)q;
    while (n--) c = __crc32b(c, *p++);
    return ~c;
}
#else
uint32_t l1_crc32_fast(const void *data, size_t n) { return l1_crc32(data, n); }
#endif
