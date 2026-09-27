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
 * Transport-neutral since the WebDAV/OwnCloud work (see transport.h):
 * the reconcile kernel below (cloudsync_run_t and everything it calls)
 * never touches cmd.h/api.php directly -- it only ever calls through a
 * SyncTransport. cmd.h is included below solely to build the small
 * amicloud_* adapter functions at the bottom of this file, which give
 * the original cloudsync_run() its AmiCloud (api.php) transport so every
 * existing caller keeps working unchanged.
 */
#include "sync.h"
#include "cmd.h"
#include "http.h" /* ac_log_printf() -- see the out->errors++ sites below */
#include "localfs.h"
#include "ui_hooks.h"
#include "md5.h"
#include "syncstate.h"

/* ac_log_printf() is only declared under PLATFORM_AMIGA in http.h (see
 * cmd.c, which guards every one of its own call sites with #ifdef
 * PLATFORM_AMIGA/#endif) -- but this file also builds for PC/Mac/Linux
 * (cloudsync, webdavsync, amicloud-shell all link it via Makefile.pc).
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

static void index_add(SyncIndex *idx, const char *path, int is_dir,
                      long size, long mtime, const char *md5, const char *token,
                      long revision, long id)
{
    SyncEntry *e;
    if (idx->n == idx->cap) {
        int newcap = idx->cap ? idx->cap * 2 : 64;
        SyncEntry *tmp = (SyncEntry *)realloc(idx->items, sizeof(SyncEntry) * (size_t)newcap);
        if (!tmp) return;
        idx->items = tmp;
        idx->cap = newcap;
    }
    e = &idx->items[idx->n++];
    e->path = sync_dupstr(path);
    e->is_dir = is_dir;
    e->size = size;
    e->mtime = mtime;
    strncpy(e->md5, md5 ? md5 : "", sizeof(e->md5) - 1);
    e->md5[sizeof(e->md5) - 1] = '\0';
    strncpy(e->token, token ? token : "", sizeof(e->token) - 1);
    e->token[sizeof(e->token) - 1] = '\0';
    e->revision = revision;
    e->id = id;
}

static void index_free(SyncIndex *idx)
{
    int i;
    for (i = 0; i < idx->n; i++) free(idx->items[i].path);
    free(idx->items);
    idx->items = NULL; idx->n = idx->cap = 0;
}

static int sync_entry_cmp(const void *a, const void *b)
{
    return strcmp(((const SyncEntry *)a)->path, ((const SyncEntry *)b)->path);
}

static int syncstate_entry_cmp(const void *a, const void *b)
{
    return strcmp(((const SyncStateEntry *)a)->path, ((const SyncStateEntry *)b)->path);
}

/* Recursively lists the remote tree into idx, one extra `list` request
 * per subdirectory. Relative paths so a plain strcmp() sort always puts
 * a directory right before its own children (a string is always < any
 * string it's a strict prefix of). Goes through the transport, not any
 * one wire protocol -- see transport.h. */
static void index_remote_tree_sync(SyncTransport *t, const char *remote_root,
                                   const char *rel_prefix, SyncIndex *idx)
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

    if (!t->list(t, remote_path, &entries, &n)) return;
    for (i = 0; i < n; i++) {
        char relchild[AC_MAX_PATH];
        if (rel_prefix[0])
            snprintf(relchild, sizeof(relchild), "%s/%s", rel_prefix, entries[i].name);
        else {
            strncpy(relchild, entries[i].name, sizeof(relchild) - 1);
            relchild[sizeof(relchild) - 1] = '\0';
        }
        index_add(idx, relchild, entries[i].is_dir, entries[i].size, entries[i].mtime,
                 entries[i].md5, entries[i].token, entries[i].revision, entries[i].id);
        if (entries[i].is_dir) index_remote_tree_sync(t, remote_root, relchild, idx);
    }
    free(entries);
}

/* Delta-optimisation fast path (see cloudsync_run()): when the server's
 * global_revision hasn't moved since our last successful sync, the
 * remote tree is by definition identical to what it was back then --
 * reuse the baseline itself as the remote index instead of a full
 * recursive `list` scan. */
static void index_remote_from_baseline(SyncState *baseline, SyncIndex *idx)
{
    int i;
    for (i = 0; i < baseline->n; i++) {
        const SyncStateEntry *e = &baseline->items[i];
        index_add(idx, e->path, e->is_dir, e->size, e->mtime, e->md5, e->token, e->revision, e->server_id);
    }
}

/* Timestamps count as equal within +/-2s — FAT resolution, Amiga ticks,
 * rounding during transfer. */
static int mtimes_equal(long a, long b)
{
    long d = a - b;
    if (d < 0) d = -d;
    return d <= 2;
}

/* Local counterpart to index_remote_tree_sync() — purely local
 * traversal, effectively free. Works fine on AmigaOS too — dos.library
 * Lock() accepts ordinary multi-component "dir/sub/file" paths.
 *
 * MD5 cache (AmiOwnCloud docs/design/07_Sync_Metadaten.md): a file whose size
 * AND mtime still match its baseline entry is presumed unchanged and
 * reuses the baseline's cached hash instead of rehashing; only a file
 * that actually looks different gets a fresh md5_file_hex(). */
static void index_local_tree_sync(const char *local_root, const char *rel_prefix,
                                  SyncIndex *idx, SyncState *baseline)
{
    LocalDir *d;
    LocalDirEntry e;
    char local_path[AC_MAX_PATH];

    if (rel_prefix[0])
        localfs_join(local_path, sizeof(local_path), local_root, rel_prefix);
    else {
        strncpy(local_path, local_root, sizeof(local_path) - 1);
        local_path[sizeof(local_path) - 1] = '\0';
    }

    d = localfs_opendir(local_path);
    if (!d) return;
    while (localfs_readdir(d, &e)) {
        char relchild[AC_MAX_PATH];
        char md5hex[33];
        SyncStateEntry *cached;

        if (rel_prefix[0])
            snprintf(relchild, sizeof(relchild), "%s/%s", rel_prefix, e.name);
        else {
            strncpy(relchild, e.name, sizeof(relchild) - 1);
            relchild[sizeof(relchild) - 1] = '\0';
        }

        /* Never treat our own baseline file (or its atomic-save tempfile)
         * as sync content -- otherwise a sync would upload/track itself. */
        if (!rel_prefix[0] && (strcmp(e.name, SYNCSTATE_FILE) == 0 ||
                              strcmp(e.name, SYNCSTATE_FILE ".tmp") == 0))
            continue;

        md5hex[0] = '\0';
        if (!e.is_dir) {
            cached = syncstate_find(baseline, relchild);
            if (cached && cached->size == e.size && mtimes_equal(cached->mtime, e.mtime)) {
                strncpy(md5hex, cached->md5, sizeof(md5hex) - 1);
                md5hex[sizeof(md5hex) - 1] = '\0';
            } else {
                char local_full[AC_MAX_PATH];
                localfs_join(local_full, sizeof(local_full), local_root, relchild);
                if (!md5_file_hex(local_full, md5hex)) md5hex[0] = '\0';
            }
        }

        index_add(idx, relchild, e.is_dir, e.size, e.mtime, md5hex, NULL, 0, 0);
        if (e.is_dir) index_local_tree_sync(local_root, relchild, idx, baseline);
    }
    localfs_closedir(d);
}

/* ------------------------------------------------------------------ */
/* Actions decided in Pass 1, executed in Pass 2.                        */
/* ------------------------------------------------------------------ */
typedef enum {
    SA_MKDIR_LOCAL, SA_MKDIR_REMOTE, SA_UPLOAD, SA_DOWNLOAD,
    SA_ALIGN_LOCAL, SA_ALIGN_REMOTE, SA_DEL_LOCAL, SA_DEL_REMOTE
} SyncActionType;

typedef struct {
    SyncActionType type;
    char relpath[AC_MAX_PATH]; /* relative to the sync root -- the baseline key */
    char lpath[AC_MAX_PATH];
    char rpath[AC_MAX_PATH];
    long mtime; /* target mtime after copy/alignment */
    long size;  /* UPLOAD/DOWNLOAD only, for the byte-percentage */
    int  failed; /* set in Pass 2 on failure -- triggers a baseline rollback */
} SyncAction;

typedef struct { SyncAction *items; int n, cap; } SyncActions;

static void actions_init(SyncActions *a) { a->items = NULL; a->n = 0; a->cap = 0; }

static SyncAction *actions_add(SyncActions *a, SyncActionType type)
{
    SyncAction *act;
    if (a->n == a->cap) {
        int newcap = a->cap ? a->cap * 2 : 32;
        SyncAction *tmp = (SyncAction *)realloc(a->items, sizeof(SyncAction) * (size_t)newcap);
        if (!tmp) return NULL;
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

static void queue_upload(SyncActions *acts, const SyncEntry *le, const char *relpath,
                        const char *lpath, const char *rpath)
{
    SyncAction *act = queue_action(acts, SA_UPLOAD, relpath);
    if (!act) return;
    strncpy(act->lpath, lpath, sizeof(act->lpath) - 1);
    strncpy(act->rpath, rpath, sizeof(act->rpath) - 1);
    act->mtime = le->mtime;
    act->size  = le->size;
}

static void queue_download(SyncActions *acts, const SyncEntry *re, const char *relpath,
                          const char *lpath, const char *rpath)
{
    SyncAction *act = queue_action(acts, SA_DOWNLOAD, relpath);
    if (!act) return;
    strncpy(act->lpath, lpath, sizeof(act->lpath) - 1);
    strncpy(act->rpath, rpath, sizeof(act->rpath) - 1);
    act->mtime = re->mtime;
    act->size  = re->size;
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

static void queue_del_local(SyncActions *acts, const char *relpath, const char *lpath)
{
    SyncAction *act = queue_action(acts, SA_DEL_LOCAL, relpath);
    if (act) strncpy(act->lpath, lpath, sizeof(act->lpath) - 1);
}

static void queue_del_remote(SyncActions *acts, const char *relpath, const char *rpath)
{
    SyncAction *act = queue_action(acts, SA_DEL_REMOTE, relpath);
    if (act) strncpy(act->rpath, rpath, sizeof(act->rpath) - 1);
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

void cloudsync_run_t(SyncTransport *t, const char *local_root, const char *remote_root,
                     int dry_run, int verbose, Progress *pr, SyncSummary *out)
{
    SyncState   old_st, new_st;
    SyncIndex   ridx, lidx;
    SyncActions acts;
    SyncAction  **del_ptrs;
    long        server_global = 0;
    long        now;
    int         status_ok, use_baseline_as_remote;
    int         ri, li, bi, i, del_n;
    char        summary[256];

    memset(out, 0, sizeof(*out));
    now = (long)time(NULL);

    if (verbose)
        ui_notify(UI_INFO, "Sync: %s <-> %s%s", local_root, remote_root,
                 dry_run ? "  (Probelauf, keine Aenderungen)" : "");
    /* Unconditional (not gated on verbose, which callers like the
     * AmiCloudSync commodity always pass 0) -- a no-op unless the caller
     * wired http_log_bptr/http_log_path from cfg.debug (see
     * ac_log_printf()'s own doc comment), but when it is, this is what
     * ties the per-failure ac_log_printf() calls below to which sync run
     * and which pair they belong to. */
    ac_log_printf("sync: %s <-> %s%s\n", local_root, remote_root,
                  dry_run ? " (Probelauf)" : "");

    /* Both sides auto-created if missing (like get -r/put -r already do)
     * -- but never in a dry run, which must never touch anything. */
    if (!dry_run) {
        localfs_mkdir_p(local_root);
        t->mkdir_quiet(t, remote_root);
    }

    syncstate_load(local_root, &old_st);
    qsort(old_st.items, (size_t)old_st.n, sizeof(SyncStateEntry), syncstate_entry_cmp);

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
    use_baseline_as_remote = status_ok && !old_st.had_conflicts &&
                            (server_global == old_st.global_revision);

    index_init(&ridx); index_init(&lidx);
    if (use_baseline_as_remote)
        index_remote_from_baseline(&old_st, &ridx);
    else
        index_remote_tree_sync(t, remote_root, "", &ridx);
    index_local_tree_sync(local_root, "", &lidx, &old_st);
    qsort(ridx.items, (size_t)ridx.n, sizeof(SyncEntry), sync_entry_cmp);
    qsort(lidx.items, (size_t)lidx.n, sizeof(SyncEntry), sync_entry_cmp);

    actions_init(&acts);
    memset(&new_st, 0, sizeof(new_st));
    new_st.global_revision = status_ok ? server_global : old_st.global_revision;

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

        localfs_join(lpath, sizeof(lpath), local_root, cur);
        snprintf(rpath, sizeof(rpath), "%s/%s", remote_root, cur);

        if (le && re) {
            if (le->is_dir != re->is_dir) {
                report_conflict(verbose, out, &new_st, cur, "file vs. directory");
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
                } else {
                    report_conflict(verbose, out, &new_st, cur,
                                    "unrelated content on both sides, no prior sync baseline");
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
                int remote_changed = (be->revision > 0 && re->revision > 0)
                                     ? (re->revision != be->revision)
                                     : re->md5[0]
                                       ? (strcmp(re->md5, be->md5) != 0)
                                       : (be->token[0] && strcmp(re->token, be->token) != 0);

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
                    queue_upload(&acts, le, cur, lpath, rpath);
                    /* token left empty: we don't know WebDAV's new ETag
                     * without an extra request, and don't need to -- see
                     * the "unchanged" branch's comment above. */
                    syncstate_upsert(&new_st, cur, 0, le->size, le->mtime, le->md5, "",
                                    be->revision + 1, be->server_id, now);
                } else if (!local_changed && remote_changed) {
                    queue_download(&acts, re, cur, lpath, rpath);
                    /* md5 may be "" here (WebDAV) -- Pass 2's SA_DOWNLOAD
                     * case patches it with a real local hash right after
                     * the transfer completes, at zero network cost. */
                    syncstate_upsert(&new_st, cur, 0, re->size, re->mtime, re->md5, re->token,
                                    re->revision, re->id, now);
                } else if (re->md5[0] && strcmp(le->md5, re->md5) == 0) {
                    out->unchanged++; /* both changed but converged independently */
                    syncstate_upsert(&new_st, cur, 0, re->size, re->mtime, re->md5, re->token,
                                    re->revision, re->id, now);
                } else {
                    report_conflict(verbose, out, &new_st, cur, "different content on both sides since last sync");
                    carry_forward(&new_st, be);
                }
            }
        } else if (le && !re) {
            if (be) {
                int unchanged_for_del;
                if (le->is_dir)      unchanged_for_del = (be->is_dir == 1);
                else if (be->is_dir) unchanged_for_del = 0; /* type mismatch vs. baseline */
                else                 unchanged_for_del = (strcmp(le->md5, be->md5) == 0);

                if (unchanged_for_del) {
                    queue_del_local(&acts, cur, lpath);
                } else {
                    report_conflict(verbose, out, &new_st, cur, "deleted remotely but changed locally");
                    carry_forward(&new_st, be);
                }
            } else if (le->is_dir) {
                queue_mkdir_remote(&acts, cur, rpath);
                syncstate_upsert(&new_st, cur, 1, 0, le->mtime, "", "", 1, 0, now);
            } else {
                queue_upload(&acts, le, cur, lpath, rpath);
                syncstate_upsert(&new_st, cur, 0, le->size, le->mtime, le->md5, "", 1, 0, now);
            }
        } else if (!le && re) {
            if (be) {
                int unchanged_for_del;
                if (re->is_dir)      unchanged_for_del = (be->is_dir == 1);
                else if (be->is_dir) unchanged_for_del = 0;
                else if (be->revision > 0 && re->revision > 0)
                    unchanged_for_del = (re->revision == be->revision);
                else if (re->md5[0])
                    unchanged_for_del = (strcmp(re->md5, be->md5) == 0);
                else
                    unchanged_for_del = !be->token[0] || (strcmp(re->token, be->token) == 0);

                if (unchanged_for_del) {
                    queue_del_remote(&acts, cur, rpath);
                } else {
                    report_conflict(verbose, out, &new_st, cur, "deleted locally but changed remotely");
                    carry_forward(&new_st, be);
                }
            } else if (re->is_dir) {
                queue_mkdir_local(&acts, cur, lpath);
                syncstate_upsert(&new_st, cur, 1, 0, re->mtime, "", re->token, re->revision, re->id, now);
            } else {
                queue_download(&acts, re, cur, lpath, rpath);
                /* md5 patched post-transfer in Pass 2, see above. */
                syncstate_upsert(&new_st, cur, 0, re->size, re->mtime, re->md5, re->token,
                                re->revision, re->id, now);
            }
        }
        /* else: !le && !re, only be -- both sides already gone; drop the
         * baseline entry silently (nothing added to new_st), done. */
    }
    index_free(&ridx); index_free(&lidx);

    /* Pass 2a: mkdir/upload/download/align, same order and progress
     * bookkeeping as before this rewrite. */
    progress_reset(pr);
    for (i = 0; i < acts.n; i++) {
        if (acts.items[i].type == SA_UPLOAD || acts.items[i].type == SA_DOWNLOAD) {
            pr->total++;
            pr->bytes_total += acts.items[i].size;
        }
    }

    for (i = 0; i < acts.n; i++) {
        SyncAction *act = &acts.items[i];
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
                progress_prefix(pr, act->size);
                if (t->put(t, act->lpath, act->rpath, act->mtime) == 0) {
                    out->uploaded++;
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
                progress_prefix(pr, act->size);
                if (t->get(t, act->rpath, act->lpath) == 0) {
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
        case SA_DEL_LOCAL:
        case SA_DEL_REMOTE:
            break; /* handled leaf-first in Pass 2b below */
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
            if (dry_run) {
                if (verbose)
                    ui_notify(UI_INFO, "(waere geloescht, %s)     %s",
                             act->type == SA_DEL_LOCAL ? "lokal" : "remote", act->relpath);
                out->deleted++;
            } else if (act->type == SA_DEL_LOCAL) {
                if (localfs_remove(act->lpath)) out->deleted++;
                else {
                    act->failed = 1; out->errors++;
                    ac_log_printf("sync: Loeschen (lokal) fehlgeschlagen: %s\n", act->lpath);
                }
            } else {
                if (t->del(t, act->rpath) == 0) out->deleted++;
                else {
                    act->failed = 1; out->errors++;
                    ac_log_printf("sync: Loeschen (remote) fehlgeschlagen: %s\n", act->rpath);
                }
            }
        }
        free(del_ptrs);
    }

    /* Baseline erst nach vollstaendigem Erfolg schreiben: roll back the
     * predicted entry for anything that didn't actually happen, then
     * persist -- never on a dry run, never advanced past a failed path. */
    for (i = 0; i < acts.n; i++) {
        if (acts.items[i].failed)
            rollback_baseline_entry(&new_st, &old_st, acts.items[i].relpath);
    }

    if (!dry_run) {
        /* Re-query global_revision now rather than reuse the pre-sync
         * value: if THIS run itself uploaded/deleted/mkdir'd anything
         * remotely, the counter already moved past what we captured at
         * the top -- recording the stale value would just cost the next
         * run one wasted full rescan before the delta-optimisation
         * catches up on its own. Not a correctness fix, purely keeps the
         * shortcut effective sync-over-sync. */
        long final_global;
        if (t->global_rev(t, &final_global)) new_st.global_revision = final_global;

        if (!syncstate_save(local_root, &new_st)) {
            if (verbose)
                ui_notify(UI_WARN, "amicloudcore: sync: could not write %s -- next sync will rescan fully",
                         SYNCSTATE_FILE);
            ac_log_printf("sync: Baseline (%s) konnte nicht geschrieben werden -- naechster Sync scannt komplett neu\n",
                          SYNCSTATE_FILE);
        }
    }

    syncstate_free(&old_st);
    syncstate_free(&new_st);
    actions_free(&acts);

    snprintf(summary, sizeof(summary),
            "%d hochgeladen, %d heruntergeladen, %d unveraendert, %d geloescht, %d Konflikt(e)",
            out->uploaded, out->downloaded, out->unchanged, out->deleted, out->conflicts);
    if (out->errors) {
        char errsuf[32];
        snprintf(errsuf, sizeof(errsuf), ", %d Fehler", out->errors);
        strncat(summary, errsuf, sizeof(summary) - strlen(summary) - 1);
    }
    ui_notify(UI_INFO, "%s", summary);
    ac_log_printf("sync: %s\n", summary);
}

/* ------------------------------------------------------------------ */
/* AmiCloud (api.php) transport -- adapts the existing cmd_*() calls to   */
/* SyncTransport, purely so the original cloudsync_run() below can keep   */
/* every caller working unchanged. Not a new feature, not part of any     */
/* public header -- this repo's own AmiCloud server/commodity is a         */
/* separate, later deliverable that this WebDAV work does not touch.      */
/* ------------------------------------------------------------------ */

static int amicloud_list(SyncTransport *t, const char *remote_path,
                         SyncRemoteEntry **out, int *out_n)
{
    AcEntry *entries;
    int n, i;

    *out = NULL; *out_n = 0;
    if (!cmd_list_raw(t->cfg, remote_path, &entries, &n)) return 0;

    if (n > 0) {
        SyncRemoteEntry *arr = (SyncRemoteEntry *)calloc((size_t)n, sizeof(SyncRemoteEntry));
        if (!arr) { cmd_entries_free(entries); return 0; }
        for (i = 0; i < n; i++) {
            strncpy(arr[i].name, entries[i].name, sizeof(arr[i].name) - 1);
            arr[i].is_dir   = entries[i].is_dir;
            arr[i].size     = entries[i].size;
            arr[i].mtime    = entries[i].mtime;
            /* AmiCloud provides a real content hash -- populate both
             * md5 (compared against local hashes/no-baseline checks) and
             * token (compared against the baseline's remembered remote
             * marker) with it; see transport.h/syncstate.h for why these
             * two roles must stay independently populated in general. */
            strncpy(arr[i].md5, entries[i].md5, sizeof(arr[i].md5) - 1);
            strncpy(arr[i].token, entries[i].md5, sizeof(arr[i].token) - 1);
            arr[i].revision = entries[i].revision;
            arr[i].id       = entries[i].id;
        }
        *out = arr;
    }
    *out_n = n;
    cmd_entries_free(entries);
    return 1;
}

static int amicloud_get(SyncTransport *t, const char *remote_path, const char *local_path)
{
    return cmd_get(t->cfg, remote_path, local_path);
}

/* Two internal requests, exactly like the pre-refactor code: setdate's
 * return value is deliberately ignored (fire-and-forget), unchanged
 * behaviour. */
static int amicloud_put(SyncTransport *t, const char *local_path,
                        const char *remote_path, long mtime)
{
    int rc = cmd_put(t->cfg, local_path, remote_path);
    if (rc == 0) cmd_setdate_mtime(t->cfg, remote_path, mtime);
    return rc;
}

static int amicloud_del(SyncTransport *t, const char *remote_path)
{
    return cmd_del(t->cfg, remote_path);
}

static int amicloud_mkdir_quiet(SyncTransport *t, const char *remote_path)
{
    return cmd_mkdir_quiet(t->cfg, remote_path);
}

static int amicloud_setdate(SyncTransport *t, const char *remote_path, long mtime)
{
    return cmd_setdate_mtime(t->cfg, remote_path, mtime);
}

static int amicloud_global_rev(SyncTransport *t, long *global_out)
{
    return cmd_status(t->cfg, NULL, 0, global_out);
}

/* Original entry point -- see sync.h. Builds the AmiCloud transport
 * above and hands off to the transport-neutral kernel; signature and
 * behaviour are exactly what every existing caller already expects. */
void cloudsync_run(const AcConfig *cfg, const char *local_root, const char *remote_root,
                   int dry_run, int verbose, Progress *pr, SyncSummary *out)
{
    SyncTransport t;

    t.cfg         = cfg;
    t.list        = amicloud_list;
    t.get         = amicloud_get;
    t.put         = amicloud_put;
    t.del         = amicloud_del;
    t.mkdir_quiet = amicloud_mkdir_quiet;
    t.setdate     = amicloud_setdate;
    t.global_rev  = amicloud_global_rev;
    t.close       = NULL;

    cloudsync_run_t(&t, local_root, remote_root, dry_run, verbose, pr, out);
}
