/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * md5.c — portable MD5 (RFC 1321) reference implementation, written in
 * plain C89 from the public algorithm description. No external
 * dependencies (no AmiSSL/libcurl/OpenSSL) so it links identically on all
 * four platforms — see docs/interactive-shell-plan.html section 18.1.
 */
#include "md5.h"
#include <string.h>
#include <stdio.h>

#define F(x, y, z) (((x) & (y)) | (~(x) & (z)))
#define G(x, y, z) (((x) & (z)) | ((y) & ~(z)))
#define H(x, y, z) ((x) ^ (y) ^ (z))
#define I(x, y, z) ((y) ^ ((x) | ~(z)))

#define ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

/* One MD5 round step: a = b + ROTL(a + f(b,c,d) + x + ac, s) */
#define FF(a, b, c, d, x, s, ac) \
    (a) += F((b), (c), (d)) + (x) + (unsigned int)(ac); \
    (a) = ROTL((a), (s)) + (b);
#define GG(a, b, c, d, x, s, ac) \
    (a) += G((b), (c), (d)) + (x) + (unsigned int)(ac); \
    (a) = ROTL((a), (s)) + (b);
#define HH(a, b, c, d, x, s, ac) \
    (a) += H((b), (c), (d)) + (x) + (unsigned int)(ac); \
    (a) = ROTL((a), (s)) + (b);
#define II(a, b, c, d, x, s, ac) \
    (a) += I((b), (c), (d)) + (x) + (unsigned int)(ac); \
    (a) = ROTL((a), (s)) + (b);

/* Byte-order-agnostic little-endian load/store — MD5 is defined in terms
 * of little-endian 32-bit words regardless of host endianness (matters on
 * the big-endian 68k Amiga). */
static unsigned int le32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static void store_le32(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}

static void md5_transform(unsigned int state[4], const unsigned char block[64])
{
    unsigned int a = state[0], b = state[1], c = state[2], d = state[3];
    unsigned int x[16];
    int i;

    for (i = 0; i < 16; i++) x[i] = le32(block + i * 4);

    /* Round 1 */
    FF(a, b, c, d, x[ 0],  7, 0xd76aa478U); FF(d, a, b, c, x[ 1], 12, 0xe8c7b756U);
    FF(c, d, a, b, x[ 2], 17, 0x242070dbU); FF(b, c, d, a, x[ 3], 22, 0xc1bdceeeU);
    FF(a, b, c, d, x[ 4],  7, 0xf57c0fafU); FF(d, a, b, c, x[ 5], 12, 0x4787c62aU);
    FF(c, d, a, b, x[ 6], 17, 0xa8304613U); FF(b, c, d, a, x[ 7], 22, 0xfd469501U);
    FF(a, b, c, d, x[ 8],  7, 0x698098d8U); FF(d, a, b, c, x[ 9], 12, 0x8b44f7afU);
    FF(c, d, a, b, x[10], 17, 0xffff5bb1U); FF(b, c, d, a, x[11], 22, 0x895cd7beU);
    FF(a, b, c, d, x[12],  7, 0x6b901122U); FF(d, a, b, c, x[13], 12, 0xfd987193U);
    FF(c, d, a, b, x[14], 17, 0xa679438eU); FF(b, c, d, a, x[15], 22, 0x49b40821U);

    /* Round 2 */
    GG(a, b, c, d, x[ 1],  5, 0xf61e2562U); GG(d, a, b, c, x[ 6],  9, 0xc040b340U);
    GG(c, d, a, b, x[11], 14, 0x265e5a51U); GG(b, c, d, a, x[ 0], 20, 0xe9b6c7aaU);
    GG(a, b, c, d, x[ 5],  5, 0xd62f105dU); GG(d, a, b, c, x[10],  9, 0x02441453U);
    GG(c, d, a, b, x[15], 14, 0xd8a1e681U); GG(b, c, d, a, x[ 4], 20, 0xe7d3fbc8U);
    GG(a, b, c, d, x[ 9],  5, 0x21e1cde6U); GG(d, a, b, c, x[14],  9, 0xc33707d6U);
    GG(c, d, a, b, x[ 3], 14, 0xf4d50d87U); GG(b, c, d, a, x[ 8], 20, 0x455a14edU);
    GG(a, b, c, d, x[13],  5, 0xa9e3e905U); GG(d, a, b, c, x[ 2],  9, 0xfcefa3f8U);
    GG(c, d, a, b, x[ 7], 14, 0x676f02d9U); GG(b, c, d, a, x[12], 20, 0x8d2a4c8aU);

    /* Round 3 */
    HH(a, b, c, d, x[ 5],  4, 0xfffa3942U); HH(d, a, b, c, x[ 8], 11, 0x8771f681U);
    HH(c, d, a, b, x[11], 16, 0x6d9d6122U); HH(b, c, d, a, x[14], 23, 0xfde5380cU);
    HH(a, b, c, d, x[ 1],  4, 0xa4beea44U); HH(d, a, b, c, x[ 4], 11, 0x4bdecfa9U);
    HH(c, d, a, b, x[ 7], 16, 0xf6bb4b60U); HH(b, c, d, a, x[10], 23, 0xbebfbc70U);
    HH(a, b, c, d, x[13],  4, 0x289b7ec6U); HH(d, a, b, c, x[ 0], 11, 0xeaa127faU);
    HH(c, d, a, b, x[ 3], 16, 0xd4ef3085U); HH(b, c, d, a, x[ 6], 23, 0x04881d05U);
    HH(a, b, c, d, x[ 9],  4, 0xd9d4d039U); HH(d, a, b, c, x[12], 11, 0xe6db99e5U);
    HH(c, d, a, b, x[15], 16, 0x1fa27cf8U); HH(b, c, d, a, x[ 2], 23, 0xc4ac5665U);

    /* Round 4 */
    II(a, b, c, d, x[ 0],  6, 0xf4292244U); II(d, a, b, c, x[ 7], 10, 0x432aff97U);
    II(c, d, a, b, x[14], 15, 0xab9423a7U); II(b, c, d, a, x[ 5], 21, 0xfc93a039U);
    II(a, b, c, d, x[12],  6, 0x655b59c3U); II(d, a, b, c, x[ 3], 10, 0x8f0ccc92U);
    II(c, d, a, b, x[10], 15, 0xffeff47dU); II(b, c, d, a, x[ 1], 21, 0x85845dd1U);
    II(a, b, c, d, x[ 8],  6, 0x6fa87e4fU); II(d, a, b, c, x[15], 10, 0xfe2ce6e0U);
    II(c, d, a, b, x[ 6], 15, 0xa3014314U); II(b, c, d, a, x[13], 21, 0x4e0811a1U);
    II(a, b, c, d, x[ 4],  6, 0xf7537e82U); II(d, a, b, c, x[11], 10, 0xbd3af235U);
    II(c, d, a, b, x[ 2], 15, 0x2ad7d2bbU); II(b, c, d, a, x[ 9], 21, 0xeb86d391U);

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
}

void md5_init(Md5Ctx *ctx)
{
    ctx->count[0] = ctx->count[1] = 0;
    ctx->state[0] = 0x67452301U;
    ctx->state[1] = 0xefcdab89U;
    ctx->state[2] = 0x98badcfeU;
    ctx->state[3] = 0x10325476U;
}

void md5_update(Md5Ctx *ctx, const unsigned char *data, size_t len)
{
    unsigned int idx = (unsigned int)((ctx->count[0] >> 3) & 0x3f);
    unsigned int bits_lo_before = ctx->count[0];

    ctx->count[0] += (unsigned int)(len << 3);
    if (ctx->count[0] < bits_lo_before) ctx->count[1]++; /* carry */
    ctx->count[1] += (unsigned int)(len >> 29);

    {
        size_t part = 64 - idx;
        size_t i;

        if (len >= part) {
            memcpy(ctx->buffer + idx, data, part);
            md5_transform(ctx->state, ctx->buffer);
            for (i = part; i + 64 <= len; i += 64)
                md5_transform(ctx->state, data + i);
            idx = 0;
        } else {
            i = 0;
        }
        memcpy(ctx->buffer + idx, data + i, len - i);
    }
}

void md5_final(Md5Ctx *ctx, unsigned char digest[16])
{
    static const unsigned char pad[64] = { 0x80 };
    unsigned char bits[8];
    unsigned int idx, pad_len;
    int i;

    store_le32(bits,     ctx->count[0]);
    store_le32(bits + 4, ctx->count[1]);

    idx = (unsigned int)((ctx->count[0] >> 3) & 0x3f);
    pad_len = (idx < 56) ? (56 - idx) : (120 - idx);
    md5_update(ctx, pad, pad_len);
    md5_update(ctx, bits, 8);

    for (i = 0; i < 4; i++) store_le32(digest + i * 4, ctx->state[i]);
}

void md5_to_hex(const unsigned char digest[16], char hex[33])
{
    static const char hexchars[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 16; i++) {
        hex[i * 2]     = hexchars[(digest[i] >> 4) & 0xf];
        hex[i * 2 + 1] = hexchars[digest[i] & 0xf];
    }
    hex[32] = '\0';
}

void md5_buffer_hex(const void *data, size_t len, char hex_out[33])
{
    Md5Ctx ctx;
    unsigned char digest[16];

    md5_init(&ctx);
    md5_update(&ctx, (const unsigned char *)data, len);
    md5_final(&ctx, digest);
    md5_to_hex(digest, hex_out);
}

int md5_file_hex(const char *path, char hex_out[33])
{
    Md5Ctx ctx;
    unsigned char digest[16];
    unsigned char block[8192];
    FILE *fp;
    size_t n;

    fp = fopen(path, "rb");
    if (!fp) return 0;

    md5_init(&ctx);
    while ((n = fread(block, 1, sizeof(block), fp)) > 0)
        md5_update(&ctx, block, n);
    fclose(fp);

    md5_final(&ctx, digest);
    md5_to_hex(digest, hex_out);
    return 1;
}
