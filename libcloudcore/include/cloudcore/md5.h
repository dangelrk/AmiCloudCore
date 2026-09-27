/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef AC_MD5_H
#define AC_MD5_H

/*
 * md5.h — portable MD5 (RFC 1321), C89, no external dependencies.
 * Used for transfer verification (get/put) and as the comparison basis
 * for `sync` — see AmiCloud docs/design/interactive-shell-plan.html section 18.
 *
 * Streaming API so large files can be hashed block-wise (fixed buffer)
 * instead of loaded whole into RAM — important on the Amiga.
 */

#include <stddef.h>

typedef struct {
    unsigned int  state[4];
    unsigned int  count[2]; /* bit count, lo/hi */
    unsigned char buffer[64];
} Md5Ctx;

void md5_init(Md5Ctx *ctx);
void md5_update(Md5Ctx *ctx, const unsigned char *data, size_t len);
/* Writes the 16 raw digest bytes to digest[16] and finalises ctx (ctx must
 * not be reused afterwards without calling md5_init() again). */
void md5_final(Md5Ctx *ctx, unsigned char digest[16]);

/* digest[16] -> 32 lowercase hex chars + NUL into hex[33]. */
void md5_to_hex(const unsigned char digest[16], char hex[33]);

/* Convenience: hash an in-RAM buffer (e.g. an upload body already read
 * into memory by cmd_put()). hex_out must be at least 33 bytes. */
void md5_buffer_hex(const void *data, size_t len, char hex_out[33]);

/* Convenience: hash a local file, streaming in fixed-size blocks (works
 * for files larger than available RAM). Returns 1 on success, 0 if the
 * file could not be opened. hex_out must be at least 33 bytes. */
int md5_file_hex(const char *path, char hex_out[33]);

#endif /* AC_MD5_H */
