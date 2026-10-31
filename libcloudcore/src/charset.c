/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 *
 * charset.c -- see charset.h. UTF-8 decoding/encoding and NFC are plain
 * C (also used by the host unit tests); the AmigaOS part talks to
 * codesets.library directly through its jump table (offsets from the
 * library's codesets_lib.fd), so the build needs no foreign headers.
 */
#include "charset.h"
#include "amicloud.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nfc_table.h"

/* ------------------------------------------------------------------ */
/* UTF-8                                                                */
/* ------------------------------------------------------------------ */

/* Decodes one code point at *p (advanced). Invalid or overlong
 * sequences yield -1 and skip one byte. */
static long utf8_next(const unsigned char **p)
{
    const unsigned char *s = *p;
    long cp;
    int n, i;
    if (s[0] < 0x80) { *p = s + 1; return s[0]; }
    if ((s[0] & 0xE0) == 0xC0)      { cp = s[0] & 0x1F; n = 1; }
    else if ((s[0] & 0xF0) == 0xE0) { cp = s[0] & 0x0F; n = 2; }
    else if ((s[0] & 0xF8) == 0xF0) { cp = s[0] & 0x07; n = 3; }
    else { *p = s + 1; return -1; }
    for (i = 1; i <= n; i++) {
        if ((s[i] & 0xC0) != 0x80) { *p = s + 1; return -1; }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) ||
        cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        *p = s + 1;
        return -1;
    }
    *p = s + n + 1;
    return cp;
}

/* Appends cp as UTF-8; 0 if it does not fit (out stays terminated). */
static int utf8_put(char *out, size_t outsz, size_t *o, long cp)
{
    unsigned char b[4];
    size_t n, i;
    if (cp < 0x80)         { b[0] = (unsigned char)cp; n = 1; }
    else if (cp < 0x800)   { b[0] = (unsigned char)(0xC0 | (cp >> 6));
                             b[1] = (unsigned char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) { b[0] = (unsigned char)(0xE0 | (cp >> 12));
                             b[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
                             b[2] = (unsigned char)(0x80 | (cp & 0x3F)); n = 3; }
    else                   { b[0] = (unsigned char)(0xF0 | (cp >> 18));
                             b[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
                             b[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
                             b[3] = (unsigned char)(0x80 | (cp & 0x3F)); n = 4; }
    if (*o + n + 1 > outsz) { out[*o] = '\0'; return 0; }
    for (i = 0; i < n; i++) out[(*o)++] = (char)b[i];
    out[*o] = '\0';
    return 1;
}

static long nfc_compose(long base, long mark)
{
    int lo = 0, hi = (int)(sizeof(nfc_pairs) / sizeof(nfc_pairs[0])) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        long b = nfc_pairs[mid][0], m = nfc_pairs[mid][1];
        if (b == base && m == mark) return nfc_pairs[mid][2];
        if (b < base || (b == base && m < mark)) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

/* NFC over valid UTF-8; invalid bytes become CS_REPLACEMENT and set
 * *bad. Returns 0 if truncated. */
static int nfc_clean(const char *in, char *out, size_t outsz, int *bad)
{
    const unsigned char *p = (const unsigned char *)in;
    size_t o = 0;
    long pending = -2;   /* code point waiting for a possible mark */
    out[0] = '\0';
    while (*p) {
        long cp = utf8_next(&p);
        if (cp < 0) { cp = CS_REPLACEMENT; if (bad) *bad = 1; }
        if (pending >= 0 && cp >= 0x300 && cp <= 0x36F) {
            long c = nfc_compose(pending, cp);
            if (c >= 0) { pending = c; continue; }
        }
        if (pending >= 0 && !utf8_put(out, outsz, &o, pending)) return 0;
        pending = cp;
    }
    if (pending >= 0 && !utf8_put(out, outsz, &o, pending)) return 0;
    return 1;
}

int cs_nfc(const char *in, char *out, size_t outsz)
{
    if (!outsz) return 0;
    return nfc_clean(in, out, outsz, NULL);
}

/* ------------------------------------------------------------------ */
/* Modes                                                                */
/* ------------------------------------------------------------------ */

enum { M_UTF8, M_LATIN1, M_ASCII, M_CODESETS };

#ifdef PLATFORM_AMIGA
static int g_mode = M_LATIN1;
#else
static int g_mode = M_UTF8;
#endif

/* single byte codesets: byte -> code point and back */
static int sb_from_utf8(const char *utf8, char *out, size_t outsz, long maxcp)
{
    char tmp[AC_MAX_PATH];
    const unsigned char *p;
    size_t o = 0;
    int bad = 0, exact;
    if (!outsz) return 0;
    if (!nfc_clean(utf8, tmp, sizeof(tmp), &bad)) bad = 1;
    exact = !bad;
    p = (const unsigned char *)tmp;
    while (*p) {
        long cp = utf8_next(&p);
        if (o + 2 > outsz) { out[o] = '\0'; return 0; }
        if (cp < 0 || cp > maxcp) { out[o++] = CS_REPLACEMENT; exact = 0; }
        else out[o++] = (char)cp;
    }
    out[o] = '\0';
    return exact;
}

static int sb_to_utf8(const char *local, char *out, size_t outsz)
{
    const unsigned char *p = (const unsigned char *)local;
    size_t o = 0;
    if (!outsz) return 0;
    out[0] = '\0';
    for (; *p; p++)
        if (!utf8_put(out, outsz, &o, *p)) return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* codesets.library (AmigaOS)                                           */
/* ------------------------------------------------------------------ */
#ifdef PLATFORM_AMIGA
#include <exec/types.h>
#include <exec/libraries.h>
#include <utility/tagitem.h>
#include <proto/exec.h>

#define CSTAG(n)              ((ULONG)0xfec901f4UL + (n))
#define CSA_SourceLen         CSTAG(1)
#define CSA_Source            CSTAG(2)
#define CSA_Dest              CSTAG(3)
#define CSA_DestLen           CSTAG(4)
#define CSA_SourceCodeset     CSTAG(7)
#define CSA_AllocIfNeeded     CSTAG(10)
#define CSA_FallbackToDefault CSTAG(12)
#define CSA_DestCodeset       CSTAG(13)
#define CSA_ErrPtr            CSTAG(19)

/* first fields of codesets.library's struct codeset */
struct cs_codeset { struct MinNode node; char *name; };

/* jump table, codesets_lib.fd: bias 30, one private slot */
APTR   __csFindA(__reg("a6") struct Library *, __reg("a0") STRPTR name,
                 __reg("a1") struct TagItem *attrs) = "\tjsr\t-102(a6)";
STRPTR __csUTF8ToStrA(__reg("a6") struct Library *, __reg("a0") struct TagItem *attrs) = "\tjsr\t-120(a6)";
APTR   __csUTF8CreateA(__reg("a6") struct Library *, __reg("a0") struct TagItem *attrs) = "\tjsr\t-126(a6)";

static struct Library    *g_csbase;
static struct cs_codeset *g_cs;

static int cl_from_utf8(const char *utf8, char *out, size_t outsz)
{
    char tmp[AC_MAX_PATH], back[AC_MAX_PATH];
    int bad = 0, errs = 0;
    size_t i;
    struct TagItem t[7];
    if (!outsz) return 0;
    out[0] = '\0';
    if (!nfc_clean(utf8, tmp, sizeof(tmp), &bad)) bad = 1;
    if (!tmp[0]) return !bad;
    t[0].ti_Tag = CSA_Source;        t[0].ti_Data = (ULONG)tmp;
    t[1].ti_Tag = CSA_Dest;          t[1].ti_Data = (ULONG)out;
    t[2].ti_Tag = CSA_DestLen;       t[2].ti_Data = (ULONG)outsz;
    t[3].ti_Tag = CSA_DestCodeset;   t[3].ti_Data = (ULONG)g_cs;
    t[4].ti_Tag = CSA_AllocIfNeeded; t[4].ti_Data = FALSE;
    t[5].ti_Tag = CSA_ErrPtr;        t[5].ti_Data = (ULONG)&errs;
    t[6].ti_Tag = TAG_DONE;          t[6].ti_Data = 0;
    if (!__csUTF8ToStrA(g_csbase, t)) { out[0] = '\0'; return 0; }
    out[outsz - 1] = '\0';
    /* the library marks what it cannot map with '?', a wildcard in
     * AmigaDOS; servers never send '?' in a name */
    for (i = 0; out[i]; i++)
        if (out[i] == '?') { out[i] = CS_REPLACEMENT; bad = 1; }
    if (errs) bad = 1;
    if (!bad && (!cs_to_utf8(out, back, sizeof(back)) || strcmp(back, tmp) != 0)) bad = 1;
    return !bad;
}

static int cl_to_utf8(const char *local, char *out, size_t outsz)
{
    struct TagItem t[6];
    if (!outsz) return 0;
    out[0] = '\0';
    if (!local[0]) return 1;
    t[0].ti_Tag = CSA_Source;        t[0].ti_Data = (ULONG)local;
    t[1].ti_Tag = CSA_SourceCodeset; t[1].ti_Data = (ULONG)g_cs;
    t[2].ti_Tag = CSA_Dest;          t[2].ti_Data = (ULONG)out;
    t[3].ti_Tag = CSA_DestLen;       t[3].ti_Data = (ULONG)outsz;
    t[4].ti_Tag = CSA_AllocIfNeeded; t[4].ti_Data = FALSE;
    t[5].ti_Tag = TAG_DONE;          t[5].ti_Data = 0;
    if (!__csUTF8CreateA(g_csbase, t)) { out[0] = '\0'; return 0; }
    out[outsz - 1] = '\0';
    return 1;
}

int cs_open(const char *override)
{
    struct TagItem t[2];
    cs_close();
    g_csbase = OpenLibrary((STRPTR)"codesets.library", 6);
    if (!g_csbase) { g_mode = M_LATIN1; return 0; }
    t[0].ti_Tag = CSA_FallbackToDefault; t[0].ti_Data = FALSE;
    t[1].ti_Tag = TAG_DONE;              t[1].ti_Data = 0;
    g_cs = (struct cs_codeset *)__csFindA(g_csbase,
                                          (STRPTR)(override && override[0] ? override : NULL),
                                          override && override[0] ? t : NULL);
    if (!g_cs) { cs_close(); g_mode = M_LATIN1; return 0; }
    g_mode = M_CODESETS;
    return 1;
}

void cs_close(void)
{
    if (g_csbase) CloseLibrary(g_csbase);
    g_csbase = NULL;
    g_cs = NULL;
    g_mode = M_LATIN1;
}
#else
int cs_open(const char *override) { (void)override; g_mode = M_UTF8; return 1; }
void cs_close(void) { }
#endif

void cs_test_mode(int mode)
{
#ifdef PLATFORM_AMIGA
    g_mode = mode == CS_TEST_ASCII ? M_ASCII : M_LATIN1;
#else
    g_mode = mode == CS_TEST_ASCII ? M_ASCII : mode == CS_TEST_LATIN1 ? M_LATIN1 : M_UTF8;
#endif
}

const char *cs_name(void)
{
    switch (g_mode) {
    case M_UTF8:   return "UTF-8";
    case M_ASCII:  return "US-ASCII";
#ifdef PLATFORM_AMIGA
    case M_CODESETS: return g_cs && g_cs->name ? g_cs->name : "?";
#endif
    default:       return "ISO-8859-1";
    }
}

int cs_to_utf8(const char *local, char *out, size_t outsz)
{
    switch (g_mode) {
    case M_UTF8:   return cs_nfc(local, out, outsz);
    case M_ASCII:  if (!outsz) return 0;
                   strncpy(out, local, outsz - 1); out[outsz - 1] = '\0';
                   return strlen(local) < outsz;
#ifdef PLATFORM_AMIGA
    case M_CODESETS: return cl_to_utf8(local, out, outsz);
#endif
    default:       return sb_to_utf8(local, out, outsz);
    }
}

int cs_from_utf8(const char *utf8, char *out, size_t outsz)
{
    char back[AC_MAX_PATH];
    int r;
    switch (g_mode) {
    case M_UTF8:   r = cs_nfc(utf8, out, outsz); break;
    case M_ASCII:  r = sb_from_utf8(utf8, out, outsz, 0x7F); break;
#ifdef PLATFORM_AMIGA
    case M_CODESETS: r = cl_from_utf8(utf8, out, outsz); break;
#endif
    default:       r = sb_from_utf8(utf8, out, outsz, 0xFF); break;
    }
    /* exact only if the way back gives the very same bytes -- a
     * decomposed name (NFD) comes back composed and is not exact */
    return r && cs_to_utf8(out, back, sizeof(back)) && strcmp(back, utf8) == 0;
}

/* ------------------------------------------------------------------ */
/* Remembered lossy names (handlers)                                    */
/* ------------------------------------------------------------------ */
#define CS_REMEMBER 128

static struct { char *l; char *u; } g_mem[CS_REMEMBER];
static int g_mem_next;

static char *cs_dup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static void join_path(char *out, size_t sz, const char *dir, const char *name)
{
    size_t dl = strlen(dir);
    if (dl && dir[dl - 1] == '/') snprintf(out, sz, "%s%s", dir, name);
    else snprintf(out, sz, "%s/%s", dir, name);
}

static const char *mem_find(const char *l)
{
    int i;
    for (i = 0; i < CS_REMEMBER; i++)
        if (g_mem[i].l && strcmp(g_mem[i].l, l) == 0) return g_mem[i].u;
    return NULL;
}

int cs_list_name(const char *local_dir, const char *utf8_dir, const char *utf8_name,
                 char *out, size_t outsz)
{
    char l[AC_MAX_PATH], u[AC_MAX_PATH];
    int exact = cs_from_utf8(utf8_name, out, outsz);
    if (exact) return 1;
    join_path(l, sizeof(l), local_dir, out);
    join_path(u, sizeof(u), utf8_dir, utf8_name);
    if (!mem_find(l)) {
        free(g_mem[g_mem_next].l);
        free(g_mem[g_mem_next].u);
        g_mem[g_mem_next].l = cs_dup(l);
        g_mem[g_mem_next].u = cs_dup(u);
        g_mem_next = (g_mem_next + 1) % CS_REMEMBER;
    }
    return 0;
}

void cs_path_to_utf8(const char *local, char *out, size_t outsz)
{
    char lp[AC_MAX_PATH], comp[AC_MAX_PATH], uc[AC_MAX_PATH];
    const char *p = local;
    size_t ll = 0;
    if (!outsz) return;
    out[0] = '\0';
    lp[0] = '\0';
    if (*p == '/') { strncpy(out, "/", outsz - 1); out[outsz - 1] = '\0'; }
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t n = slash ? (size_t)(slash - p) : strlen(p);
        const char *mapped;
        if (n) {
            if (n >= sizeof(comp)) n = sizeof(comp) - 1;
            memcpy(comp, p, n); comp[n] = '\0';
            snprintf(lp + ll, sizeof(lp) - ll, "/%s", comp);
            ll = strlen(lp);
            mapped = mem_find(lp);
            if (mapped) {
                strncpy(out, mapped, outsz - 1);
                out[outsz - 1] = '\0';
            } else {
                size_t ol = strlen(out);
                cs_to_utf8(comp, uc, sizeof(uc));
                snprintf(out + ol, outsz - ol, "%s%s", (ol && out[ol - 1] == '/') ? "" : "/", uc);
            }
        }
        p += n;
        if (*p == '/') p++;
    }
    if (!out[0] && *local != '/') cs_to_utf8(local, out, outsz);
}

