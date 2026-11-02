/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef CLOUDCORE_CHARSET_H
#define CLOUDCORE_CHARSET_H

/*
 * charset.h -- names between UTF-8 (servers, sync kernel, everything on
 * the wire) and the local codeset of the file system.
 *
 *   PC (macOS, Linux, Windows): the local side is UTF-8 too. cs_to_utf8()
 *       copies, cs_from_utf8() only composes decomposed characters (NFC,
 *       macOS writes "a" + U+0308 for "ä").
 *   AmigaOS: the local codeset is the system's (ISO-8859-1 in Western
 *       Europe, ISO-8859-2/AmigaPL in Poland, Amiga-1251 ...), converted
 *       through codesets.library 6+. Without the library a built-in
 *       ISO-8859-1 conversion is used and cs_open() returns 0 so the
 *       caller can say so.
 *
 * cs_from_utf8() can lose information: a character without an equivalent
 * in the local codeset (EUR in ISO-8859-1, emoji ...) becomes '_'. The
 * result says so; callers that need the original back (sync, handler)
 * keep their own mapping. cs_to_utf8() never loses anything.
 *
 * Not thread-safe; open once per process (the handler: per worker).
 */

#include <stddef.h>

#define CS_REPLACEMENT '_'   /* never a wildcard or path separator anywhere */

/* Selects the local codeset. override: a codesets.library codeset name
 * ("ISO-8859-2", "AmigaPL") or NULL/"" for the system default. Returns 1
 * if the local codeset is fully supported (PC, or codesets.library found
 * the codeset), 0 if the built-in ISO-8859-1 fallback is used. Calling it
 * again re-selects. */
int  cs_open(const char *override);
void cs_close(void);

/* Name of the local codeset, e.g. "UTF-8", "ISO-8859-1" -- part of the
 * sync baseline's identity (a different codeset maps names differently). */
const char *cs_name(void);

/* Local name -> UTF-8 (NFC). 1 ok, 0 output truncated. */
int cs_to_utf8(const char *local, char *out, size_t outsz);

/* UTF-8 name -> local codeset, NFC-composed first. Returns 1 only if
 * cs_to_utf8() of the result gives back exactly utf8; 0 if anything was
 * replaced, truncated, invalid, or the name was decomposed (NFD). */
int cs_from_utf8(const char *utf8, char *out, size_t outsz);

/* Composes base + combining mark pairs (U+00C0..U+024F) to NFC. out may
 * not alias in. Always NUL-terminates; 0 if truncated. */
int cs_nfc(const char *in, char *out, size_t outsz);

/* For the handlers: a server listing name (UTF-8) as shown in a
 * directory listing, in the local codeset. If the conversion is lossy
 * ("Preis_.txt" for "Preis\xE2\x82\xAC.txt"), the pair is remembered
 * (local_dir/out -> utf8_dir/utf8_name, last 128 such names), so that
 * cs_path_to_utf8() finds the real server name again when the shown name
 * is opened. local_dir and utf8_dir are the directory's path in both
 * forms ("/" for the root). Returns like cs_from_utf8(). */
int cs_list_name(const char *local_dir, const char *utf8_dir, const char *utf8_name,
                 char *out, size_t outsz);

/* A local path ("/dir/file", as a handler builds it) for the server:
 * component by component through the remembered lossy names, else
 * cs_to_utf8(). */
void cs_path_to_utf8(const char *local, char *out, size_t outsz);

/* Names folded for "would these be one file here?": ASCII and Latin-1
 * letters (U+00C0..U+00DE except U+00D7) to lower case. AmigaOS (the
 * international mode of FFS, PFS and SFS), macOS and Windows treat
 * "Äpfel" and "äpfel" as one file, not only "abc" and "ABC".
 * cs_fold_utf8() takes UTF-8; cs_fold_local() takes the local codeset
 * (single bytes on the Amiga, folded like ISO-8859-1 -- for another
 * codeset that may join a pair too many, never one too few; UTF-8 on
 * the PC). Nothing else changes, so a lossy local name ("Preis_.txt")
 * folds like any other. out may alias in. Always NUL-terminates. */
void cs_fold_utf8(const char *in, char *out, size_t outsz);
void cs_fold_local(const char *in, char *out, size_t outsz);

/* For the handlers: a whole server listing, n names (UTF-8) into
 * out + i * stride (outsz bytes each), each through cs_list_name().
 * Names that would be one file on the Amiga (equal after
 * cs_fold_local(): case, or a lossy name meeting another one) cannot all
 * be shown under their own name -- every lookup would find the first.
 * The bytewise smallest server name of such a group keeps its name, the
 * others are shown as "name_2.ext", "name_3.ext" ... (the first number
 * free in this listing; before the last '.', so "x_2.info" is the icon
 * of "x_2"; '_' as for lossy names, '~' would be an AmigaDOS pattern)
 * and remembered like a lossy name, so cs_path_to_utf8()
 * leads to the right file. Returns how many were renamed; -1 if out of
 * memory (the names are then as cs_list_name() gives them). */
int cs_list_dir(const char *local_dir, const char *utf8_dir,
                const char *const *utf8_names, int n,
                char *out, size_t stride, size_t outsz);

/* For the host unit tests only: make the "local" codeset ASCII (every
 * non-ASCII character is lossy) or ISO-8859-1 (built-in), or back to
 * the platform default with mode 0. */
#define CS_TEST_DEFAULT 0
#define CS_TEST_ASCII   1
#define CS_TEST_LATIN1  2
void cs_test_mode(int mode);

#endif /* CLOUDCORE_CHARSET_H */
