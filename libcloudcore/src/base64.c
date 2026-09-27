/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#include "base64.h"
#include <stdlib.h>

static const char b64table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *base64_encode(const unsigned char *in, size_t in_len)
{
    size_t out_len = 4 * ((in_len + 2) / 3) + 1;
    char  *out     = malloc(out_len);
    size_t i, o;

    if (!out) return NULL;

    for (i = 0, o = 0; i < in_len; i += 3) {
        unsigned int a = in[i];
        unsigned int b = (i + 1 < in_len) ? in[i + 1] : 0;
        unsigned int c = (i + 2 < in_len) ? in[i + 2] : 0;

        out[o++] = b64table[a >> 2];
        out[o++] = b64table[((a & 3) << 4) | (b >> 4)];
        out[o++] = (i + 1 < in_len) ? b64table[((b & 0xF) << 2) | (c >> 6)] : '=';
        out[o++] = (i + 2 < in_len) ? b64table[c & 0x3F] : '=';
    }
    out[o] = '\0';
    return out;
}
