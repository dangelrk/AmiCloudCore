/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef BASE64_H
#define BASE64_H

#include <stddef.h>

/* Returns a malloc'd null-terminated Base64 string; caller must free(). */
char *base64_encode(const unsigned char *in, size_t in_len);

#endif /* BASE64_H */
