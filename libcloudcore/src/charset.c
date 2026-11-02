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

static int g_mem_used;   /* entries ever set: skips the scan while 0 */

static int mem_index(const char *l)
{
    int i;
    if (!g_mem_used) return -1;
    for (i = 0; i < CS_REMEMBER; i++)
        if (g_mem[i].l && strcmp(g_mem[i].l, l) == 0) return i;
    return -1;
}

static const char *mem_find(const char *l)
{
    int i = mem_index(l);
    return i < 0 ? NULL : g_mem[i].u;
}

/* l now stands for u -- also when it stood for another name before (a
 * "_2" name moves to another file once the listing changes) */
static void mem_put(const char *l, const char *u)
{
    int i = mem_index(l);
    if (i >= 0) {
        char *d;
        if (strcmp(g_mem[i].u, u) == 0) return;
        if (!(d = cs_dup(u))) return;
        free(g_mem[i].u);
        g_mem[i].u = d;
        return;
    }
    /* a free place first (cs_list_dir() frees a directory's pairs and
     * sets them again -- that must not push other directories' pairs
     * out of the ring), else the oldest */
    for (i = 0; i < CS_REMEMBER && g_mem[i].l; i++) ;
    if (i == CS_REMEMBER) {
        i = g_mem_next;
        g_mem_next = (g_mem_next + 1) % CS_REMEMBER;
        free(g_mem[i].l);
        free(g_mem[i].u);
    }
    g_mem[i].l = cs_dup(l);
    g_mem[i].u = cs_dup(u);
    if (!g_mem[i].l || !g_mem[i].u) {
        free(g_mem[i].l); free(g_mem[i].u);
        g_mem[i].l = g_mem[i].u = NULL;
        return;
    }
    g_mem_used = 1;
}

/* l is a server name as it is: an older pair for it is stale */
static void mem_drop(const char *l)
{
    int i = mem_index(l);
    if (i < 0) return;
    free(g_mem[i].l); free(g_mem[i].u);
    g_mem[i].l = g_mem[i].u = NULL;
}

int cs_list_name(const char *local_dir, const char *utf8_dir, const char *utf8_name,
                 char *out, size_t outsz)
{
    char l[AC_MAX_PATH], u[AC_MAX_PATH];
    int exact = cs_from_utf8(utf8_name, out, outsz);
    join_path(l, sizeof(l), local_dir, out);
    if (exact) {
        if (g_mem_used) mem_drop(l);
        return 1;
    }
    join_path(u, sizeof(u), utf8_dir, utf8_name);
    mem_put(l, u);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Case folding, names that would be one file                           */
/* ------------------------------------------------------------------ */

void cs_fold_utf8(const char *in, char *out, size_t outsz)
{
    size_t i = 0;
    if (!outsz) return;
    for (; *in && i + 1 < outsz; in++, i++) {
        unsigned char c = (unsigned char)*in;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        else if (c == 0xC3 && i + 2 < outsz) {
            unsigned char d = (unsigned char)in[1];
            /* U+00C0..U+00DE = C3 80..C3 9E, lower case is +0x20 */
            if (d >= 0x80 && d <= 0x9E && d != 0x97) d = (unsigned char)(d + 0x20);
            if (in[1]) { out[i++] = (char)c; out[i] = (char)d; in++; continue; }
        }
        out[i] = (char)c;
    }
    out[i] = '\0';
}

void cs_fold_local(const char *in, char *out, size_t outsz)
{
    size_t i = 0;
    if (g_mode == M_UTF8) { cs_fold_utf8(in, out, outsz); return; }
    if (!outsz) return;
    for (; *in && i + 1 < outsz; in++, i++) {
        unsigned char c = (unsigned char)*in;
        if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7))
            c = (unsigned char)(c + 32);
        out[i] = (char)c;
    }
    out[i] = '\0';
}

/* cs_list_dir(): the listing's folded names and ranks, computed once;
 * entries sorted by folded name, then rank (a name the last listing
 * gave out first, then exact names, then new lossy ones), then server
 * name (qsort has no context argument -- static, like the rest here) */
static char **g_ld_fold;
static const char *const *g_ld_utf8;
static const char *g_ld_rank;

static int ld_cmp(const void *a, const void *b)
{
    int ia = *(const int *)a, ib = *(const int *)b;
    int c = strcmp(g_ld_fold[ia], g_ld_fold[ib]);
    if (c) return c;
    if (g_ld_rank[ia] != g_ld_rank[ib]) return g_ld_rank[ia] - g_ld_rank[ib];
    return strcmp(g_ld_utf8[ia], g_ld_utf8[ib]);
}

/* 1 if a name of the listing (idx sorted) folds to folded */
static int ld_taken(const int *idx, int n, const char *folded)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(g_ld_fold[idx[mid]], folded);
        if (c == 0) return 1;
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

/* name -> name_k with the suffix before the last '.' (not a leading
 * one); the stem is cut if the result would not fit. '_' like
 * CS_REPLACEMENT: '~' would be a pattern character in AmigaDOS
 * ("Delete x~2.info" deletes "X.info") */
static void suffix_name(const char *name, int k, char *out, size_t outsz)
{
    char suf[16];
    const char *dot = strrchr(name, '.');
    size_t stem, extl, sl;
    if (!dot || dot == name) dot = name + strlen(name);
    snprintf(suf, sizeof(suf), "_%d", k);
    sl = strlen(suf);
    extl = strlen(dot);
    stem = (size_t)(dot - name);
    if (stem + sl + extl + 1 > outsz) {
        if (sl + extl + 2 > outsz) extl = 0;                 /* no room even for that */
        stem = outsz > sl + extl + 1 ? outsz - sl - extl - 1 : 0;
    }
    memcpy(out, name, stem);
    memcpy(out + stem, suf, sl);
    memcpy(out + stem + sl, dot, extl);
    out[stem + sl + extl] = '\0';
}

int cs_list_dir(const char *local_dir, const char *utf8_dir,
                const char *const *utf8_names, int n,
                char *out, size_t stride, size_t outsz)
{
    static char cand[AC_MAX_PATH], fc[AC_MAX_PATH], l[AC_MAX_PATH], u[AC_MAX_PATH];
    static char lpre[AC_MAX_PATH];
    int *idx = NULL, i, j, renamed = 0, ngiven = 0;
    size_t lpl;
    char **fold = NULL, **base = NULL, **given = NULL;
    char *rank = NULL, *exact = NULL;

    if (n > 0) {
        idx   = (int *)malloc(sizeof(int) * (size_t)n);
        fold  = (char **)calloc((size_t)n, sizeof(char *));
        base  = (char **)calloc((size_t)n, sizeof(char *));
        given = (char **)malloc(sizeof(char *) * (size_t)n);
        rank  = (char *)malloc((size_t)n);
        exact = (char *)malloc((size_t)n);
        if (!idx || !fold || !base || !given || !rank || !exact) goto oom;
    }
    join_path(lpre, sizeof(lpre), local_dir, "");   /* "dir/" */
    lpl = strlen(lpre);

    /* 1. names as the plain rule gives them, folded */
    for (i = 0; i < n; i++) {
        char *name = out + (size_t)i * stride;
        exact[i] = (char)cs_from_utf8(utf8_names[i], name, outsz);
        if (!(base[i] = cs_dup(name))) goto oom;
        cs_fold_local(name, fc, sizeof(fc));
        if (!(fold[i] = cs_dup(fc))) goto oom;
        rank[i] = (char)(exact[i] ? 1 : 2);
        idx[i] = i;
    }

    /* 2. a name that still meets another one keeps what the last listing
     *    of this directory showed for it ("x_2", a lossy name) -- an open
     *    file keeps its meaning; once alone it gets its plain name back */
    if (n > 1 && g_mem_used) {
        g_ld_fold = fold; g_ld_utf8 = utf8_names; g_ld_rank = exact;   /* any order */
        qsort(idx, (size_t)n, sizeof(int), ld_cmp);
        for (i = 0; i < n; i++) {
            int me = idx[i];
            char *name = out + (size_t)me * stride;
            if (!((i > 0 && strcmp(fold[idx[i - 1]], fold[me]) == 0) ||
                  (i + 1 < n && strcmp(fold[idx[i + 1]], fold[me]) == 0)))
                continue;
            join_path(u, sizeof(u), utf8_dir, utf8_names[me]);
            for (j = 0; j < CS_REMEMBER; j++)
                if (g_mem[j].l && strncmp(g_mem[j].l, lpre, lpl) == 0
                    && !strchr(g_mem[j].l + lpl, '/') && strcmp(g_mem[j].u, u) == 0
                    && strlen(g_mem[j].l + lpl) < outsz) {
                    strcpy(name, g_mem[j].l + lpl);
                    rank[me] = 0;
                    break;
                }
        }
        for (i = 0; i < n; i++) {
            if (rank[i] != 0) continue;
            cs_fold_local(out + (size_t)i * stride, fc, sizeof(fc));
            free(fold[i]);
            if (!(fold[i] = cs_dup(fc))) goto oom;
        }
        for (i = 0; i < n; i++) idx[i] = i;
    }

    /* 3. names that would be one file here: the group's first keeps its
     *    name, the others get the first "_k" free in this listing */
    if (n > 1) {
        g_ld_fold = fold; g_ld_utf8 = utf8_names; g_ld_rank = rank;
        qsort(idx, (size_t)n, sizeof(int), ld_cmp);
        for (i = 1; i < n; i++) {
            char *name = out + (size_t)idx[i] * stride;
            int k, used;
            if (strcmp(fold[idx[i - 1]], fold[idx[i]]) != 0) continue;
            for (k = 2; ; k++) {
                suffix_name(base[idx[i]], k, cand, outsz < sizeof(cand) ? outsz : sizeof(cand));
                cs_fold_local(cand, fc, sizeof(fc));
                if (ld_taken(idx, n, fc)) continue;
                for (j = 0, used = 0; j < ngiven && !used; j++) used = strcmp(given[j], fc) == 0;
                if (!used) break;
            }
            if (!(given[ngiven] = cs_dup(fc))) goto oom;
            ngiven++;
            strcpy(name, cand);
            renamed++;
        }
    }

    /* 3b. the table forgets this directory: its own pairs, and those
     *     below a name it no longer shows (its own set again in 4.) */
    if (g_mem_used)
        for (j = 0; j < CS_REMEMBER; j++) {
            const char *rest, *slash;
            int keep = 0;
            if (!g_mem[j].l || strncmp(g_mem[j].l, lpre, lpl) != 0) continue;
            rest = g_mem[j].l + lpl;
            slash = strchr(rest, '/');
            if (slash)   /* below a child: kept if this listing still shows it */
                for (i = 0; i < n && !keep; i++) {
                    const char *nm = out + (size_t)i * stride;
                    if (strlen(nm) != (size_t)(slash - rest)
                        || strncmp(nm, rest, (size_t)(slash - rest)) != 0) continue;
                    /* ... and still for the same server directory */
                    join_path(u, sizeof(u), utf8_dir, utf8_names[i]);
                    keep = strncmp(g_mem[j].u, u, strlen(u)) == 0 && g_mem[j].u[strlen(u)] == '/';
                }
            if (keep) continue;
            free(g_mem[j].l); free(g_mem[j].u);
            g_mem[j].l = g_mem[j].u = NULL;
        }

    /* 4. every shown name that is not its server name as it is */
    for (i = 0; i < n; i++) {
        const char *name = out + (size_t)i * stride;
        if (exact[i] && strcmp(name, base[i]) == 0) continue;
        join_path(l, sizeof(l), local_dir, name);
        join_path(u, sizeof(u), utf8_dir, utf8_names[i]);
        mem_put(l, u);
    }
    goto done;

oom:
    /* names as cs_list_name() gives them, like before cs_list_dir() */
    for (i = 0; i < n; i++)
        cs_list_name(local_dir, utf8_dir, utf8_names[i], out + (size_t)i * stride, outsz);
    renamed = -1;
done:
    if (given) for (i = 0; i < ngiven; i++) free(given[i]);
    if (fold) for (i = 0; i < n; i++) free(fold[i]);
    if (base) for (i = 0; i < n; i++) free(base[i]);
    free(given); free(fold); free(base); free(idx); free(rank); free(exact);
    return renamed;
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

