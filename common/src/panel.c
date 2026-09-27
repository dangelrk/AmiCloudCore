/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * panel.c — see panel.h. Path resolution and recursive get/put moved
 * here from shell.c (GUI-Commander-plan section 5.4); panel_read()/
 * panel_sort()/panel_marked_size() are new, written for the GUI but
 * equally usable by any future console feature that wants both sides of
 * a copy expressed uniformly.
 *
 * Copy of the old client/panel.c (docs/05_Refactoring_Client_Handler.md,
 * M5) with the two-way sync engine removed (now libcloudsync) and
 * progress_reset()/progress_prefix() removed (now libcloudcore's
 * progress.c — this file just calls them).
 */
#include "panel.h"
#include "localfs.h"
#include "ui_hooks.h"
#include "md5.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Remote path resolution (moved verbatim from shell.c)                 */
/* ------------------------------------------------------------------ */
static void path_pop(char *p)
{
    char *slash = strrchr(p, '/');
    if (slash) *slash = '\0';
    else p[0] = '\0';
}

static void path_push(char *p, size_t sz, const char *seg)
{
    size_t len = strlen(p);
    int need_sep = (len > 0 && p[len - 1] != '/');
    if (len + (size_t)need_sep + strlen(seg) < sz) {
        if (need_sep) p[len++] = '/';
        strcpy(p + len, seg);
    }
}

void remote_resolve(char *out, size_t outsz, const char *cwd, const char *input)
{
    char work[AC_MAX_PATH];
    const char *rest;
    int is_amiga;
#ifdef PLATFORM_AMIGA
    is_amiga = 1;
#else
    is_amiga = 0;
#endif

    /* Whole-token shortcuts that are unambiguous everywhere. */
    if (strcmp(input, ":") == 0 || strcmp(input, "\\") == 0) {
        strncpy(out, "/", outsz - 1); out[outsz - 1] = '\0';
        return;
    }
    if (strcmp(input, "..") == 0) {
        strncpy(work, cwd, sizeof(work) - 1); work[sizeof(work) - 1] = '\0';
        path_pop(work);
        strncpy(out, work[0] ? work : "/", outsz - 1); out[outsz - 1] = '\0';
        return;
    }
    /* The one real conflict: '/' means "up" on Amiga, "root" elsewhere. */
    if (strcmp(input, "/") == 0) {
        if (is_amiga) {
            strncpy(work, cwd, sizeof(work) - 1); work[sizeof(work) - 1] = '\0';
            path_pop(work);
            strncpy(out, work[0] ? work : "/", outsz - 1); out[outsz - 1] = '\0';
        } else {
            strncpy(out, "/", outsz - 1); out[outsz - 1] = '\0';
        }
        return;
    }

    /* General case: a (possibly relative, possibly '..'-containing) path. */
    rest = input;
    if (input[0] == '/') {
        if (is_amiga) {
            strncpy(work, cwd, sizeof(work) - 1); work[sizeof(work) - 1] = '\0';
            while (*rest == '/') { path_pop(work); rest++; }
        } else {
            work[0] = '/'; work[1] = '\0'; /* absolute from root */
            while (*rest == '/') rest++;
        }
    } else {
        strncpy(work, cwd, sizeof(work) - 1); work[sizeof(work) - 1] = '\0';
    }

    {
        char buf[AC_MAX_PATH];
        char *seg;
        strncpy(buf, rest, sizeof(buf) - 1); buf[sizeof(buf) - 1] = '\0';
        seg = strtok(buf, "/");
        while (seg) {
            if (strcmp(seg, "..") == 0) path_pop(work);
            else if (strcmp(seg, ".") != 0 && seg[0] != '\0') path_push(work, sizeof(work), seg);
            seg = strtok(NULL, "/");
        }
    }

    strncpy(out, work[0] ? work : "/", outsz - 1); out[outsz - 1] = '\0';
}

void count_remote_tree(const AcConfig *cfg, const char *remote_path, Progress *p)
{
    AcEntry *entries;
    int n, i;

    /* One ui_progress() ping per folder visited — on the Amiga this is
     * the only thing that pumps MUIM_Application_NewInput (and so notices
     * a Cancel click) during the scan phase, which otherwise does nothing
     * but back-to-back `list` round-trips with no event processing at
     * all. A folder-heavy tree (many subfolders, few files) could
     * previously leave Cancel looking completely dead for the whole scan
     * — see do_copy()'s "Ermittle Dateien..." phase. total==-1: this is a
     * pump, not a real byte count, and its own return doubles as an
     * early-exit so a cancelled scan doesn't keep walking a huge tree
     * just to throw the count away. */
    if (!ui_progress(0, -1, remote_path)) return;
    if (!cmd_list_raw(cfg, remote_path, &entries, &n)) return;
    for (i = 0; i < n; i++) {
        if (entries[i].is_dir) {
            char child[AC_MAX_PATH];
            snprintf(child, sizeof(child), "%s/%s", remote_path, entries[i].name);
            count_remote_tree(cfg, child, p);
        } else {
            p->total++;
            p->bytes_total += entries[i].size;
        }
    }
    cmd_entries_free(entries);
}

void count_local_tree(const char *local_path, Progress *p)
{
    LocalDir *d;
    LocalDirEntry e;

    /* Local walking has no network latency, but a very large tree can
     * still take a visible moment — same pump-per-folder reasoning as
     * count_remote_tree() above. */
    if (!ui_progress(0, -1, local_path)) return;
    d = localfs_opendir(local_path);
    if (!d) return;
    while (localfs_readdir(d, &e)) {
        if (e.is_dir) {
            char child[AC_MAX_PATH];
            localfs_join(child, sizeof(child), local_path, e.name);
            count_local_tree(child, p);
        } else {
            p->total++;
            p->bytes_total += e.size;
        }
    }
    localfs_closedir(d);
}

/* ------------------------------------------------------------------ */
/* Copy orchestration with per-file/per-folder overwrite policy — see    */
/* panel.h. Replaces the old bare-Progress panel_get_dir()/panel_put_dir() */
/* (moved from shell.c; fprintf -> ui_notify since this is now core code */
/* a GUI links too).                                                      */
/* ------------------------------------------------------------------ */
void copyctx_init(CopyCtx *c, int ask)
{
    progress_reset(&c->pr);
    c->ask       = ask;
    c->policy    = -1;
    c->cancelled = 0;
}

/* AcEntry lookup by name — dst_entries came from one cmd_list_raw() call
 * for the whole destination directory, so this is an in-memory scan, no
 * extra round-trip per file. */
static const AcEntry *find_entry(const AcEntry *entries, int n, const char *name)
{
    int i;
    for (i = 0; i < n; i++)
        if (!entries[i].is_dir && strcmp(entries[i].name, name) == 0) return &entries[i];
    return NULL;
}

static const LocalDirEntry *find_local_entry(const LocalDirEntry *entries, int n, const char *name)
{
    int i;
    for (i = 0; i < n; i++)
        if (!entries[i].is_dir && strcmp(entries[i].name, name) == 0) return &entries[i];
    return NULL;
}

/* One-shot local directory snapshot (mirrors what cmd_list_raw() gives
 * the remote side) — lets panel_get_dir() look up "does this file already
 * exist, and with what size/date" without a stat() per entry. Returns
 * NULL (with *n_out==0) if path doesn't exist or is empty, same as an
 * empty cmd_list_raw() result. */
static LocalDirEntry *local_dir_snapshot(const char *path, int *n_out)
{
    LocalDir *d = localfs_opendir(path);
    LocalDirEntry *arr = NULL;
    LocalDirEntry e;
    int n = 0, cap = 0;

    *n_out = 0;
    if (!d) return NULL;
    while (localfs_readdir(d, &e)) {
        if (n == cap) {
            int newcap = cap ? cap * 2 : 16;
            LocalDirEntry *tmp = (LocalDirEntry *)realloc(arr, sizeof(LocalDirEntry) * (size_t)newcap);
            if (!tmp) break;
            arr = tmp;
            cap = newcap;
        }
        arr[n++] = e;
    }
    localfs_closedir(d);
    *n_out = n;
    return arr;
}

static const char *basename_of(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

void panel_copy_file(PanelSource ssrc, const AcConfig *scfg, const char *spath,
                     PanelSource dsrc, const AcConfig *dcfg, const char *dpath,
                     const char *name, const FileMeta *smeta,
                     const FileMeta *dmeta, CopyCtx *c)
{
    if (c->cancelled) return;

    if (c->ask && dmeta) {
        UiOverwrite ow;
        if (c->policy != -1) {
            ow = (UiOverwrite)c->policy;
        } else {
            /* FileMeta.md5 is always "" for whichever side is local (see
             * panel.h) — hash it here, on demand, only for the one file
             * we're actually about to prompt about (not eagerly for every
             * file, and never once "Alle ueberschreiben/-springen" already
             * made c->policy moot, per the branch above), so the overwrite
             * dialog can show whether the transfer would genuinely change
             * anything instead of leaving the local side's MD5 blank. */
            char src_local_md5[33] = "", dst_local_md5[33] = "";
            const char *src_md5 = smeta->md5;
            const char *dst_md5 = dmeta->md5;
            if (ssrc == PANEL_LOCAL && md5_file_hex(spath, src_local_md5))
                src_md5 = src_local_md5;
            if (dsrc == PANEL_LOCAL && md5_file_hex(dpath, dst_local_md5))
                dst_md5 = dst_local_md5;
            ow = ui_overwrite(0, name,
                              smeta->size, smeta->date, src_md5,
                              dmeta->size, dmeta->date, dst_md5);
        }
        if (ow == UI_OW_OVERWRITE_ALL || ow == UI_OW_SKIP_ALL) c->policy = (int)ow;
        if (ow == UI_OW_CANCEL) { c->cancelled = 1; return; }
        if (ow == UI_OW_SKIP || ow == UI_OW_SKIP_ALL) {
            /* Still counts toward x/y and pokes the progress hook (Cancel-
             * button event pump on the Amiga side) even though nothing is
             * transferred — a long run of skips must not look frozen. Its
             * return also means "keep going" like everywhere else ui_progress
             * is called, so a Cancel click that lands between two skips is
             * caught here too, not just between top-level entries. */
            progress_prefix(&c->pr, smeta->size);
            if (!ui_progress(0, smeta->size, name)) c->cancelled = 1;
            return;
        }
        /* UI_OW_OVERWRITE / UI_OW_OVERWRITE_ALL: fall through and copy. */
    }

    progress_prefix(&c->pr, smeta->size);
    if (!ui_progress(0, smeta->size, name)) { c->cancelled = 1; return; }
    if (ssrc == PANEL_LOCAL && dsrc == PANEL_CLOUD) {
        if (cmd_put(dcfg, spath, dpath) == 0) c->pr.ok++; else c->pr.err++;
    } else if (ssrc == PANEL_CLOUD && dsrc == PANEL_LOCAL) {
        if (cmd_get(scfg, spath, dpath) == 0) c->pr.ok++; else c->pr.err++;
    } else {
        /* Local<->Local, Cloud<->Cloud: no case for that exists yet
         * (do_copy()/doCopy() already reject Cloud->Cloud and don't offer
         * Local->Local before ever reaching here). */
        c->pr.err++;
        ui_notify(UI_ERROR, "amicloudcore: unsupported copy direction for %s", name);
    }
}

void panel_get_dir(const AcConfig *cfg, const char *remote_path,
                   const char *local_path, CopyCtx *c)
{
    AcEntry *entries;
    int n, i;
    int dst_existed;

    if (c->cancelled) return;
    /* Per-folder pump: the existence check, cmd_list_raw() and the mkdir
     * in panel_put_dir()'s counterpart below are all plain network round-
     * trips with no chunked ui_progress() callback of their own, unlike
     * the per-file transfer itself — a directory-heavy tree (many
     * subfolders relative to files) could leave a whole visit's worth of
     * round-trips (existence check + list/mkdir + every child's own
     * recursive visit) go by with the Cancel button's click sitting
     * unprocessed the entire time (Amiga: nothing pumps
     * MUIM_Application_NewInput outside a ui_progress() call). */
    if (!ui_progress(0, -1, basename_of(remote_path))) { c->cancelled = 1; return; }

    /* Cheap local existence check (no network) before the remote list —
     * mirrors panel_put_dir()'s cmd_list_raw() existence+snapshot combo. */
    dst_existed = c->ask && localfs_exists(local_path);
    if (dst_existed) {
        UiOverwrite ow = (c->policy != -1) ? (UiOverwrite)c->policy
                        : ui_overwrite(1, basename_of(remote_path), 0, "", "", 0, "", "");
        if (ow == UI_OW_OVERWRITE_ALL || ow == UI_OW_SKIP_ALL) c->policy = (int)ow;
        if (ow == UI_OW_CANCEL) { c->cancelled = 1; return; }
        if (ow == UI_OW_SKIP || ow == UI_OW_SKIP_ALL) return;
        /* UI_OW_OVERWRITE / UI_OW_OVERWRITE_ALL: merge into it below. */
    }

    /* mkdir_p, nicht mkdir: local_path kann auf ein anderes Laufwerk mit
     * fehlenden Elternverzeichnissen zeigen (get -r fotos Work:2026/fotos). */
    if (!localfs_mkdir_p(local_path)) {
        ui_notify(UI_ERROR, "amicloudcore: cannot create local directory %s", local_path);
        c->pr.err++;
        return;
    }
    if (!cmd_list_raw(cfg, remote_path, &entries, &n)) {
        ui_notify(UI_ERROR, "amicloudcore: cannot list %s", remote_path);
        c->pr.err++;
        return;
    }

    {
        int dst_n = 0;
        LocalDirEntry *dst_entries = dst_existed ? local_dir_snapshot(local_path, &dst_n) : NULL;

        for (i = 0; i < n; i++) {
            char rchild[AC_MAX_PATH], lchild[AC_MAX_PATH];
            snprintf(rchild, sizeof(rchild), "%s/%s", remote_path, entries[i].name);
            localfs_join(lchild, sizeof(lchild), local_path, entries[i].name);
            if (entries[i].is_dir) {
                panel_get_dir(cfg, rchild, lchild, c);
            } else {
                FileMeta smeta, dmeta;
                const LocalDirEntry *dle = dst_entries ? find_local_entry(dst_entries, dst_n, entries[i].name) : NULL;

                smeta.size = entries[i].size;
                strncpy(smeta.date, entries[i].modified, sizeof(smeta.date) - 1);
                smeta.date[sizeof(smeta.date) - 1] = '\0';
                strncpy(smeta.md5, entries[i].md5, sizeof(smeta.md5) - 1);
                smeta.md5[sizeof(smeta.md5) - 1] = '\0';

                if (dle) {
                    dmeta.size = dle->size;
                    strncpy(dmeta.date, dle->modified, sizeof(dmeta.date) - 1);
                    dmeta.date[sizeof(dmeta.date) - 1] = '\0';
                    dmeta.md5[0] = '\0'; /* local files: no MD5 available */
                }
                panel_copy_file(PANEL_CLOUD, cfg, rchild, PANEL_LOCAL, NULL, lchild,
                                entries[i].name, &smeta, dle ? &dmeta : NULL, c);
            }
            if (c->cancelled) break;
        }
        free(dst_entries);
    }
    cmd_entries_free(entries);
}

void panel_put_dir(const AcConfig *cfg, const char *local_path,
                   const char *remote_path, CopyCtx *c)
{
    LocalDir *d;
    LocalDirEntry e;
    AcEntry *dst_entries = NULL;
    int dst_n = 0;
    int dst_existed;

    if (c->cancelled) return;
    /* Per-folder pump — see panel_get_dir()'s counterpart above for why. */
    if (!ui_progress(0, -1, basename_of(remote_path))) { c->cancelled = 1; return; }

    /* cmd_is_dir() first, silently — a new top-level folder not existing
     * yet on the remote side is the ordinary case (every first upload of
     * it), not an error. cmd_list_raw() itself is NOT silent: on a 404 it
     * reports "server error: Directory not found" via ui_notify(UI_ERROR)
     * same as any other server error, which used to fire on every single
     * new-folder upload when called here directly as the existence probe
     * — exactly the spurious message users were seeing "every so often"
     * (i.e. whenever they uploaded a folder that was actually new). Only
     * call it once we already know the folder is there, so its entries
     * (the existence/metadata snapshot for every child file below, no
     * per-file cmd_exists() round-trip) are the sole reason for the call,
     * with no failure path left to misreport. */
    dst_existed = c->ask && cmd_is_dir(cfg, remote_path)
                && cmd_list_raw(cfg, remote_path, &dst_entries, &dst_n);
    if (dst_existed) {
        UiOverwrite ow = (c->policy != -1) ? (UiOverwrite)c->policy
                        : ui_overwrite(1, basename_of(remote_path), 0, "", "", 0, "", "");
        if (ow == UI_OW_OVERWRITE_ALL || ow == UI_OW_SKIP_ALL) c->policy = (int)ow;
        if (ow == UI_OW_CANCEL) { c->cancelled = 1; cmd_entries_free(dst_entries); return; }
        if (ow == UI_OW_SKIP || ow == UI_OW_SKIP_ALL) { cmd_entries_free(dst_entries); return; }
        /* UI_OW_OVERWRITE / UI_OW_OVERWRITE_ALL: merge into it below. */
    }

    if (cmd_mkdir_quiet(cfg, remote_path) != 0)
        ui_notify(UI_WARN, "amicloudcore: warning: could not create %s (continuing)", remote_path);

    d = localfs_opendir(local_path);
    if (!d) {
        ui_notify(UI_ERROR, "amicloudcore: cannot open local directory %s", local_path);
        c->pr.err++;
        cmd_entries_free(dst_entries);
        return;
    }

    while (localfs_readdir(d, &e)) {
        char lchild[AC_MAX_PATH], rchild[AC_MAX_PATH];
        localfs_join(lchild, sizeof(lchild), local_path, e.name);
        snprintf(rchild, sizeof(rchild), "%s/%s", remote_path, e.name);
        if (e.is_dir) {
            panel_put_dir(cfg, lchild, rchild, c);
        } else {
            FileMeta smeta, dmeta;
            const AcEntry *ae = dst_entries ? find_entry(dst_entries, dst_n, e.name) : NULL;

            smeta.size = e.size;
            strncpy(smeta.date, e.modified, sizeof(smeta.date) - 1);
            smeta.date[sizeof(smeta.date) - 1] = '\0';
            smeta.md5[0] = '\0'; /* local files: no MD5 available */

            if (ae) {
                dmeta.size = ae->size;
                strncpy(dmeta.date, ae->modified, sizeof(dmeta.date) - 1);
                dmeta.date[sizeof(dmeta.date) - 1] = '\0';
                strncpy(dmeta.md5, ae->md5, sizeof(dmeta.md5) - 1);
                dmeta.md5[sizeof(dmeta.md5) - 1] = '\0';
            }
            panel_copy_file(PANEL_LOCAL, NULL, lchild, PANEL_CLOUD, cfg, rchild,
                            e.name, &smeta, ae ? &dmeta : NULL, c);
        }
        if (c->cancelled) break;
    }
    localfs_closedir(d);
    cmd_entries_free(dst_entries);
}

/* ------------------------------------------------------------------ */
/* panel_read()/panel_sort()/panel_marked_size() — new for the GUI.      */
/* ------------------------------------------------------------------ */
int panel_read(PanelSource src, const AcConfig *cfg, const char *path, PanelList *out)
{
    out->items = NULL;
    out->n     = 0;

    if (src == PANEL_LOCAL) {
        LocalDir *d = localfs_opendir(path);
        LocalDirEntry e;
        int cap = 0;

        if (!d) {
            ui_notify(UI_ERROR, "amicloudcore: cannot open local directory %s", path);
            return 0;
        }
        while (localfs_readdir(d, &e)) {
            if (out->n == cap) {
                int newcap = cap ? cap * 2 : 32;
                PanelEntry *tmp = (PanelEntry *)realloc(out->items, sizeof(PanelEntry) * (size_t)newcap);
                if (!tmp) break;
                out->items = tmp;
                cap = newcap;
            }
            {
                PanelEntry *pe = &out->items[out->n++];
                memset(pe, 0, sizeof(*pe));
                strncpy(pe->name, e.name, sizeof(pe->name) - 1);
                pe->is_dir = e.is_dir;
                pe->size   = e.size;
                strncpy(pe->modified, e.modified, sizeof(pe->modified) - 1);
                pe->mtime  = e.mtime;
            }
        }
        localfs_closedir(d);
        return 1;
    } else {
        AcEntry *entries;
        int n, i;

        if (!cmd_list_raw(cfg, path, &entries, &n)) return 0;
        if (n > 0) {
            out->items = (PanelEntry *)calloc((size_t)n, sizeof(PanelEntry));
            if (!out->items) { cmd_entries_free(entries); return 0; }
            for (i = 0; i < n; i++) {
                PanelEntry *pe = &out->items[i];
                strncpy(pe->name, entries[i].name, sizeof(pe->name) - 1);
                pe->is_dir = entries[i].is_dir;
                pe->size   = entries[i].size;
                strncpy(pe->modified, entries[i].modified, sizeof(pe->modified) - 1);
                pe->mtime  = entries[i].mtime;
                strncpy(pe->md5, entries[i].md5, sizeof(pe->md5) - 1);
            }
        }
        out->n = n;
        cmd_entries_free(entries);
        return 1;
    }
}

void panel_list_free(PanelList *list)
{
    free(list->items);
    list->items = NULL;
    list->n     = 0;
}

static int panel_cmp_name(const void *a, const void *b)
{
    const PanelEntry *ea = (const PanelEntry *)a, *eb = (const PanelEntry *)b;
    if (ea->is_dir != eb->is_dir) return eb->is_dir - ea->is_dir;
    return strcmp(ea->name, eb->name);
}

static int panel_cmp_size(const void *a, const void *b)
{
    const PanelEntry *ea = (const PanelEntry *)a, *eb = (const PanelEntry *)b;
    if (ea->is_dir != eb->is_dir) return eb->is_dir - ea->is_dir;
    if (ea->size != eb->size) return (ea->size > eb->size) - (ea->size < eb->size);
    return strcmp(ea->name, eb->name);
}

static int panel_cmp_date(const void *a, const void *b)
{
    const PanelEntry *ea = (const PanelEntry *)a, *eb = (const PanelEntry *)b;
    if (ea->is_dir != eb->is_dir) return eb->is_dir - ea->is_dir;
    if (ea->mtime != eb->mtime) return (ea->mtime > eb->mtime) - (ea->mtime < eb->mtime);
    return strcmp(ea->name, eb->name);
}

void panel_sort(PanelList *list, PanelSortKey key)
{
    int (*cmp)(const void *, const void *);
    switch (key) {
    case PANEL_SORT_SIZE: cmp = panel_cmp_size; break;
    case PANEL_SORT_DATE: cmp = panel_cmp_date; break;
    case PANEL_SORT_NAME:
    default:              cmp = panel_cmp_name; break;
    }
    qsort(list->items, (size_t)list->n, sizeof(PanelEntry), cmp);
}

long panel_marked_size(const PanelList *list, int *count_out)
{
    long total = 0;
    int  count = 0, i;
    for (i = 0; i < list->n; i++) {
        if (list->items[i].marked) {
            total += list->items[i].size;
            count++;
        }
    }
    if (count_out) *count_out = count;
    return total;
}
