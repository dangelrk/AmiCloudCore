/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * sync.c — two-way local<->cloud folder synchronization with a persistent
 * last-sync baseline (AmiOwnCloud docs/design/07_Sync_Metadaten.md). Originally
 * extracted verbatim from the old client/panel.c's panel_sync() (see
 * sync.h); this rewrite adds the missing baseline (syncstate.h) so a
 * 3-way diff (Local / Remote / Baseline) replaces the old stateless
 * 2-way merge-scan, which could never tell "deleted since the last sync"
 * apart from "new on the other side" and resolved every content
 * conflict by mtime instead of ever reporting one.
 *
 * Transport-neutral (see transport.h): the reconcile kernel below
 * (cloudsync_run_ex and everything it calls) only ever calls through a
 * SyncTransport -- amicloud_transport.c (api.php) and webdav.c are the
 * backends.
 */
#include "sync.h"
#include "http.h" /* ac_log_printf() -- see the out->errors++ sites below */
#include "localfs.h"
#include "ui_hooks.h"
#include "md5.h"
#include "syncstate.h"
#include "charset.h"

/* ac_log_printf() is only declared under PLATFORM_AMIGA in http.h (see
 * cmd.c, which guards every one of its own call sites with #ifdef
 * PLATFORM_AMIGA/#endif) -- but this file also builds for PC/Mac/Linux
 * (cloudsync, amiowncloud-sync, amicloud-shell all link it via Makefile.pc).
 * One local no-op fallback here beats individually #ifdef-guarding every
 * call site below; harmless on non-Amiga since ac_log_printf's only
 * consumer there would be this same no-op. */
#ifndef PLATFORM_AMIGA
#define ac_log_printf(...) ((void)0)
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* Flat, sorted indices of both live trees (relative '/'-joined paths,   */
/* so a merge-scan finds matches regardless of directory structure).     */
/* ------------------------------------------------------------------ */
typedef struct {
    char *path;    /* relative to the sync root, '/'-joined, malloc'd */
    int   is_dir;
    long  size;
    long  mtime;   /* UTC epoch */
    /* Real content hash: local entries always have one (freshly hashed
     * or cached from the baseline, see index_local_tree_sync()); remote
     * entries have one only if the transport provides one (AmiCloud),
     * else "" (WebDAV -- an ETag is not a content hash, see token). */
    char  md5[64];
    /* Remote entries only: opaque per-backend change token from the
     * transport (AmiCloud mirrors its own md5 here too; WebDAV's ETag).
     * Never set on local entries, never compared against md5. */
    char  token[64];
    long  revision;/* remote/baseline entries: server content revision, 0 if unknown */
    long  id;      /* remote/baseline entries: server file ID, 0 if unknown */
    /* Local entries only: the path as it is on disk, relative to the
     * root, in the local codeset -- NULL if the same bytes as path (the
     * UTF-8 key). Phase 4: names are UTF-8 everywhere except on disk. */
    char *lrel;
} SyncEntry;

typedef struct { SyncEntry *items; int n, cap; } SyncIndex;

static char *sync_dupstr(const char *s)
{
    size_t len = strlen(s) + 1;
    char *p = (char *)malloc(len);
    if (p) memcpy(p, s, len);
    return p;
}

static void index_init(SyncIndex *idx) { idx->items = NULL; idx->n = 0; idx->cap = 0; }

/* 0 on out-of-memory: a path missing from an index would look deleted on
 * that side, so the caller must treat this as fatal (S1). */
static int index_add(SyncIndex *idx, const char *path, int is_dir,
                     long size, long mtime, const char *md5, const char *token,
                     long revision, long id)
{
    SyncEntry *e;
    char *p;
    if (idx->n == idx->cap) {
        int newcap = idx->cap ? idx->cap * 2 : 64;
        SyncEntry *tmp = (SyncEntry *)realloc(idx->items, sizeof(SyncEntry) * (size_t)newcap);
        if (!tmp) return 0;
        idx->items = tmp;
        idx->cap = newcap;
    }
    p = sync_dupstr(path);
    if (!p) return 0;
    e = &idx->items[idx->n++];
    e->path = p;
    e->lrel = NULL;
    e->is_dir = is_dir;
    e->size = size;
    e->mtime = mtime;
    strncpy(e->md5, md5 ? md5 : "", sizeof(e->md5) - 1);
    e->md5[sizeof(e->md5) - 1] = '\0';
    strncpy(e->token, token ? token : "", sizeof(e->token) - 1);
    e->token[sizeof(e->token) - 1] = '\0';
    e->revision = revision;
    e->id = id;
    return 1;
}

static void index_free(SyncIndex *idx)
{
    int i;
    for (i = 0; i < idx->n; i++) { free(idx->items[i].path); free(idx->items[i].lrel); }
    free(idx->items);
    idx->items = NULL; idx->n = idx->cap = 0;
}

static int sync_entry_cmp(const void *a, const void *b)
{
    return strcmp(((const SyncEntry *)a)->path, ((const SyncEntry *)b)->path);
}


/* Recursively lists the remote tree into idx, one extra `list` request
 * per subdirectory. Relative paths so a plain strcmp() sort always puts
 * a directory right before its own children (a string is always < any
 * string it's a strict prefix of). Goes through the transport, not any
 * one wire protocol -- see transport.h.
 *
 * Returns 0 as soon as one listing fails (or memory runs out), with the
 * failing remote path in errpath. A partial index must never be used: an
 * unlisted subtree would look deleted remotely and its local copy would
 * be deleted (S1). */
static int index_remote_tree_sync(SyncTransport *t, const char *remote_root,
                                  const char *rel_prefix, SyncIndex *idx,
                                  char *errpath, size_t errsz)
{
    SyncRemoteEntry *entries;
    int n, i;
    char remote_path[AC_MAX_PATH];

    if (rel_prefix[0])
        snprintf(remote_path, sizeof(remote_path), "%s/%s", remote_root, rel_prefix);
    else {
        strncpy(remote_path, remote_root, sizeof(remote_path) - 1);
        remote_path[sizeof(remote_path) - 1] = '\0';
    }

    if (!t->list(t, remote_path, &entries, &n)) {
        snprintf(errpath, errsz, "%s", remote_path);
        return 0;
    }
    for (i = 0; i < n; i++) {
        char relchild[AC_MAX_PATH];
        if (sync_name_excluded(entries[i].name)) continue;   /* S13 */
        if (rel_prefix[0])
            snprintf(relchild, sizeof(relchild), "%s/%s", rel_prefix, entries[i].name);
        else {
            strncpy(relchild, entries[i].name, sizeof(relchild) - 1);
            relchild[sizeof(relchild) - 1] = '\0';
        }
        if (!index_add(idx, relchild, entries[i].is_dir, entries[i].size, entries[i].mtime,
                       entries[i].md5, entries[i].token, entries[i].revision, entries[i].id) ||
            (entries[i].is_dir &&
             !index_remote_tree_sync(t, remote_root, relchild, idx, errpath, errsz))) {
            if (!errpath[0]) snprintf(errpath, errsz, "%s (kein Speicher)", remote_path);
            free(entries);
            return 0;
        }
    }
    free(entries);
    return 1;
}

/* Delta-optimisation fast path (see cloudsync_run_ex()): when the server's
 * global_revision hasn't moved since our last successful sync, the
 * remote tree is by definition identical to what it was back then --
 * reuse the baseline itself as the remote index instead of a full
 * recursive `list` scan. */
static int index_remote_from_baseline(SyncState *baseline, SyncIndex *idx)
{
    int i;
    for (i = 0; i < baseline->n; i++) {
        const SyncStateEntry *e = &baseline->items[i];
        if (!index_add(idx, e->path, e->is_dir, e->size, e->mtime, e->md5, e->token,
                       e->revision, e->server_id))
            return 0;
    }
    return 1;
}

/* S13: names never synced, on either side and on every client alike:
 * the kernel's own files, metadata litter of other systems, Office lock
 * files, partial downloads, and names the server or other systems would
 * mangle (backslash -- the server turns it into a '/' -- and control
 * characters). */
static int ends_with(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lx = strlen(suffix);
    return ls >= lx && strcmp(s + ls - lx, suffix) == 0;
}

int sync_name_excluded(const char *name)
{
    const unsigned char *c;
    if (strncmp(name, ".amicloudsync", 13) == 0) return 1;
    if (strcmp(name, ".DS_Store") == 0 || strcmp(name, "Thumbs.db") == 0 ||
        strcmp(name, "desktop.ini") == 0)
        return 1;
    if (strncmp(name, "._", 2) == 0 || strncmp(name, "~$", 2) == 0) return 1;
    if (ends_with(name, ".part")) return 1;
    for (c = (const unsigned char *)name; *c; c++)
        if (*c < 0x20 || *c == 0x7f || *c == '\\') return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* S6: names that differ only in case                                   */
/* ------------------------------------------------------------------ */
/* The server keeps "A.txt" and "a.txt" apart, a case-insensitive local
 * file system (AmigaOS, macOS, Windows) cannot: both would land in one
 * local file, and the next run would upload the wrong content or delete
 * one of them remotely. Such paths -- with everything below them -- are
 * skipped on both sides and reported. ASCII folding only. */
static void fold_case(char *out, size_t sz, const char *in)
{
    size_t i;
    for (i = 0; in[i] && i + 1 < sz; i++)
        out[i] = (in[i] >= 'A' && in[i] <= 'Z') ? (char)(in[i] + 32) : in[i];
    out[i] = '\0';
}

static int folded_cmp(const void *a, const void *b)
{
    char fa[AC_MAX_PATH], fb[AC_MAX_PATH];
    fold_case(fa, sizeof(fa), (*(const SyncEntry *const *)a)->path);
    fold_case(fb, sizeof(fb), (*(const SyncEntry *const *)b)->path);
    return strcmp(fa, fb);
}

typedef struct { char **items; int n, cap; } CaseSkips;

static void case_skips_free(CaseSkips *cs)
{
    int i;
    for (i = 0; i < cs->n; i++) free(cs->items[i]);
    free(cs->items);
    cs->items = NULL; cs->n = cs->cap = 0;
}

/* adds a folded path; 0 on out of memory */
static int skips_add(CaseSkips *cs, const char *folded)
{
    char *d;
    if (cs->n == cs->cap) {
        int nc = cs->cap ? cs->cap * 2 : 16;
        char **t = (char **)realloc(cs->items, sizeof(char *) * (size_t)nc);
        if (!t) return 0;
        cs->items = t; cs->cap = nc;
    }
    if (!(d = sync_dupstr(folded))) return 0;
    cs->items[cs->n++] = d;
    return 1;
}

/* 1 if path is one of the skipped paths or lies below one */
static int case_skipped(const CaseSkips *cs, const char *path);

/* S15: adds every remote path whose own name is longer than name_max
 * (and is not below an already skipped one) to cs; *count is the number
 * added. cs->items gets room for idx->n entries here -- the case check
 * after it adds to the same list. 0 on out of memory. */
static int find_long_names(const SyncIndex *idx, int name_max, CaseSkips *cs, int *count)
{
    int i;
    *count = 0;
    for (i = 0; i < idx->n; i++) {
        const char *p = idx->items[i].path, *name = strrchr(p, '/');
        char f[AC_MAX_PATH], lname[AC_MAX_PATH];
        name = name ? name + 1 : p;
        cs_from_utf8(name, lname, sizeof(lname));   /* length on disk counts */
        if ((int)strlen(lname) <= name_max || case_skipped(cs, p)) continue;
        ui_notify(UI_WARN, "'%s': Name laenger als %d Zeichen, hier nicht moeglich -- uebersprungen",
                  p, name_max);
        fold_case(f, sizeof(f), p);
        if (!skips_add(cs, f)) return 0;
        (*count)++;
    }
    return 1;
}

/* Adds the folded paths that occur more than once in idx to cs (which
 * find_long_names() has set up). 0 on out of memory. */
static int find_case_collisions(const SyncIndex *idx, CaseSkips *cs)
{
    const SyncEntry **v;
    int i;
    if (idx->n < 2) return 1;
    v = (const SyncEntry **)malloc(sizeof(SyncEntry *) * (size_t)idx->n);
    if (!v) return 0;
    for (i = 0; i < idx->n; i++) v[i] = &idx->items[i];
    qsort(v, (size_t)idx->n, sizeof(SyncEntry *), folded_cmp);
    for (i = 1; i < idx->n; i++) {
        char f[AC_MAX_PATH];
        if (folded_cmp(&v[i - 1], &v[i]) != 0) continue;
        fold_case(f, sizeof(f), v[i]->path);
        if (case_skipped(cs, v[i]->path)) continue;   /* already skipped (long name, earlier pair) */
        ui_notify(UI_WARN, "'%s' und '%s' unterscheiden sich nur in Gross-/Kleinschreibung -- "
                  "beide uebersprungen", v[i - 1]->path, v[i]->path);
        if (!skips_add(cs, f)) { free(v); return 0; }
    }
    free(v);
    return 1;
}

/* 1 if path is one of the colliding paths or lies below one */
static int case_skipped(const CaseSkips *cs, const char *path)
{
    char f[AC_MAX_PATH];
    int i;
    if (!cs->n) return 0;
    fold_case(f, sizeof(f), path);
    for (i = 0; i < cs->n; i++) {
        size_t l = strlen(cs->items[i]);
        if (strncmp(f, cs->items[i], l) == 0 && (f[l] == '\0' || f[l] == '/')) return 1;
    }
    return 0;
}

/* Timestamps count as equal within +/-2s — FAT resolution, Amiga ticks,
 * rounding during transfer. */
static int mtimes_equal(long a, long b)
{
    long d = a - b;
    if (d < 0) d = -d;
    return d <= 2;
}

/* ------------------------------------------------------------------ */
/* Phase 4: names on disk vs. UTF-8 keys                                */
/* ------------------------------------------------------------------ */

/* The local form of a UTF-8 relative path: every component through
 * cs_from_utf8(). Deterministic, so a lossy parent maps the same way for
 * all its children. Returns 1 if exact (the way back gives utf8). */
static int local_rel_of(const char *utf8, char *out, size_t outsz)
{
    char comp[AC_MAX_PATH], loc[AC_MAX_PATH];
    const char *p = utf8;
    size_t o = 0;
    int exact = 1;
    out[0] = '\0';
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t n = slash ? (size_t)(slash - p) : strlen(p);
        if (n >= sizeof(comp)) n = sizeof(comp) - 1;
        memcpy(comp, p, n); comp[n] = '\0';
        if (!cs_from_utf8(comp, loc, sizeof(loc))) exact = 0;
        if (o + strlen(loc) + 2 > outsz) return 0;
        if (o) out[o++] = '/';
        strcpy(out + o, loc);
        o += strlen(loc);
        p += n;
        if (*p == '/') p++;
    }
    return exact;
}

/* Lossy names: a server name like "Preis\xE2\x82\xAC.txt" exists on an
 * ISO-8859-1 disk as "Preis_.txt", whose way back is a different UTF-8
 * name. Without help the next run would see "Preis_.txt" as a new local
 * file (upload) and the original as deleted locally (delete on the
 * server). The map ties such local forms back to their server names;
 * it is built from the remote index and the baseline (a name that was
 * deleted on the server must still be recognised locally). */
typedef struct { char *l; char *p; } NameMapEntry;
typedef struct { NameMapEntry *items; int n; } NameMap;

static int nm_cmp(const void *a, const void *b)
{
    int c = strcmp(((const NameMapEntry *)a)->l, ((const NameMapEntry *)b)->l);
    return c ? c : strcmp(((const NameMapEntry *)a)->p, ((const NameMapEntry *)b)->p);
}

static void namemap_free(NameMap *m)
{
    int i;
    for (i = 0; i < m->n; i++) { free(m->items[i].l); free(m->items[i].p); }
    free(m->items);
    m->items = NULL; m->n = 0;
}

static int namemap_add(NameMap *m, int *cap, const char *path)
{
    char l[AC_MAX_PATH];
    if (local_rel_of(path, l, sizeof(l))) return 1;   /* exact: needs no entry */
    if (m->n == *cap) {
        int nc = *cap ? *cap * 2 : 16;
        NameMapEntry *t = (NameMapEntry *)realloc(m->items, sizeof(NameMapEntry) * (size_t)nc);
        if (!t) return 0;
        m->items = t; *cap = nc;
    }
    m->items[m->n].l = sync_dupstr(l);
    m->items[m->n].p = sync_dupstr(path);
    if (!m->items[m->n].l || !m->items[m->n].p) {
        free(m->items[m->n].l); free(m->items[m->n].p);
        return 0;
    }
    m->n++;
    return 1;
}

/* Builds the map; two server names with the same local form (both
 * lossy, or one exact and one lossy) cannot both live on this disk and
 * are skipped like a case collision (S6). 0 on out of memory. */
static int namemap_build(NameMap *m, const SyncIndex *ridx, const SyncState *base,
                         CaseSkips *skips)
{
    int i, cap = 0, w;
    for (i = 0; i < ridx->n; i++)
        if (!namemap_add(m, &cap, ridx->items[i].path)) return 0;
    for (i = 0; i < base->n; i++)
        if (!namemap_add(m, &cap, base->items[i].path)) return 0;
    if (!m->n) return 1;
    qsort(m->items, (size_t)m->n, sizeof(NameMapEntry), nm_cmp);
    for (i = 1, w = 1; i < m->n; i++) {   /* drop exact duplicates (remote + baseline) */
        if (strcmp(m->items[i].l, m->items[w - 1].l) == 0 &&
            strcmp(m->items[i].p, m->items[w - 1].p) == 0) {
            free(m->items[i].l); free(m->items[i].p);
            continue;
        }
        m->items[w++] = m->items[i];
    }
    m->n = w;
    for (i = 0; i < m->n; i++) {
        /* the same local form for different server names */
        int clash = (i > 0 && strcmp(m->items[i].l, m->items[i - 1].l) == 0) ||
                    (i + 1 < m->n && strcmp(m->items[i].l, m->items[i + 1].l) == 0);
        /* ... or an exact server name that is this local form already:
         * then both are skipped, they would share one file on disk */
        char back[AC_MAX_PATH];
        int k, exact_too = 0;
        cs_to_utf8(m->items[i].l, back, sizeof(back));
        for (k = 0; k < ridx->n && !exact_too; k++)
            if (strcmp(ridx->items[k].path, back) == 0) exact_too = 1;
        if (clash || exact_too) {
            char f[AC_MAX_PATH];
            ui_notify(UI_WARN, "'%s' laesst sich hier nicht eindeutig benennen -- uebersprungen",
                      m->items[i].p);
            fold_case(f, sizeof(f), m->items[i].p);
            if (!case_skipped(skips, m->items[i].p) && !skips_add(skips, f)) return 0;
            if (exact_too) {
                fold_case(f, sizeof(f), back);
                if (!case_skipped(skips, back) && !skips_add(skips, f)) return 0;
            }
        }
    }
    return 1;
}

static const char *namemap_find(const NameMap *m, const char *lrel)
{
    int lo = 0, hi = m->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(m->items[mid].l, lrel);
        if (c == 0) return m->items[mid].p;
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

/* Local counterpart to index_remote_tree_sync() — purely local
 * traversal, effectively free. Works fine on AmigaOS too — dos.library
 * Lock() accepts ordinary multi-component "dir/sub/file" paths.
 *
 * MD5 cache (AmiOwnCloud docs/design/07_Sync_Metadaten.md): a file whose size
 * AND mtime still match its baseline entry is presumed unchanged and
 * reuses the baseline's cached hash instead of rehashing; only a file
 * that actually looks different gets a fresh md5_file_hex().
 *
 * Returns 0 if a directory cannot be read (or memory runs out), with the
 * failing local path in errpath -- an unreadable subtree would otherwise
 * look deleted locally and be deleted on the server (S1). */
static int index_local_tree_sync(const char *local_root, const char *rel_prefix,
                                 const char *lrel_prefix, SyncIndex *idx, SyncState *baseline,
                                 const NameMap *map, char *errpath, size_t errsz)
{
    LocalDir *d;
    LocalDirEntry e;
    char local_path[AC_MAX_PATH];

    if (lrel_prefix[0])
        localfs_join(local_path, sizeof(local_path), local_root, lrel_prefix);
    else {
        strncpy(local_path, local_root, sizeof(local_path) - 1);
        local_path[sizeof(local_path) - 1] = '\0';
    }

    d = localfs_opendir(local_path);
    if (!d) {
        snprintf(errpath, errsz, "%s", local_path);
        return 0;
    }
    while (localfs_readdir(d, &e)) {
        char relchild[AC_MAX_PATH], lrelchild[AC_MAX_PATH], uname[AC_MAX_PATH];
        char md5hex[33];
        const char *mapped;
        SyncStateEntry *cached;

        /* S13 -- includes our own baseline, its tempfile and the
         * leftover .part of an aborted download */
        if (sync_name_excluded(e.name)) continue;

        /* the on-disk path, and its UTF-8 key: the server name it was
         * made from if that was lossy (map), else the plain conversion */
        if (lrel_prefix[0])
            snprintf(lrelchild, sizeof(lrelchild), "%s/%s", lrel_prefix, e.name);
        else
            snprintf(lrelchild, sizeof(lrelchild), "%s", e.name);
        mapped = namemap_find(map, lrelchild);
        if (mapped) {
            snprintf(relchild, sizeof(relchild), "%s", mapped);
        } else {
            cs_to_utf8(e.name, uname, sizeof(uname));
            if (rel_prefix[0])
                snprintf(relchild, sizeof(relchild), "%s/%s", rel_prefix, uname);
            else
                snprintf(relchild, sizeof(relchild), "%s", uname);
        }

        md5hex[0] = '\0';
        if (!e.is_dir) {
            cached = syncstate_find(baseline, relchild);
            if (cached && cached->size == e.size && mtimes_equal(cached->mtime, e.mtime)) {
                strncpy(md5hex, cached->md5, sizeof(md5hex) - 1);
                md5hex[sizeof(md5hex) - 1] = '\0';
            } else {
                char local_full[AC_MAX_PATH];
                localfs_join(local_full, sizeof(local_full), local_root, lrelchild);
                if (!md5_file_hex(local_full, md5hex)) md5hex[0] = '\0';
            }
        }

        if (!index_add(idx, relchild, e.is_dir, e.size, e.mtime, md5hex, NULL, 0, 0) ||
            (strcmp(relchild, lrelchild) != 0 &&
             !(idx->items[idx->n - 1].lrel = sync_dupstr(lrelchild))) ||
            (e.is_dir &&
             !index_local_tree_sync(local_root, relchild, lrelchild, idx, baseline, map,
                                    errpath, errsz))) {
            if (!errpath[0]) snprintf(errpath, errsz, "%s (kein Speicher)", local_path);
            localfs_closedir(d);
            return 0;
        }
    }
    localfs_closedir(d);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Actions decided in Pass 1, executed in Pass 2.                        */
/* ------------------------------------------------------------------ */
typedef enum {
    SA_MKDIR_LOCAL, SA_MKDIR_REMOTE, SA_UPLOAD, SA_DOWNLOAD,
    SA_ALIGN_LOCAL, SA_ALIGN_REMOTE, SA_DEL_LOCAL, SA_DEL_REMOTE,
    /* phase 3, conflicts kept as copies (see do_keep_both() etc.) */
    SA_KEEP_BOTH,   /* file vs file: compare, then local -> copy, server -> name */
    SA_FILE_AWAY,   /* local file vs server directory */
    SA_RFILE_AWAY   /* local directory vs server file */
} SyncActionType;

typedef struct {
    SyncActionType type;
    char relpath[AC_MAX_PATH]; /* relative to the sync root -- the baseline key */
    char lpath[AC_MAX_PATH];
    char rpath[AC_MAX_PATH];
    long mtime; /* target mtime after copy/alignment */
    long size;  /* UPLOAD/DOWNLOAD only, for the byte-percentage */
    int  failed; /* set in Pass 2 on failure -- triggers a baseline rollback */
    int  skipped;/* not executed (break, changed since the scan) -- rolled back */
    int  done;   /* processed in Pass 2 (successfully or not) */
    int  is_dir; /* DEL_LOCAL/DEL_REMOTE: a directory */
    /* DOWNLOAD/DEL_LOCAL: the local file as seen by the scan, re-checked
     * right before it is replaced or deleted (S7) */
    int  lexists;
    long lsize, lmtime;
    /* UPLOAD: condition for the server (S8) -- the remote file as the
     * scan saw it, or create_only if there was none */
    long if_revision;
    long if_id;
    char if_token[64];
    int  create_only;
    char lmd5[33];  /* KEEP_BOTH: hash of the local version as scanned */
    int  declined;  /* deletion refused by the mass deletion guard */
} SyncAction;

/* oom: an action could not be queued. Its baseline entry is predicted
 * anyway, so the run must not execute or save anything (S1). */
typedef struct { SyncAction *items; int n, cap, oom; } SyncActions;

static void actions_init(SyncActions *a) { a->items = NULL; a->n = 0; a->cap = 0; a->oom = 0; }

static SyncAction *actions_add(SyncActions *a, SyncActionType type)
{
    SyncAction *act;
    if (a->n == a->cap) {
        int newcap = a->cap ? a->cap * 2 : 32;
        SyncAction *tmp = (SyncAction *)realloc(a->items, sizeof(SyncAction) * (size_t)newcap);
        if (!tmp) { a->oom = 1; return NULL; }
        a->items = tmp; a->cap = newcap;
    }
    act = &a->items[a->n++];
    memset(act, 0, sizeof(*act));
    act->type = type;
    return act;
}

static void actions_free(SyncActions *a) { free(a->items); a->items = NULL; a->n = a->cap = 0; }

static SyncAction *queue_action(SyncActions *acts, SyncActionType type, const char *relpath)
{
    SyncAction *act = actions_add(acts, type);
    if (act) strncpy(act->relpath, relpath, sizeof(act->relpath) - 1);
    return act;
}

/* re: the remote file the upload replaces, NULL if there is none (S8) */
static void queue_upload(SyncActions *acts, const SyncEntry *le, const SyncEntry *re,
                        const char *relpath, const char *lpath, const char *rpath)
{
    SyncAction *act = queue_action(acts, SA_UPLOAD, relpath);
    if (!act) return;
    strncpy(act->lpath, lpath, sizeof(act->lpath) - 1);
    strncpy(act->rpath, rpath, sizeof(act->rpath) - 1);
    act->mtime = le->mtime;
    act->size  = le->size;
    if (re) {
        act->if_revision = re->revision;
        act->if_id       = re->id;
        strncpy(act->if_token, re->token, sizeof(act->if_token) - 1);
    } else {
        act->create_only = 1;
    }
}

static void note_local(SyncAction *act, const SyncEntry *le)
{
    act->lexists = le != NULL;
    act->lsize   = le ? le->size : 0;
    act->lmtime  = le ? le->mtime : 0;
}

/* le: the local file the download replaces, NULL if there is none */
static void queue_download(SyncActions *acts, const SyncEntry *re, const SyncEntry *le,
                          const char *relpath, const char *lpath, const char *rpath)
{
    SyncAction *act = queue_action(acts, SA_DOWNLOAD, relpath);
    if (!act) return;
    strncpy(act->lpath, lpath, sizeof(act->lpath) - 1);
    strncpy(act->rpath, rpath, sizeof(act->rpath) - 1);
    act->mtime = re->mtime;
    act->size  = re->size;
    note_local(act, le);
}

static void queue_mkdir_local(SyncActions *acts, const char *relpath, const char *lpath)
{
    SyncAction *act = queue_action(acts, SA_MKDIR_LOCAL, relpath);
    if (act) strncpy(act->lpath, lpath, sizeof(act->lpath) - 1);
}

static void queue_mkdir_remote(SyncActions *acts, const char *relpath, const char *rpath)
{
    SyncAction *act = queue_action(acts, SA_MKDIR_REMOTE, relpath);
    if (act) strncpy(act->rpath, rpath, sizeof(act->rpath) - 1);
}

/* Virus check of a file before it is uploaded or put in place (see
 * SyncOptions). Reports and counts a hit as an error. */
static int virus_found(const SyncOptions *o, const char *path, const char *shown,
                       SyncSummary *out)
{
    char name[128];
    int r;
    if (o->virus_max < 0) return 0;
    r = (o->virus_check ? o->virus_check : localfs_virus_check)(path, o->virus_max,
                                                                 name, sizeof(name));
    if (r != 1) return 0;
    ui_notify(UI_ERROR, "VIRUS in %s: %s -- nicht uebertragen", shown, name[0] ? name : "?");
    ac_log_printf("sync: Virus %s in %s -- nicht uebertragen\n", name, shown);
    out->errors++;
    out->viruses++;
    return 1;
}

/* S7: 1 if the local file is still exactly what the scan saw (or still
 * absent) -- checked right before it is replaced or deleted. */
static int local_as_scanned(const SyncAction *act)
{
    long size = 0, mtime = 0;
    int exists = localfs_stat(act->lpath, &size, &mtime);
    if (!act->lexists) return !exists;
    return exists && size == act->lsize && mtime == act->lmtime;
}

/* Both paths are stored: a directory delete may be turned into a mkdir
 * on the other side (keep_dirs_with_survivors(), S5). */
static void queue_del_local(SyncActions *acts, const SyncEntry *le, const char *relpath,
                            const char *lpath, const char *rpath)
{
    SyncAction *act = queue_action(acts, SA_DEL_LOCAL, relpath);
    if (!act) return;
    strncpy(act->lpath, lpath, sizeof(act->lpath) - 1);
    strncpy(act->rpath, rpath, sizeof(act->rpath) - 1);
    act->is_dir = le->is_dir;
    note_local(act, le);
}

static void queue_del_remote(SyncActions *acts, const SyncEntry *re, const char *relpath,
                             const char *lpath, const char *rpath)
{
    SyncAction *act = queue_action(acts, SA_DEL_REMOTE, relpath);
    if (!act) return;
    strncpy(act->lpath, lpath, sizeof(act->lpath) - 1);
    strncpy(act->rpath, rpath, sizeof(act->rpath) - 1);
    act->is_dir = re->is_dir;
    if (!re->is_dir) {   /* S8: delete only the version the scan saw */
        act->if_revision = re->revision;
        act->if_id       = re->id;
        strncpy(act->if_token, re->token, sizeof(act->if_token) - 1);
    }
}

/* Right before a directory is deleted on one side (Pass 2b), it is read
 * again: everything the run knew about is gone by now (deepest first),
 * so anything still in it appeared during the run, or is a kept
 * conflict -- then the directory stays. Names that are never synced
 * (S13: .DS_Store, Thumbs.db, ...) would block a plain remove forever and
 * are removed with the directory. Returns 1 = now empty, 0 = something
 * else is in it, -1 = error. */
static int remote_dir_clear(SyncTransport *t, const char *rpath)
{
    SyncRemoteEntry *e = NULL;
    int n = 0, i, others = 0, rc = 1;
    if (!t->list(t, rpath, &e, &n)) return -1;
    for (i = 0; i < n; i++)
        if (!sync_name_excluded(e[i].name)) others = 1;
    if (others) rc = 0;
    else {
        for (i = 0; i < n; i++) {
            char p[AC_MAX_PATH];
            snprintf(p, sizeof(p), "%s/%s", rpath, e[i].name);
            if (t->del(t, p, NULL) != SYNC_PUT_OK) { rc = -1; break; }
        }
    }
    free(e);
    return rc;
}

static int local_dir_clear(const char *lpath)
{
    LocalDir *d;
    LocalDirEntry e;
    int others = 0, rc = 1;
    if (!(d = localfs_opendir(lpath))) return -1;
    while (localfs_readdir(d, &e))
        if (!sync_name_excluded(e.name)) others = 1;
    localfs_closedir(d);
    if (others) return 0;
    if (!(d = localfs_opendir(lpath))) return -1;
    while (localfs_readdir(d, &e)) {
        char p[AC_MAX_PATH];
        localfs_join(p, sizeof(p), lpath, e.name);
        if (!localfs_remove(p)) rc = -1;
    }
    localfs_closedir(d);
    return rc;
}

static int act_ptr_relpath_cmp(const void *a, const void *b)
{
    return strcmp((*(SyncAction *const *)a)->relpath, (*(SyncAction *const *)b)->relpath);
}

static int entry_path_cmp(const void *key, const void *e)
{
    return strcmp((const char *)key, ((const SyncEntry *)e)->path);
}

/* 1 if a delete action of the given type is queued for relpath. dels is
 * sorted by relpath and holds every delete action of both types. */
static int del_queued(SyncAction **dels, int n, SyncActionType type, const char *relpath)
{
    SyncAction key, *kp = &key;
    SyncAction **hit;
    strncpy(key.relpath, relpath, sizeof(key.relpath) - 1);
    key.relpath[sizeof(key.relpath) - 1] = '\0';
    hit = (SyncAction **)bsearch(&kp, dels, (size_t)n, sizeof(SyncAction *), act_ptr_relpath_cmp);
    return hit && (*hit)->type == type;
}

/* S5: a directory is only deleted if this run also deletes everything
 * below it on that side. Otherwise something survives (a file added on
 * that side since the last sync, or a conflict that is kept) -- a WebDAV
 * DELETE would take it along, and the plain local remove would fail. The
 * delete then turns into recreating the directory on the side where it
 * was deleted, so the survivors can be transferred into it.
 *
 * Deepest directories first, so a converted subdirectory already counts
 * as a survivor of its parent. idx is the side the delete would happen
 * on (ridx for SA_DEL_REMOTE, lidx for SA_DEL_LOCAL), sorted by path. */
static void keep_dirs_with_survivors(SyncActions *acts, const SyncIndex *ridx,
                                     const SyncIndex *lidx, SyncState *new_st, long now,
                                     const char *verbose_tag)
{
    SyncAction **dels;
    int i, n = 0;

    dels = (SyncAction **)malloc(sizeof(SyncAction *) * (size_t)(acts->n ? acts->n : 1));
    if (!dels) { acts->oom = 1; return; }
    for (i = 0; i < acts->n; i++)
        if (acts->items[i].type == SA_DEL_LOCAL || acts->items[i].type == SA_DEL_REMOTE)
            dels[n++] = &acts->items[i];
    qsort(dels, (size_t)n, sizeof(SyncAction *), act_ptr_relpath_cmp);

    for (i = n - 1; i >= 0; i--) {
        SyncAction *act = dels[i];
        const SyncIndex *idx;
        const SyncEntry *d;
        size_t plen;
        int k, survivor = 0;

        if (!act->is_dir) continue;
        idx = (act->type == SA_DEL_REMOTE) ? ridx : lidx;
        d = (const SyncEntry *)bsearch(act->relpath, idx->items, (size_t)idx->n,
                                       sizeof(SyncEntry), entry_path_cmp);
        if (!d) continue;
        plen = strlen(act->relpath);
        for (k = (int)(d - idx->items) + 1; k < idx->n; k++) {
            const char *p = idx->items[k].path;
            if (strncmp(p, act->relpath, plen) != 0 || p[plen] != '/') break;
            if (!del_queued(dels, n, act->type, p)) { survivor = 1; break; }
        }
        if (!survivor) continue;

        if (verbose_tag)
            ui_notify(UI_INFO, "%s: Ordner %s bleibt, darin gibt es Neues", verbose_tag, act->relpath);
        if (act->type == SA_DEL_REMOTE) {
            act->type = SA_MKDIR_LOCAL;
            syncstate_upsert(new_st, act->relpath, 1, 0, d->mtime, "", d->token,
                             d->revision, d->id, now);
        } else {
            act->type = SA_MKDIR_REMOTE;
            syncstate_upsert(new_st, act->relpath, 1, 0, d->mtime, "", "", 1, 0, now);
        }
    }
    free(dels);
}

/* Content matches on both sides but mtimes drifted -- align both to the
 * older one (2s-tolerant: FAT resolution, Amiga ticks, rounding). */
static void queue_align_if_needed(SyncActions *acts, long re_mtime, long le_mtime,
                                 const char *lpath, const char *rpath)
{
    long older;
    if (mtimes_equal(re_mtime, le_mtime)) return;
    older = (re_mtime < le_mtime) ? re_mtime : le_mtime;
    if (re_mtime != older) {
        SyncAction *act = actions_add(acts, SA_ALIGN_REMOTE);
        if (act) { strncpy(act->rpath, rpath, sizeof(act->rpath) - 1); act->mtime = older; }
    }
    if (le_mtime != older) {
        SyncAction *act = actions_add(acts, SA_ALIGN_LOCAL);
        if (act) { strncpy(act->lpath, lpath, sizeof(act->lpath) - 1); act->mtime = older; }
    }
}

static void report_conflict(int verbose, SyncSummary *out, SyncState *new_st,
                           const char *path, const char *reason)
{
    if (verbose)
        ui_notify(UI_WARN, "amicloudcore: sync: conflict at '%s' (%s) -- skipped", path, reason);
    out->conflicts++;
    /* A conflicted path's baseline entry (see carry_forward()) does NOT
     * mirror its actual current remote content -- disables the delta-
     * optimisation shortcut on every future run until this is resolved,
     * see SyncState.had_conflicts. Without this, a later sync could
     * reuse that stale entry as if it were live remote truth and
     * silently "resolve" the conflict by overwriting one side -- exactly
     * what this whole feature exists to prevent. */
    new_st->had_conflicts = 1;
}

/* Nothing was resolved for this path (a conflict) -- keep its old
 * baseline entry exactly as-is, so the next run re-evaluates the same
 * comparison instead of losing the reference point. */
static void carry_forward(SyncState *new_st, const SyncStateEntry *be)
{
    if (!be) return;
    syncstate_upsert(new_st, be->path, be->is_dir, be->size, be->mtime, be->md5, be->token,
                    be->revision, be->server_id, be->last_sync);
}

/* A predicted new-baseline entry was added in Pass 1, but its action
 * failed to execute in Pass 2 -- the path never actually changed, so
 * revert to whatever the old baseline said (or nothing, if it wasn't in
 * the old baseline either). */
static void rollback_baseline_entry(SyncState *new_st, SyncState *old_st, const char *path)
{
    SyncStateEntry *old = syncstate_find(old_st, path);
    syncstate_remove(new_st, path);
    if (old)
        syncstate_upsert(new_st, old->path, old->is_dir, old->size, old->mtime, old->md5, old->token,
                        old->revision, old->server_id, old->last_sync);
}

/* S11: a long run saves what is really done so far every
 * SYNC_CHECKPOINT_ACTIONS actions or SYNC_CHECKPOINT_SECS seconds -- a
 * crash or power cut after hours then loses little. The checkpoint is
 * the predicted new baseline with every pending, failed or skipped
 * action rolled back, and the OLD global_revision (so the next run scans
 * fully). */
#define SYNC_CHECKPOINT_ACTIONS 50   /* at least; grows to acts/10 */
#define SYNC_CHECKPOINT_SECS    120

static int str_ptr_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void upsert_entry(SyncState *st, const SyncStateEntry *e)
{
    syncstate_upsert(st, e->path, e->is_dir, e->size, e->mtime, e->md5, e->token,
                     e->revision, e->server_id, e->last_sync);
}

/* The baseline as it really is: new_st, except that every path of a
 * failed or skipped action -- and with pending_too also of every action
 * not done yet -- keeps its old_st entry (or none). One merge pass over
 * both sorted states, O(n log n); rolling back entry by entry would be
 * O(n^2) for a long run. out is initialised here; 0 on out of memory. */
static int build_effective_state(SyncState *out, const SyncState *new_st,
                                 const SyncState *old_st, const SyncActions *acts,
                                 int pending_too)
{
    const char **pend;
    int np = 0, i = 0, j = 0, k, before;

    memset(out, 0, sizeof(*out));
    pend = (const char **)malloc(sizeof(char *) * (size_t)(acts->n ? acts->n : 1));
    if (!pend || !syncstate_set_identity(out, new_st->identity)) { free(pend); return 0; }
    for (k = 0; k < acts->n; k++) {
        const SyncAction *a = &acts->items[k];
        if (a->relpath[0] && (a->failed || a->skipped || (pending_too && !a->done)))
            pend[np++] = a->relpath;
    }
    qsort(pend, (size_t)np, sizeof(char *), str_ptr_cmp);

    while (i < new_st->n || j < old_st->n) {
        const SyncStateEntry *ne = i < new_st->n ? &new_st->items[i] : NULL;
        const SyncStateEntry *oe = j < old_st->n ? &old_st->items[j] : NULL;
        const char *p;
        int c = !ne ? 1 : !oe ? -1 : strcmp(ne->path, oe->path);
        p = c <= 0 ? ne->path : oe->path;
        if (c > 0) ne = NULL;
        if (c < 0) oe = NULL;
        if (ne) i++;
        if (oe) j++;
        before = out->n;
        if (bsearch(&p, pend, (size_t)np, sizeof(char *), str_ptr_cmp)) {
            if (!oe) continue;
            upsert_entry(out, oe);
        } else {
            if (!ne) continue;
            upsert_entry(out, ne);
        }
        if (out->n == before) { free(pend); syncstate_free(out); return 0; }
    }
    free(pend);
    out->global_revision = new_st->global_revision;
    out->had_conflicts   = new_st->had_conflicts;
    return 1;
}

static void save_checkpoint(const char *local_root, const SyncState *new_st,
                            SyncState *old_st, const SyncActions *acts)
{
    SyncState cp;
    if (!build_effective_state(&cp, new_st, old_st, acts, 1))
        return;   /* no memory: just no checkpoint */
    cp.global_revision = old_st->global_revision;
    cp.had_conflicts = old_st->had_conflicts || new_st->had_conflicts;
    syncstate_save(local_root, &cp);
    syncstate_free(&cp);
}

/* Has the remote file changed since the last sync? Strongest signals
 * first:
 *   - another server file ID: the path now holds a different file (e.g.
 *     deleted, then another file renamed onto it -- a rename keeps the
 *     revision, so the revision alone would not notice);
 *   - AmiCloud's revision, and with it the real content hash (the
 *     baseline md5 equals the remote hash of a synced file);
 *   - only a hash (no revisions);
 *   - WebDAV's opaque ETag against the remembered one. An empty
 *     remembered token means "not known yet" (right after our own upload),
 *     which is not a change -- the next listing fills it in. */
static int remote_file_changed(const SyncEntry *re, const SyncStateEntry *be)
{
    if (be->server_id > 0 && re->id > 0 && re->id != be->server_id) return 1;
    if (be->revision > 0 && re->revision > 0) {
        if (re->revision != be->revision) return 1;
        return re->md5[0] && be->md5[0] && strcmp(re->md5, be->md5) != 0;
    }
    if (re->md5[0]) return strcmp(re->md5, be->md5) != 0;
    return be->token[0] && strcmp(re->token, be->token) != 0;
}

/* ------------------------------------------------------------------ */
/* Phase 3: conflicts -- keep both versions                             */
/* ------------------------------------------------------------------ */

/* "Brief.txt" -> "Brief_K261031-1402.txt" (local time; n > 1 adds
 * "-n"). base is a UTF-8 server name; the result is shortened at the
 * stem so that its LOCAL form fits name_max. Only '_', '-', 'K' and
 * digits are added: legal on AmigaOS, Windows, macOS, Linux and servers. */
static void conflict_name(const char *base, int n, int name_max, char *out, size_t sz)
{
    char stem[AC_MAX_PATH], suffix[32], local[AC_MAX_PATH];
    const char *dot = strrchr(base, '.');
    const char *ext;
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    size_t sl;

    if (!dot || dot == base) dot = base + strlen(base);   /* no extension, or ".name" */
    ext = dot;
    sl = (size_t)(dot - base);
    if (sl >= sizeof(stem)) sl = sizeof(stem) - 1;
    memcpy(stem, base, sl);
    stem[sl] = '\0';
    if (tm)
        snprintf(suffix, sizeof(suffix), "_K%02d%02d%02d-%02d%02d", tm->tm_year % 100,
                 tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min);
    else
        snprintf(suffix, sizeof(suffix), "_K");
    if (n > 1) {
        size_t l = strlen(suffix);
        snprintf(suffix + l, sizeof(suffix) - l, "-%d", n);
    }
    for (;;) {
        snprintf(out, sz, "%s%s%s", stem, suffix, ext);
        cs_from_utf8(out, local, sizeof(local));
        if ((int)strlen(local) <= name_max || !stem[0]) break;
        /* drop the last UTF-8 character of the stem */
        sl = strlen(stem);
        do { sl--; } while (sl > 0 && ((unsigned char)stem[sl] & 0xC0) == 0x80);
        stem[sl] = '\0';
    }
}

/* last component of a '/' path */
static const char *last_comp(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* dir part of relpath ("" at the top) joined with name */
static void rel_sibling(char *out, size_t sz, const char *relpath, const char *name)
{
    const char *s = strrchr(relpath, '/');
    if (s) snprintf(out, sz, "%.*s/%s", (int)(s - relpath), relpath, name);
    else snprintf(out, sz, "%s", name);
}

static void rpath_sibling(char *out, size_t sz, const char *rpath, const char *name)
{
    const char *s = strrchr(rpath, '/');
    if (s) snprintf(out, sz, "%.*s/%s", (int)(s - rpath), rpath, name);
    else snprintf(out, sz, "%s", name);
}

/* A free conflict copy name next to lpath: UTF-8 name in uname, local
 * path in lcpath. 0 if none of 9 tries is free. */
static int find_conflict_copy(const SyncAction *act, int name_max,
                              char *uname, size_t usz, char *lcpath, size_t lsz)
{
    int n;
    for (n = 1; n <= 9; n++) {
        char lname[AC_MAX_PATH];
        conflict_name(last_comp(act->relpath), n, name_max, uname, usz);
        cs_from_utf8(uname, lname, sizeof(lname));
        localfs_sibling(lcpath, lsz, act->lpath, lname);
        if (!localfs_exists(lcpath)) return 1;
    }
    return 0;
}

/* Uploads a conflict copy under its own name (never over something) and
 * records it in the baseline. 1 on success. */
static int upload_copy(SyncTransport *t, const SyncAction *act, const char *lcpath,
                       const char *uname, SyncState *new_st, long now)
{
    char rcpath[AC_MAX_PATH], relc[AC_MAX_PATH], md5hex[33];
    long size = 0, mtime = 0;
    SyncPutCond cond;
    SyncPutResult res;
    rpath_sibling(rcpath, sizeof(rcpath), act->rpath, uname);
    rel_sibling(relc, sizeof(relc), act->relpath, uname);
    localfs_stat(lcpath, &size, &mtime);
    if (!md5_file_hex(lcpath, md5hex)) md5hex[0] = '\0';
    memset(&cond, 0, sizeof(cond));
    cond.create_only = 1;
    if (t->put(t, lcpath, rcpath, mtime, &cond, &res) != SYNC_PUT_OK) {
        ui_notify(UI_WARN, "Konfliktkopie %s nicht hochgeladen -- folgt beim naechsten Lauf", lcpath);
        return 0;
    }
    syncstate_upsert(new_st, relc, 0, size, mtime, md5hex, res.token,
                     res.revision > 0 ? res.revision : 1, res.id, now);
    return 1;
}

/* SA_KEEP_BOTH: both versions of a file differ (changed on both sides,
 * or both new without a baseline). The server version is loaded into a
 * .part first and compared -- equal content (WebDAV has no hashes) is no
 * conflict at all. Otherwise the local file becomes the conflict copy
 * (renamed, never overwritten), the server version takes the original
 * name, and the copy is uploaded under its own name. */
static void do_keep_both(SyncTransport *t, SyncAction *act, const SyncOptions *o,
                         SyncState *new_st, SyncSummary *out, int name_max, long now)
{
    char part[AC_MAX_PATH], h[33], uname[AC_MAX_PATH], lcpath[AC_MAX_PATH];
    SyncStateEntry *se;

    localfs_sibling(part, sizeof(part), act->lpath, SYNC_PART_FILE);
    if (t->get(t, act->rpath, part, act->lpath) != 0) {
        remove(part);
        act->failed = 1; out->errors++;
        ac_log_printf("sync: Herunterladen fehlgeschlagen: %s\n", act->rpath);
        return;
    }
    if (virus_found(o, part, act->lpath, out)) { remove(part); act->failed = 1; return; }
    if (!md5_file_hex(part, h)) h[0] = '\0';
    se = syncstate_find(new_st, act->relpath);
    if (h[0] && strcmp(h, act->lmd5) == 0) {   /* same content: nothing to keep twice */
        remove(part);
        if (se) { strncpy(se->md5, h, sizeof(se->md5) - 1); se->md5[sizeof(se->md5) - 1] = '\0'; }
        out->unchanged++;
        return;
    }
    if (!local_as_scanned(act)) {              /* S7 */
        remove(part);
        act->skipped = 1;
        return;
    }
    if (!find_conflict_copy(act, name_max, uname, sizeof(uname), lcpath, sizeof(lcpath)) ||
        rename(act->lpath, lcpath) != 0) {
        remove(part);
        act->failed = 1; out->errors++;
        ui_notify(UI_ERROR, "Konflikt bei %s: lokale Fassung laesst sich nicht umbenennen", act->lpath);
        return;
    }
    if (!localfs_replace(part, act->lpath)) {
        rename(lcpath, act->lpath);            /* back as it was */
        remove(part);
        act->failed = 1; out->errors++;
        return;
    }
    localfs_utime(act->lpath, act->mtime);
    if (se) { strncpy(se->md5, h, sizeof(se->md5) - 1); se->md5[sizeof(se->md5) - 1] = '\0'; }
    upload_copy(t, act, lcpath, uname, new_st, now);
    out->copies++;
    ui_notify(UI_WARN, "Konflikt: %s -- beide Fassungen behalten, die lokale als %s",
              act->lpath, lcpath);
}

/* SA_FILE_AWAY: a local file where the server has a directory. The file
 * becomes a conflict copy (renamed, uploaded); the directory then comes
 * down normally (its contents are queued right after this action). */
static void do_file_away(SyncTransport *t, SyncAction *act, SyncState *new_st,
                         SyncSummary *out, int name_max, long now)
{
    char uname[AC_MAX_PATH], lcpath[AC_MAX_PATH];
    if (!local_as_scanned(act)) { act->skipped = 1; return; }
    if (!find_conflict_copy(act, name_max, uname, sizeof(uname), lcpath, sizeof(lcpath)) ||
        rename(act->lpath, lcpath) != 0 || !localfs_mkdir(act->lpath)) {
        act->failed = 1; out->errors++;
        ui_notify(UI_ERROR, "Konflikt bei %s: Datei gegen Ordner nicht aufloesbar", act->lpath);
        return;
    }
    upload_copy(t, act, lcpath, uname, new_st, now);
    out->copies++;
    ui_notify(UI_WARN, "Konflikt: %s ist auf dem Server ein Ordner -- die Datei heisst jetzt %s",
              act->lpath, lcpath);
}

/* SA_RFILE_AWAY: a local directory where the server has a file. The
 * server file is loaded as a local conflict copy, uploaded under that
 * name, deleted under its old name (only if unchanged, S8), and the
 * directory is created on the server; its contents follow. */
static void do_rfile_away(SyncTransport *t, SyncAction *act, const SyncOptions *o,
                          SyncState *new_st, SyncSummary *out, int name_max, long now)
{
    char uname[AC_MAX_PATH], lcpath[AC_MAX_PATH];
    SyncPutCond cond;
    if (!find_conflict_copy(act, name_max, uname, sizeof(uname), lcpath, sizeof(lcpath)) ||
        t->get(t, act->rpath, lcpath, lcpath) != 0) {
        remove(lcpath);
        act->failed = 1; out->errors++;
        return;
    }
    if (virus_found(o, lcpath, lcpath, out)) { remove(lcpath); act->failed = 1; return; }
    localfs_utime(lcpath, act->mtime);
    if (!upload_copy(t, act, lcpath, uname, new_st, now)) { act->failed = 1; out->errors++; return; }
    memset(&cond, 0, sizeof(cond));
    cond.if_revision = act->if_revision;
    cond.if_id       = act->if_id;
    cond.if_token    = act->if_token;
    if (t->del(t, act->rpath, &cond) != SYNC_PUT_OK || t->mkdir_quiet(t, act->rpath) != 0) {
        act->failed = 1; out->errors++;
        ui_notify(UI_ERROR, "Konflikt bei %s: Server-Datei nicht durch den Ordner ersetzbar", act->rpath);
        return;
    }
    out->copies++;
    ui_notify(UI_WARN, "Konflikt: %s ist lokal ein Ordner -- die Server-Datei heisst jetzt %s",
              act->rpath, uname);
}

/* queues one of the conflict actions; re/le as for downloads/uploads */
static SyncAction *queue_conflict(SyncActions *acts, SyncActionType type, const SyncEntry *re,
                                  const SyncEntry *le, const char *relpath,
                                  const char *lpath, const char *rpath)
{
    SyncAction *act = queue_action(acts, type, relpath);
    if (!act) return NULL;
    strncpy(act->lpath, lpath, sizeof(act->lpath) - 1);
    strncpy(act->rpath, rpath, sizeof(act->rpath) - 1);
    act->mtime = re->mtime;
    act->size  = re->size;
    act->if_revision = re->revision;
    act->if_id       = re->id;
    strncpy(act->if_token, re->token, sizeof(act->if_token) - 1);
    note_local(act, le);
    if (le) strncpy(act->lmd5, le->md5, sizeof(act->lmd5) - 1);
    return act;
}

/* Section 5 of the plan: more deletions on one side than
 * max(del_min, del_pct % of the baseline) are asked for once per side.
 * "No" marks them declined: not executed, and NOT rolled back -- their
 * paths are absent from the new baseline, so the next run sees the files
 * on one side only and copies them back (like ownCloud). */
static void guard_mass_deletion(SyncActions *acts, const SyncOptions *o, int base_n,
                                int dry_run, SyncSummary *out)
{
    int side, i;
    int dmin = o->del_min > 0 ? o->del_min : 20;
    int dpct = o->del_pct > 0 ? o->del_pct : 50;
    long limit = (long)base_n * dpct / 100;
    if (limit < dmin) limit = dmin;
    for (side = 0; side < 2; side++) {
        SyncActionType ty = side ? SA_DEL_REMOTE : SA_DEL_LOCAL;
        int n = 0;
        for (i = 0; i < acts->n; i++) if (acts->items[i].type == ty) n++;
        if (n <= limit) continue;
        if (dry_run) {
            ui_notify(UI_WARN, "Achtung: %d Eintraege wuerden %s geloescht", n,
                      side ? "auf dem Server" : "lokal");
            continue;
        }
        if (o->force_deletes ||
            ui_confirm("Der Abgleich wuerde %d von %d Eintraegen %s loeschen. Wirklich loeschen?",
                       n, base_n, side ? "auf dem Server" : "lokal"))
            continue;
        for (i = 0; i < acts->n; i++)
            if (acts->items[i].type == ty) { acts->items[i].declined = 1; out->kept++; }
        ui_notify(UI_WARN, "%d Loeschungen %s nicht ausgefuehrt -- der naechste Lauf kopiert "
                  "die Dateien zurueck", n, side ? "auf dem Server" : "lokal");
    }
}

/* Smallest of up to three (possibly NULL) current merge-scan keys. */
static const char *pick_min_path(const char *a, const char *b, const char *c)
{
    const char *m = NULL;
    if (a && (!m || strcmp(a, m) < 0)) m = a;
    if (b && (!m || strcmp(b, m) < 0)) m = b;
    if (c && (!m || strcmp(c, m) < 0)) m = c;
    return m;
}

/* Descending by relpath -- deepest path first, so a directory delete
 * always finds itself already empty (Pass 2b). */
static int del_ptr_cmp(const void *a, const void *b)
{
    const SyncAction *aa = *(SyncAction *const *)a;
    const SyncAction *bb = *(SyncAction *const *)b;
    return strcmp(bb->relpath, aa->relpath);
}

/* 1 if remote_root can be listed (only used for a dry first sync) */
static int remote_root_exists(SyncTransport *t, const char *remote_root)
{
    SyncRemoteEntry *e = NULL;
    int n = 0, ok = t->list(t, remote_root, &e, &n);
    free(e);
    return ok;
}

void sync_options_init(SyncOptions *o)
{
    memset(o, 0, sizeof(*o));
}

/* S3: which pair a baseline belongs to. The server URL, user and base
 * path come from the transport's config (NULL in the unit tests), the
 * remote root from the caller, anything else (codeset) from the options. */
static void build_identity(char *out, size_t sz, const SyncTransport *t,
                           const char *remote_root, const SyncOptions *o)
{
    const AcConfig *c = t->cfg;
    snprintf(out, sz, "server=%s|user=%s|base=%s|root=%s|cs=%s|extra=%s",
             c ? c->server : "", c ? c->username : "", c ? c->basepath : "",
             remote_root, cs_name(), (o && o->identity_extra) ? o->identity_extra : "");
}

void cloudsync_run_ex(SyncTransport *t, const char *local_root, const char *remote_root,
                      const SyncOptions *opts, Progress *pr, SyncSummary *out)
{
    SyncOptions defaults;
    int         dry_run, verbose;
    char        identity[AC_MAX_PATH + 512];
    SyncState   old_st, new_st;
    SyncIndex   ridx, lidx;
    SyncActions acts;
    SyncAction  **del_ptrs;
    long        server_global = 0;
    long        now;
    int         status_ok, use_baseline_as_remote, scan_ok;
    int         stopped = 0;   /* break (Ctrl-C) seen: run nothing more */
    int         incomplete = 0;/* some action failed or was skipped */
    int         ri, li, bi, i, del_n;
    char        summary[256];
    char        errpath[AC_MAX_PATH];
    CaseSkips   case_skips = { NULL, 0, 0 };
    NameMap     namemap = { NULL, 0 };
    int         cp_actions = 0;   /* S11 checkpoint counters */
    long        cp_time;
    int         cp_every = SYNC_CHECKPOINT_ACTIONS;
    int         run_name_max = 255;

    if (!opts) { sync_options_init(&defaults); opts = &defaults; }
    dry_run = opts->dry_run;
    verbose = opts->verbose;
    memset(out, 0, sizeof(*out));
    now = (long)time(NULL);
    actions_init(&acts);
    memset(&new_st, 0, sizeof(new_st));
    index_init(&ridx); index_init(&lidx);

    if (verbose)
        ui_notify(UI_INFO, "Sync: %s <-> %s%s", local_root, remote_root,
                 dry_run ? "  (Probelauf, keine Aenderungen)" : "");
    /* Unconditional (not gated on verbose, which callers like the
     * AmiOwnCloudSync commodity always pass 0) -- a no-op unless the caller
     * wired http_log_bptr/http_log_path from cfg.debug (see
     * ac_log_printf()'s own doc comment), but when it is, this is what
     * ties the per-failure ac_log_printf() calls below to which sync run
     * and which pair they belong to. */
    ac_log_printf("sync: %s <-> %s%s\n", local_root, remote_root,
                  dry_run ? " (Probelauf)" : "");

    if (!syncstate_load(local_root, &old_st)) {
        ui_notify(UI_ERROR, "Abgleich abgebrochen, nichts geaendert: kein Speicher fuer %s",
                  SYNCSTATE_FILE);
        out->errors++;
        out->aborted = 1;
        goto out_free;
    }

    /* S3: a baseline of another server, user or remote folder is
     * discarded -- this run then counts as a first sync, which merges
     * both sides and never deletes. */
    build_identity(identity, sizeof(identity), t, remote_root, opts);
    if (old_st.identity && strcmp(old_st.identity, identity) != 0) {
        ui_notify(UI_WARN, "Abgleichstand gehoert zu einem anderen Server oder Ordner -- "
                  "Erstabgleich, es wird nichts geloescht");
        ac_log_printf("sync: Stand verworfen (%s statt %s)\n", old_st.identity, identity);
        syncstate_free(&old_st);
        old_st.had_conflicts = 0;
    }
    if (!syncstate_set_identity(&new_st, identity)) {
        ui_notify(UI_ERROR, "Abgleich abgebrochen, nichts geaendert: kein Speicher");
        out->errors++;
        out->aborted = 1;
        goto out_free;
    }
    /* old_st is sorted by path already (syncstate.h, S12) */

    /* Both sides are created if missing -- never in a dry run. S4: with a
     * baseline the remote root is expected to exist; if it is gone (e.g.
     * renamed on the server), recreating it empty would make every local
     * file look deleted remotely. Its listing then fails and the run stops
     * (S1). The local root needs no such check: the baseline lives in it. */
    /* A missing local root is only created if the folder it should be in
     * exists -- an unmounted drive or USB stick must not turn into a
     * full download to some other place (macOS: /Volumes/...; AmigaOS
     * asks for the volume, cancelling ends up here). */
    if (!localfs_is_dir(local_root)) {
        char parent[AC_MAX_PATH];
        size_t pl;
        localfs_sibling(parent, sizeof(parent), local_root, "");
        pl = strlen(parent);
        if (pl > 1 && parent[pl - 1] == '/') parent[pl - 1] = '\0';  /* AmigaOS: "a/" = parent of a */
        if (parent[0] && !localfs_is_dir(parent)) {
            ui_notify(UI_ERROR, "Abgleich abgebrochen: %s fehlt (Laufwerk nicht eingehaengt?)", parent);
            out->errors++;
            out->aborted = 1;
            goto out_free;
        }
    }
    if (!dry_run) {
        localfs_mkdir_p(local_root);
        if (old_st.n == 0) t->mkdir_quiet(t, remote_root);
    }

    /* Delta optimisation: if the server's global revision hasn't moved
     * since our last successful sync, nothing changed remotely -- skip
     * the (many-requests) recursive remote scan and reuse the baseline
     * itself as the remote index. Local changes are still diffed
     * normally either way; this only ever shortcuts the remote side.
     *
     * Disabled while old_st.had_conflicts is set: a conflicted path's
     * baseline entry deliberately still holds its pre-divergence state
     * (see carry_forward()), not its actual current remote content, so
     * reusing the baseline as-if-live would risk silently "resolving"
     * the conflict by trusting stale data -- see report_conflict(). */
    status_ok = t->global_rev(t, &server_global);
    use_baseline_as_remote = status_ok && !old_st.had_conflicts && old_st.n > 0 &&
                            (server_global == old_st.global_revision);

    /* S1: both trees must be scanned completely, or nothing happens at
     * all -- a missing subtree would otherwise look deleted. */
    errpath[0] = '\0';
    if (use_baseline_as_remote)
        scan_ok = index_remote_from_baseline(&old_st, &ridx);
    else if (dry_run && old_st.n == 0 && !remote_root_exists(t, remote_root))
        scan_ok = 1;   /* preview of a first sync: the root comes later */
    else
        scan_ok = index_remote_tree_sync(t, remote_root, "", &ridx, errpath, sizeof(errpath));
    /* Phase 4: map lossy local names back to their server names */
    if (scan_ok && !namemap_build(&namemap, &ridx, &old_st, &case_skips)) {
        ui_notify(UI_ERROR, "Abgleich abgebrochen, nichts geaendert: kein Speicher");
        out->errors++;
        out->aborted = 1;
        goto out_free;
    }
    if (scan_ok)
        scan_ok = index_local_tree_sync(local_root, "", "", &lidx, &old_st, &namemap,
                                        errpath, sizeof(errpath));
    if (!scan_ok) {
        ui_notify(UI_ERROR, "Abgleich abgebrochen, nichts geaendert: %s nicht lesbar",
                  errpath[0] ? errpath : "(kein Speicher)");
        ac_log_printf("sync: Scan fehlgeschlagen (%s) -- Abbruch ohne Aenderungen\n", errpath);
        out->errors++;
        out->aborted = 1;
        goto out_free;
    }
    /* S2: Ctrl-C during the scan -- stop before anything changes */
    if (ui_break_pending()) {
        ui_notify(UI_WARN, "Abgleich abgebrochen, nichts geaendert");
        out->aborted = 1;
        goto out_free;
    }
    qsort(ridx.items, (size_t)ridx.n, sizeof(SyncEntry), sync_entry_cmp);
    qsort(lidx.items, (size_t)lidx.n, sizeof(SyncEntry), sync_entry_cmp);

    /* S15: names the local file system would truncate (classic FFS: 30)
     * -- truncated they would collide or be uploaded under the short
     * name while the original gets deleted on the server */
    {
        int name_max = opts->name_max > 0 ? opts->name_max
                     : dry_run ? 255 : localfs_name_max(local_root);
        run_name_max = name_max;
        if (!find_long_names(&ridx, name_max, &case_skips, &out->skipped)) {
            ui_notify(UI_ERROR, "Abgleich abgebrochen, nichts geaendert: kein Speicher");
            out->errors++;
            out->aborted = 1;
            goto out_free;
        }
    }

    /* S6 (a dry run must not write the probe file: assume insensitive) */
    if ((dry_run || !localfs_case_sensitive(local_root)) &&
        !find_case_collisions(&ridx, &case_skips)) {
        ui_notify(UI_ERROR, "Abgleich abgebrochen, nichts geaendert: kein Speicher");
        out->errors++;
        out->aborted = 1;
        goto out_free;
    }
    if (case_skips.n > out->skipped) {
        out->conflicts += case_skips.n - out->skipped;
        new_st.had_conflicts = 1;
    }

    /* Pass 1: 3-way merge-scan over Remote (ridx) / Local (lidx) /
     * Baseline (old_st), all keyed by the same relative path -- decide
     * one action (or none) per path. See AmiOwnCloud docs/design/
     * 07_Sync_Metadaten.md for the full decision matrix this implements. */
    ri = li = bi = 0;
    while (ri < ridx.n || li < lidx.n || bi < old_st.n) {
        const char *rp = (ri < ridx.n) ? ridx.items[ri].path : NULL;
        const char *lp = (li < lidx.n) ? lidx.items[li].path : NULL;
        const char *bp = (bi < old_st.n) ? old_st.items[bi].path : NULL;
        const char *cur = pick_min_path(rp, lp, bp);
        SyncEntry      *re = (rp && strcmp(rp, cur) == 0) ? &ridx.items[ri]   : NULL;
        SyncEntry      *le = (lp && strcmp(lp, cur) == 0) ? &lidx.items[li]   : NULL;
        SyncStateEntry *be = (bp && strcmp(bp, cur) == 0) ? &old_st.items[bi] : NULL;
        char lpath[AC_MAX_PATH], rpath[AC_MAX_PATH];

        if (re) ri++;
        if (le) li++;
        if (be) bi++;

        if (case_skipped(&case_skips, cur)) {   /* S6: leave all three alone */
            carry_forward(&new_st, be);
            continue;
        }

        /* the path on disk: as scanned, or the local form of the name */
        if (le && le->lrel) {
            localfs_join(lpath, sizeof(lpath), local_root, le->lrel);
        } else if (le) {
            localfs_join(lpath, sizeof(lpath), local_root, cur);
        } else {
            char lrel[AC_MAX_PATH];
            local_rel_of(cur, lrel, sizeof(lrel));
            localfs_join(lpath, sizeof(lpath), local_root, lrel);
        }
        snprintf(rpath, sizeof(rpath), "%s/%s", remote_root, cur);

        if (le && re) {
            if (le->is_dir != re->is_dir) {
                if (opts->conflict_skip) {
                    report_conflict(verbose, out, &new_st, cur, "file vs. directory");
                } else if (!le->is_dir) {   /* local file, server directory */
                    queue_conflict(&acts, SA_FILE_AWAY, re, le, cur, lpath, rpath);
                    syncstate_upsert(&new_st, cur, 1, 0, re->mtime, "", re->token,
                                     re->revision, re->id, now);
                } else {                    /* local directory, server file */
                    queue_conflict(&acts, SA_RFILE_AWAY, re, le, cur, lpath, rpath);
                    syncstate_upsert(&new_st, cur, 1, 0, le->mtime, "", "", 1, 0, now);
                }
            } else if (le->is_dir) {
                /* Both directories, both already exist: nothing to
                 * transfer, but keep the baseline current so a future
                 * single-sided disappearance is correctly recognised as
                 * a deletion, not "new elsewhere". */
                syncstate_upsert(&new_st, cur, 1, 0, re->mtime, "", re->token, re->revision, re->id, now);
            } else if (!be) {
                /* No prior baseline: the only way to know these two
                 * already-existing copies are the same content is a real
                 * hash on both sides (AmiCloud). A transport with no
                 * content hash (WebDAV, re->md5=="") can never clear this
                 * bar -- it conservatively reports a conflict here rather
                 * than guessing, exactly this codebase's usual policy for
                 * anything it can't actually verify. */
                if (re->md5[0] && strcmp(le->md5, re->md5) == 0) {
                    out->unchanged++;
                    queue_align_if_needed(&acts, re->mtime, le->mtime, lpath, rpath);
                    syncstate_upsert(&new_st, cur, 0, re->size,
                                    (re->mtime < le->mtime) ? re->mtime : le->mtime,
                                    re->md5, re->token, re->revision, re->id, now);
                } else if (opts->conflict_skip) {
                    report_conflict(verbose, out, &new_st, cur,
                                    "unrelated content on both sides, no prior sync baseline");
                } else {
                    /* compared after loading (WebDAV has no hashes); a
                     * real difference keeps both */
                    queue_conflict(&acts, SA_KEEP_BOTH, re, le, cur, lpath, rpath);
                    syncstate_upsert(&new_st, cur, 0, re->size, re->mtime, re->md5, re->token,
                                     re->revision, re->id, now);
                }
            } else {
                int local_changed  = (strcmp(le->md5, be->md5) != 0);
                /* remote_changed: prefer the strongest signal actually
                 * available, in order --
                 *   1. AmiCloud's numeric revision, when both sides have
                 *      one (unchanged from the original logic).
                 *   2. A real content hash from the transport (re->md5,
                 *      AmiCloud) against the SAME kind of value in the
                 *      baseline (be->md5 is always a real hash, see
                 *      syncstate.h) -- also unchanged from the original.
                 *   3. The transport's opaque token (WebDAV's ETag)
                 *      against the baseline's own remembered token. An
                 *      empty be->token means "we don't yet know the
                 *      remote's current marker for this path" (always
                 *      true right after WE uploaded/created it
                 *      ourselves, see the upload/mkdir branches below,
                 *      which deliberately leave it empty) -- there is no
                 *      real signal to compare in that situation, so this
                 *      deliberately does NOT flag a change; the very next
                 *      sync's live listing supplies a real token and the
                 *      "unchanged" branch below records it, closing the
                 *      gap with zero wasted transfers. Without this,
                 *      comparing "" against a real token would always
                 *      differ and the file would flip between upload and
                 *      download forever. */
                int remote_changed = remote_file_changed(re, be);

                if (!local_changed && !remote_changed) {
                    out->unchanged++;
                    queue_align_if_needed(&acts, re->mtime, le->mtime, lpath, rpath);
                    /* md5 stays le->md5 (the real, already-correct local
                     * hash) rather than re->md5 -- for WebDAV, re->md5 is
                     * always "", and overwriting a known-good local hash
                     * with "" here would make the NEXT run's local_changed
                     * check spuriously fire. token is refreshed to the
                     * live re->token unconditionally, which is exactly
                     * what closes the "just uploaded, token unknown yet"
                     * gap described above. */
                    syncstate_upsert(&new_st, cur, 0, re->size,
                                    (re->mtime < le->mtime) ? re->mtime : le->mtime,
                                    le->md5, re->token, re->revision, re->id, now);
                } else if (local_changed && !remote_changed) {
                    queue_upload(&acts, le, re, cur, lpath, rpath);
                    /* token left empty: we don't know WebDAV's new ETag
                     * without an extra request, and don't need to -- see
                     * the "unchanged" branch's comment above. */
                    syncstate_upsert(&new_st, cur, 0, le->size, le->mtime, le->md5, "",
                                    be->revision + 1, be->server_id, now);
                } else if (!local_changed && remote_changed) {
                    queue_download(&acts, re, le, cur, lpath, rpath);
                    /* md5 may be "" here (WebDAV) -- Pass 2's SA_DOWNLOAD
                     * case patches it with a real local hash right after
                     * the transfer completes, at zero network cost. */
                    syncstate_upsert(&new_st, cur, 0, re->size, re->mtime, re->md5, re->token,
                                    re->revision, re->id, now);
                } else if (re->md5[0] && strcmp(le->md5, re->md5) == 0) {
                    out->unchanged++; /* both changed but converged independently */
                    syncstate_upsert(&new_st, cur, 0, re->size, re->mtime, re->md5, re->token,
                                    re->revision, re->id, now);
                } else if (opts->conflict_skip) {
                    report_conflict(verbose, out, &new_st, cur, "different content on both sides since last sync");
                    carry_forward(&new_st, be);
                } else {
                    queue_conflict(&acts, SA_KEEP_BOTH, re, le, cur, lpath, rpath);
                    syncstate_upsert(&new_st, cur, 0, re->size, re->mtime, re->md5, re->token,
                                     re->revision, re->id, now);
                }
            }
        } else if (le && !re) {
            if (be) {
                int unchanged_for_del;
                if (le->is_dir)      unchanged_for_del = (be->is_dir == 1);
                else if (be->is_dir) unchanged_for_del = 0; /* type mismatch vs. baseline */
                else                 unchanged_for_del = (strcmp(le->md5, be->md5) == 0);

                if (unchanged_for_del) {
                    queue_del_local(&acts, le, cur, lpath, rpath);
                } else if (opts->conflict_skip) {
                    report_conflict(verbose, out, &new_st, cur, "deleted remotely but changed locally");
                    carry_forward(&new_st, be);
                } else if (le->is_dir) {    /* changed locally wins: back to the server */
                    queue_mkdir_remote(&acts, cur, rpath);
                    syncstate_upsert(&new_st, cur, 1, 0, le->mtime, "", "", 1, 0, now);
                } else {
                    queue_upload(&acts, le, NULL, cur, lpath, rpath);
                    syncstate_upsert(&new_st, cur, 0, le->size, le->mtime, le->md5, "", 1, 0, now);
                }
            } else if (le->is_dir) {
                queue_mkdir_remote(&acts, cur, rpath);
                syncstate_upsert(&new_st, cur, 1, 0, le->mtime, "", "", 1, 0, now);
            } else {
                queue_upload(&acts, le, NULL, cur, lpath, rpath);
                syncstate_upsert(&new_st, cur, 0, le->size, le->mtime, le->md5, "", 1, 0, now);
            }
        } else if (!le && re) {
            if (be) {
                int unchanged_for_del;
                if (re->is_dir)      unchanged_for_del = (be->is_dir == 1);
                else if (be->is_dir) unchanged_for_del = 0;
                else unchanged_for_del = !remote_file_changed(re, be);

                if (unchanged_for_del) {
                    queue_del_remote(&acts, re, cur, lpath, rpath);
                } else if (opts->conflict_skip) {
                    report_conflict(verbose, out, &new_st, cur, "deleted locally but changed remotely");
                    carry_forward(&new_st, be);
                } else if (re->is_dir) {    /* changed on the server wins: back down */
                    queue_mkdir_local(&acts, cur, lpath);
                    syncstate_upsert(&new_st, cur, 1, 0, re->mtime, "", re->token, re->revision, re->id, now);
                } else {
                    queue_download(&acts, re, NULL, cur, lpath, rpath);
                    syncstate_upsert(&new_st, cur, 0, re->size, re->mtime, re->md5, re->token,
                                     re->revision, re->id, now);
                }
            } else if (re->is_dir) {
                queue_mkdir_local(&acts, cur, lpath);
                syncstate_upsert(&new_st, cur, 1, 0, re->mtime, "", re->token, re->revision, re->id, now);
            } else {
                queue_download(&acts, re, NULL, cur, lpath, rpath);
                /* md5 patched post-transfer in Pass 2, see above. */
                syncstate_upsert(&new_st, cur, 0, re->size, re->mtime, re->md5, re->token,
                                re->revision, re->id, now);
            }
        }
        /* else: !le && !re, only be -- both sides already gone; drop the
         * baseline entry silently (nothing added to new_st), done. */
    }
    keep_dirs_with_survivors(&acts, &ridx, &lidx, &new_st, now, verbose ? "Sync" : NULL);
    guard_mass_deletion(&acts, opts, old_st.n, dry_run, out);
    index_free(&ridx); index_free(&lidx);

    /* An action that could not be queued still has its predicted baseline
     * entry -- saving that would e.g. record an upload that never
     * happened, and the next run would delete the local file. */
    if (acts.oom) {
        ui_notify(UI_ERROR, "Abgleich abgebrochen, nichts geaendert: kein Speicher");
        ac_log_printf("sync: kein Speicher fuer die Aktionsliste -- Abbruch\n");
        out->errors++;
        out->aborted = 1;
        goto out_free;
    }

    /* Pass 2a: mkdir/upload/download/align, same order and progress
     * bookkeeping as before this rewrite. */
    progress_reset(pr);
    cp_time = (long)time(NULL);
    cp_every = acts.n / 10 > SYNC_CHECKPOINT_ACTIONS ? acts.n / 10 : SYNC_CHECKPOINT_ACTIONS;
    for (i = 0; i < acts.n; i++) {
        if (acts.items[i].type == SA_UPLOAD || acts.items[i].type == SA_DOWNLOAD ||
            acts.items[i].type == SA_KEEP_BOTH) {
            pr->total++;
            pr->bytes_total += acts.items[i].size;
        }
    }

    for (i = 0; i < acts.n; i++) {
        SyncAction *act = &acts.items[i];
        /* S2: after a break nothing more runs; the skipped actions are
         * rolled back in the baseline and redone by the next run. */
        if (!dry_run && !stopped && ui_break_pending()) stopped = 1;
        if (stopped) { act->skipped = 1; continue; }
        switch (act->type) {
        case SA_MKDIR_LOCAL:
            if (dry_run) { if (verbose) ui_notify(UI_INFO, "(waere angelegt, lokal)   %s", act->lpath); }
            else if (!localfs_mkdir_p(act->lpath)) {
                act->failed = 1; out->errors++;
                ac_log_printf("sync: mkdir (lokal) fehlgeschlagen: %s\n", act->lpath);
            }
            break;
        case SA_MKDIR_REMOTE:
            if (dry_run) { if (verbose) ui_notify(UI_INFO, "(waere angelegt, remote)  %s", act->rpath); }
            else if (t->mkdir_quiet(t, act->rpath) != 0) {
                act->failed = 1; out->errors++;
                ac_log_printf("sync: mkdir (remote) fehlgeschlagen: %s\n", act->rpath);
            }
            break;
        case SA_UPLOAD:
            if (dry_run) {
                if (verbose) ui_notify(UI_INFO, "(waere hochgeladen)       %s -> %s", act->lpath, act->rpath);
                out->uploaded++;
            } else {
                SyncPutCond   cond;
                SyncPutResult res;
                int rc;
                progress_prefix(pr, act->size);
                if (virus_found(opts, act->lpath, act->lpath, out)) {
                    act->failed = 1;   /* never uploaded */
                    break;
                }
                cond.if_revision = act->if_revision;
                cond.if_id       = act->if_id;
                cond.if_token    = act->if_token;
                cond.create_only = act->create_only;
                rc = t->put(t, act->lpath, act->rpath, act->mtime, &cond, &res);
                if (rc == SYNC_PUT_OK) {
                    /* S10: the server's own view of the new file, instead
                     * of a guessed revision -- avoids a needless download
                     * or a false conflict on the next run */
                    SyncStateEntry *se = syncstate_find(&new_st, act->relpath);
                    out->uploaded++;
                    if (se) {
                        if (res.revision > 0) se->revision  = res.revision;
                        if (res.id > 0)       se->server_id = res.id;
                        if (res.token[0]) {
                            strncpy(se->token, res.token, sizeof(se->token) - 1);
                            se->token[sizeof(se->token) - 1] = '\0';
                        }
                    }
                } else if (rc == SYNC_PUT_CONFLICT) {
                    /* S8: changed (or created) on the server since the
                     * scan -- nothing was overwritten; the next run sees
                     * both changes and reports the conflict */
                    act->skipped = 1;
                    out->conflicts++;
                    new_st.had_conflicts = 1;
                    ui_notify(UI_WARN, "%s wurde inzwischen auf dem Server geaendert -- "
                              "nicht ueberschrieben", act->rpath);
                } else {
                    act->failed = 1;
                    out->errors++;
                    ac_log_printf("sync: Hochladen fehlgeschlagen: %s -> %s (%ld Bytes)\n",
                                 act->lpath, act->rpath, act->size);
                }
            }
            break;
        case SA_DOWNLOAD:
            if (dry_run) {
                if (verbose) ui_notify(UI_INFO, "(waere heruntergeladen)   %s <- %s", act->lpath, act->rpath);
                out->downloaded++;
            } else {
                char part[AC_MAX_PATH];
                int got;
                progress_prefix(pr, act->size);
                localfs_sibling(part, sizeof(part), act->lpath, SYNC_PART_FILE);
                got = (t->get(t, act->rpath, part, act->lpath) == 0);
                if (got && virus_found(opts, part, act->lpath, out)) {
                    remove(part);   /* never put in place */
                    act->failed = 1;
                    break;
                }
                if (got && !local_as_scanned(act)) {
                    /* S7: edited while we were busy -- keep the edit, the
                     * next run compares again */
                    remove(part);
                    act->skipped = 1;
                    ui_notify(UI_WARN, "%s wurde waehrend des Abgleichs geaendert -- uebersprungen",
                              act->lpath);
                    break;
                }
                if (got && !localfs_replace(part, act->lpath)) {
                    ac_log_printf("sync: cannot replace %s\n", act->lpath);
                    got = 0;
                }
                if (!got) remove(part);
                if (got) {
                    localfs_utime(act->lpath, act->mtime);
                    out->downloaded++;
                    /* Pass 1 may have left this path's baseline md5 empty
                     * (a transport with no real content hash, e.g. WebDAV
                     * -- see the SA_DOWNLOAD comment there). Patch it in
                     * with a real local hash of what was just written, at
                     * zero network cost -- otherwise the NEXT sync's
                     * local_changed check (le->md5 vs be->md5=="") would
                     * spuriously fire and re-upload content that's
                     * already correct on the server. */
                    {
                        char hash[64];
                        if (md5_file_hex(act->lpath, hash)) {
                            SyncStateEntry *se = syncstate_find(&new_st, act->relpath);
                            if (se && !se->md5[0]) {
                                strncpy(se->md5, hash, sizeof(se->md5) - 1);
                                se->md5[sizeof(se->md5) - 1] = '\0';
                            }
                        }
                    }
                } else {
                    act->failed = 1;
                    out->errors++;
                    ac_log_printf("sync: Herunterladen fehlgeschlagen: %s <- %s (%ld Bytes)\n",
                                 act->lpath, act->rpath, act->size);
                }
            }
            break;
        case SA_ALIGN_LOCAL:
            if (!dry_run) localfs_utime(act->lpath, act->mtime);
            break;
        case SA_ALIGN_REMOTE:
            if (!dry_run) t->setdate(t, act->rpath, act->mtime);
            break;
        case SA_KEEP_BOTH:
            if (dry_run) { if (verbose) ui_notify(UI_INFO, "(Konflikt, beide behalten) %s", act->lpath); }
            else { progress_prefix(pr, act->size); do_keep_both(t, act, opts, &new_st, out, run_name_max, now); }
            break;
        case SA_FILE_AWAY:
            if (dry_run) { if (verbose) ui_notify(UI_INFO, "(Konflikt Datei/Ordner)   %s", act->lpath); }
            else do_file_away(t, act, &new_st, out, run_name_max, now);
            break;
        case SA_RFILE_AWAY:
            if (dry_run) { if (verbose) ui_notify(UI_INFO, "(Konflikt Ordner/Datei)   %s", act->lpath); }
            else do_rfile_away(t, act, opts, &new_st, out, run_name_max, now);
            break;
        case SA_DEL_LOCAL:
        case SA_DEL_REMOTE:
            continue; /* handled leaf-first in Pass 2b below */
        }
        act->done = 1;
        if (!dry_run && (++cp_actions >= cp_every ||
                         (long)time(NULL) - cp_time >= SYNC_CHECKPOINT_SECS)) {
            save_checkpoint(local_root, &new_st, &old_st, &acts);
            cp_actions = 0;
            cp_time = (long)time(NULL);
        }
    }

    /* Pass 2b: deletions, deepest path first -- a directory delete only
     * ever runs once everything under it is already gone, so a plain
     * non-recursive remove suffices everywhere. Deliberately NOT a
     * recursive rmtree: that would blindly take out a file that
     * independently conflicts (row 9/12 above) along with the rest of
     * the subtree, silently overwriting exactly what this whole feature
     * exists to prevent. */
    del_ptrs = (SyncAction **)malloc(sizeof(SyncAction *) * (size_t)(acts.n ? acts.n : 1));
    del_n = 0;
    if (del_ptrs) {
        for (i = 0; i < acts.n; i++) {
            if (acts.items[i].type == SA_DEL_LOCAL || acts.items[i].type == SA_DEL_REMOTE)
                del_ptrs[del_n++] = &acts.items[i];
        }
        qsort(del_ptrs, (size_t)del_n, sizeof(SyncAction *), del_ptr_cmp);

        for (i = 0; i < del_n; i++) {
            SyncAction *act = del_ptrs[i];
            if (act->declined) continue;   /* the user said no (mass deletion) */
            /* S2: a stopped run never deletes */
            if (!dry_run && !stopped && ui_break_pending()) stopped = 1;
            if (stopped) { act->skipped = 1; continue; }
            if (dry_run) {
                if (verbose)
                    ui_notify(UI_INFO, "(waere geloescht, %s)     %s",
                             act->type == SA_DEL_LOCAL ? "lokal" : "remote", act->relpath);
                out->deleted++;
            } else if (act->type == SA_DEL_LOCAL && !act->is_dir && !local_as_scanned(act)) {
                act->skipped = 1;   /* S7: changed since the scan -- keep it */
                ui_notify(UI_WARN, "%s wurde waehrend des Abgleichs geaendert -- nicht geloescht",
                          act->lpath);
            } else if (act->type == SA_DEL_LOCAL) {
                int clear = act->is_dir ? local_dir_clear(act->lpath) : 1;
                if (clear == 0) {
                    act->skipped = 1;
                    ui_notify(UI_WARN, "%s enthaelt Neues -- nicht geloescht", act->lpath);
                } else if (clear > 0 && localfs_remove(act->lpath)) {
                    out->deleted++;
                } else {
                    act->failed = 1; out->errors++;
                    ac_log_printf("sync: Loeschen (lokal) fehlgeschlagen: %s\n", act->lpath);
                }
            } else {
                SyncPutCond cond;
                int clear = act->is_dir ? remote_dir_clear(t, act->rpath) : 1;
                int rc = SYNC_PUT_ERROR;
                cond.if_revision = act->if_revision;
                cond.if_id       = act->if_id;
                cond.if_token    = act->if_token;
                cond.create_only = 0;
                if (clear > 0) rc = t->del(t, act->rpath, act->is_dir ? NULL : &cond);
                if (clear == 0) {
                    act->skipped = 1;
                    ui_notify(UI_WARN, "%s enthaelt auf dem Server Neues -- nicht geloescht", act->rpath);
                } else if (rc == SYNC_PUT_OK) {
                    out->deleted++;
                } else if (rc == SYNC_PUT_CONFLICT) {
                    /* S8: changed on the server since the scan -- keep it */
                    act->skipped = 1;
                    out->conflicts++;
                    new_st.had_conflicts = 1;
                    ui_notify(UI_WARN, "%s wurde inzwischen auf dem Server geaendert -- nicht geloescht",
                              act->rpath);
                } else {
                    act->failed = 1; out->errors++;
                    ac_log_printf("sync: Loeschen (remote) fehlgeschlagen: %s\n", act->rpath);
                }
            }
            act->done = 1;
            if (!dry_run && (++cp_actions >= cp_every ||
                             (long)time(NULL) - cp_time >= SYNC_CHECKPOINT_SECS)) {
                save_checkpoint(local_root, &new_st, &old_st, &acts);
                cp_actions = 0;
                cp_time = (long)time(NULL);
            }
        }
        free(del_ptrs);
    } else {
        /* no memory for the delete order: delete nothing this time, and
         * roll the deletions back in the baseline so the next run redoes
         * them -- otherwise they would vanish from the baseline and the
         * next run would bring the files back */
        for (i = 0; i < acts.n; i++)
            if (acts.items[i].type == SA_DEL_LOCAL || acts.items[i].type == SA_DEL_REMOTE)
                acts.items[i].skipped = 1;
    }

    /* Baseline erst nach vollstaendigem Erfolg schreiben: roll back the
     * predicted entry for anything that didn't actually happen, then
     * persist -- never on a dry run, never advanced past a failed path. */
    for (i = 0; i < acts.n; i++)
        if (acts.items[i].failed || acts.items[i].skipped || acts.items[i].declined) incomplete = 1;
    if (incomplete) {
        SyncState eff;
        if (build_effective_state(&eff, &new_st, &old_st, &acts, 0)) {
            syncstate_free(&new_st);
            new_st = eff;
        } else {
            for (i = 0; i < acts.n; i++)   /* no memory: the slow way */
                if (acts.items[i].failed || acts.items[i].skipped)
                    rollback_baseline_entry(&new_st, &old_st, acts.items[i].relpath);
        }
    }
    if (stopped) {
        out->aborted = 1;
        ui_notify(UI_WARN, "Abgleich abgebrochen, der Rest folgt beim naechsten Lauf");
    }

    /* S9: the counter as of the START of this run -- a change made by
     * someone else while this run was busy is newer than that and makes
     * the next run scan fully. Own changes cost one full rescan, where
     * the hashes recognise them as already known.
     * S14: after any failed or skipped action the old counter stays, so
     * the next run scans fully and retries (a failed download would
     * otherwise never be seen again). */
    new_st.global_revision = (status_ok && !incomplete) ? server_global
                                                        : old_st.global_revision;

    if (!dry_run) {
        if (!syncstate_save(local_root, &new_st)) {
            if (verbose)
                ui_notify(UI_WARN, "amicloudcore: sync: could not write %s -- next sync will rescan fully",
                         SYNCSTATE_FILE);
            ac_log_printf("sync: Baseline (%s) konnte nicht geschrieben werden -- naechster Sync scannt komplett neu\n",
                          SYNCSTATE_FILE);
        }
    }

out_free:
    case_skips_free(&case_skips);
    namemap_free(&namemap);
    index_free(&ridx); index_free(&lidx);
    syncstate_free(&old_st);
    syncstate_free(&new_st);
    actions_free(&acts);

    snprintf(summary, sizeof(summary),
            "%d hochgeladen, %d heruntergeladen, %d unveraendert, %d geloescht, %d Konflikt(e)",
            out->uploaded, out->downloaded, out->unchanged, out->deleted, out->conflicts);
    if (out->copies) {
        char cpsuf[48];
        snprintf(cpsuf, sizeof(cpsuf), ", %d Konfliktkopie(n)", out->copies);
        strncat(summary, cpsuf, sizeof(summary) - strlen(summary) - 1);
    }
    if (out->kept) {
        char kpsuf[48];
        snprintf(kpsuf, sizeof(kpsuf), ", %d Loeschung(en) zurueckgehalten", out->kept);
        strncat(summary, kpsuf, sizeof(summary) - strlen(summary) - 1);
    }
    if (out->viruses) {
        char virsuf[40];
        snprintf(virsuf, sizeof(virsuf), ", %d VIRUS", out->viruses);
        strncat(summary, virsuf, sizeof(summary) - strlen(summary) - 1);
    }
    if (out->skipped) {
        char skipsuf[40];
        snprintf(skipsuf, sizeof(skipsuf), ", %d uebersprungen", out->skipped);
        strncat(summary, skipsuf, sizeof(summary) - strlen(summary) - 1);
    }
    if (out->errors) {
        char errsuf[32];
        snprintf(errsuf, sizeof(errsuf), ", %d Fehler", out->errors);
        strncat(summary, errsuf, sizeof(summary) - strlen(summary) - 1);
    }
    ui_notify(UI_INFO, "%s", summary);
    ac_log_printf("sync: %s\n", summary);
}
