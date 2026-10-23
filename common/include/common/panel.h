/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef PANEL_H
#define PANEL_H

/*
 * panel.h — frontend-agnostic directory-panel core (GUI-Commander-plan
 * section 5.4). Unifies a local and a remote directory listing behind
 * one entry type so any two-pane frontend (the shell's own ls/lls today,
 * a future Qt/MUI Commander tomorrow) can treat both sides identically.
 *
 * Also home to two pieces of logic that used to live in shell.c but have
 * no real dependency on the REPL (ShellSession, printf prompts): remote
 * path resolution and the recursive get/put orchestration shared by
 * `get -r`/`put -r` and any GUI copy operation. Moving them here lets a
 * GUI frontend reuse them without dragging shell.c's REPL loop along.
 *
 * This is a copy of the old client/panel.c/.h (AmiCloud docs/design/05_Refactoring_Client_
 * Handler.md, M5) with the two-way sync engine (panel_sync()/SyncSummary
 * and its helpers) removed — that logic now lives in libcloudsync
 * (libcloudsync/include/cloudsync/sync.h), dormant until a future
 * sync commodity links it. Progress/progress_reset()/progress_prefix()
 * also moved out, to libcloudcore (both this file and libcloudsync need
 * them) — this header just includes cloudcore's progress.h instead of
 * declaring its own copy.
 */

#include "amicloud.h"
#include "cmd.h"
#include "progress.h"
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Remote path resolution: turns a (possibly shortcut) `cd`/get/put/…   */
/* argument into a clean, absolute '/'-rooted path.                     */
/* ------------------------------------------------------------------ */
void remote_resolve(char *out, size_t outsz, const char *cwd, const char *input);

/* ------------------------------------------------------------------ */
/* Tree counting for the two-pass progress model (wildcard get/put,       */
/* get -r/put -r): a counting pass fills total/bytes_total before the    */
/* first transfer.                                                        */
/* ------------------------------------------------------------------ */
void count_remote_tree(const AcConfig *cfg, const char *remote_path, Progress *p);
void count_local_tree(const char *local_path, Progress *p);

/* PanelSource is needed by panel_copy_file() below, ahead of the rest of
 * the panel abstraction further down (which also defines it). */
typedef enum { PANEL_LOCAL, PANEL_CLOUD } PanelSource;

/* ------------------------------------------------------------------ */
/* Copy orchestration with per-file/per-folder overwrite policy (GUI-    */
/* Commander copy-flow rework). One CopyCtx spans an entire operation    */
/* (the whole selection, recursing into every subfolder), so an "…All"   */
/* decision made on the first conflict sticks for every later one.       */
/* ------------------------------------------------------------------ */

/* The bit of metadata worth showing a user deciding whether to
 * overwrite: size, a pre-formatted display date, and MD5 (32 hex chars,
 * "" if unknown — always "" for local files, none is computed here). */
typedef struct {
    long size;
    char date[24];
    char md5[33];
} FileMeta;

typedef struct {
    Progress pr;
    int ask;       /* 1 = check destination existence and ask via ui_overwrite();
                    * 0 = silently overwrite, no existence check at all (the
                    * shell's get -r/put -r with -f/FORCE). */
    int policy;    /* -1 = ask again on the next conflict; otherwise a sticky
                    * UiOverwrite (UI_OW_OVERWRITE_ALL or UI_OW_SKIP_ALL) from
                    * ui_hooks.h, applied without asking. Plain int here so
                    * this header doesn't need to pull in ui_hooks.h. */
    int cancelled; /* set once ui_overwrite() returns UI_OW_CANCEL */
} CopyCtx;

void copyctx_init(CopyCtx *c, int ask);

/* Copies one file, handling the overwrite decision (if c->ask) and
 * bumping c->pr. dmeta==NULL means the destination doesn't exist yet
 * (skips the prompt outright). Local<->Local and Cloud<->Cloud are not
 * supported (no case for that combination exists yet); ssrc/dsrc pick
 * cmd_put() (Local->Cloud) or cmd_get() (Cloud->Local). scfg/dcfg are
 * only read for the Cloud-side config, ignored for the Local side. */
void panel_copy_file(PanelSource ssrc, const AcConfig *scfg, const char *spath,
                     PanelSource dsrc, const AcConfig *dcfg, const char *dpath,
                     const char *name, const FileMeta *smeta,
                     const FileMeta *dmeta, CopyCtx *c);

/* Recursive get/put orchestration — shared by the shell's `get -r`/
 * `put -r` and the GUI Commander's F5 copy. When c->ask, also prompts
 * (via ui_overwrite(), is_dir=1) if remote_path/local_path already
 * exists before merging into it, and prompts per conflicting file
 * inside via panel_copy_file(). */
void panel_get_dir(const AcConfig *cfg, const char *remote_path,
                   const char *local_path, CopyCtx *c);
void panel_put_dir(const AcConfig *cfg, const char *local_path,
                   const char *remote_path, CopyCtx *c);

/* ------------------------------------------------------------------ */
/* Panel abstraction proper: one entry type for both a local and a       */
/* remote directory listing. PanelSource itself is declared above,       */
/* ahead of panel_copy_file().                                            */
/* ------------------------------------------------------------------ */
typedef struct {
    char name[256];
    int  is_dir;
    long size;
    char modified[24]; /* display string, local time (both sources) */
    long mtime;         /* UTC epoch */
    char md5[33];        /* PANEL_CLOUD only; "" for PANEL_LOCAL entries */
    int  marked;
} PanelEntry;

typedef struct {
    PanelEntry *items; /* malloc'd, NULL if n==0 */
    int         n;
} PanelList;

typedef enum { PANEL_SORT_NAME, PANEL_SORT_SIZE, PANEL_SORT_DATE } PanelSortKey;

/* Reads one directory listing. For PANEL_CLOUD, path is the full
 * canonical remote path to list — cfg->basepath plays no role here, the
 * caller resolves that the same way the shell's eff_config() trick does.
 * Returns 1 on success (out->items/out->n populated, free with
 * panel_list_free() even if n==0), 0 on error (already reported via
 * ui_notify). */
int  panel_read(PanelSource src, const AcConfig *cfg, const char *path, PanelList *out);
void panel_list_free(PanelList *list);

/* Sorts in place: directories always sort before files; within each
 * group, ascending by the given key, falling back to name on ties. */
void panel_sort(PanelList *list, PanelSortKey key);

/* Sum of size (and count) of every entry with marked!=0. */
long panel_marked_size(const PanelList *list, int *count_out);

#endif /* PANEL_H */
