/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef CLOUDSYNC_SYNC_H
#define CLOUDSYNC_SYNC_H

/*
 * sync.h — two-way local<->cloud folder synchronization with a
 * persistent last-sync baseline (AmiOwnCloud docs/design/07_Sync_Metadaten.md).
 * Originally extracted from the old client/panel.c's panel_sync()
 * (AmiCloud docs/design/handler-plan.md's sibling, AmiCloud docs/design/interactive-shell-plan.html
 * section 20) into its own library per AmiCloud docs/design/05_Refactoring_Client_
 * Handler.md, M1: sync no longer ships wired into any interactive
 * frontend (shell command / Commander menu) — it lives here, dormant,
 * until a future cloudsync commodity (roadmap phase 5) links it.
 *
 * SyncSummary/SyncAction* are sync's own domain and stay here, not in
 * libcloudcore (module-encapsulation rule) — nothing outside this lib
 * needs to know sync's decision-matrix internals.
 *
 * transport.h keeps the reconcile kernel (cloudsync_run_ex) independent
 * of any wire protocol: amicloud_transport.h (api.php) and webdav.h
 * (ownCloud/Nextcloud) are the two backends.
 */

#include "amicloud.h"
#include "progress.h"
#include "transport.h"
#include <stddef.h>

typedef struct {
    int uploaded, downloaded, unchanged, deleted, conflicts, errors;
    /* 1 if the run stopped early: scan error, out of memory (nothing was
     * changed) or a break (the rest follows with the next run). */
    int aborted;
    /* remote names skipped because they are too long for the local file
     * system (S15) */
    int skipped;
    /* files not transferred because the virus check found something */
    int viruses;
    /* conflicts resolved by keeping both versions (conflict copies) */
    int copies;
    /* deletions not done because the mass deletion guard said no */
    int kept;
} SyncSummary;

/* Options of one run. Initialise with sync_options_init() and change
 * only what you need -- new fields always get a safe default there. */
typedef struct {
    /* Scan and decide only: nothing changes on either side, the baseline
     * file included. */
    int dry_run;
    /* 1: a ui_notify() line per action (CLI). 0: only warnings, errors
     * and the final tally (commodity, GUI). */
    int verbose;
    /* Extra identity of this pair, stored in the baseline next to the
     * server, user and remote root the kernel takes from the transport
     * (S3); e.g. the local codeset. NULL = none. */
    const char *identity_extra;
    /* Longest local file name in bytes; 0 = ask libcloudfs
     * (localfs_name_max(), S15). Longer remote names are skipped. */
    int name_max;
    /* Virus check of every file before it is uploaded or put in place
     * after a download (libcloudfs: xvs.library on AmigaOS, nothing
     * elsewhere). virus_max: largest file checked, 0 = LOCALFS_VIRUS_MAX,
     * < 0 = no check. virus_check: replaces localfs_virus_check (tests). */
    long virus_max;
    int (*virus_check)(const char *path, long max_bytes, char *name, size_t namesz);
    /* Conflicts (phase 3): 0 = keep both versions -- the local one is
     * renamed to a conflict copy "Name_K261031-1402.ext" and uploaded under
     * that name, the server version takes the original name; a file
     * deleted on one side but changed on the other is transferred again.
     * 1 = skip and report, change nothing (the behaviour before 0.3). */
    int conflict_skip;
    /* Mass deletion guard: more deletions on one side than
     * max(del_min, del_pct % of the baseline) are asked for (ui_confirm);
     * "no" leaves the files alone and the next run copies them back.
     * 0 = defaults (20 files, 50 %); force_deletes = 1 never asks. */
    int del_min, del_pct;
    int force_deletes;
    /* Selective sync (AmiCloud Desktop): paths relative to the remote
     * root (UTF-8, '/'-joined, no leading or trailing '/') that are left
     * out, each with everything below it. Neither side is touched there --
     * nothing is transferred or deleted, a local folder of that name stays
     * as it is -- and their baseline entries are dropped, so taking a path
     * back in later merges like a first sync (never deletes). A changed
     * list turns the global_revision fast path off for one run. NULL/0 =
     * everything. The kernel keeps no pointer after the run. */
    const char *const *excludes;
    int n_excludes;
} SyncOptions;

void sync_options_init(SyncOptions *o);

/* 1 if a file or directory of this name is never synced (S13): the
 * kernel's own .amicloudsync*, .DS_Store, ._*, Thumbs.db, desktop.ini,
 * ~$* (Office locks), *.part, names with a backslash or control
 * characters. Same list on every client. */
int sync_name_excluded(const char *name);

/* 1 if relpath (relative to the remote root, UTF-8) is one of the n
 * paths in ex or lies below one (see SyncOptions.excludes). */
int sync_path_excluded(const char *const *ex, int n, const char *relpath);

/* Reconciles local_root and remote_root through transport t via a 3-way
 * diff against syncstate.h's persistent baseline (see sync.c for the
 * full decision matrix): a path changed on only one side transfers in
 * that direction; unchanged on both sides just gets 2s-tolerant mtime
 * alignment; changed identically on both sides converges silently;
 * different content on both sides is a conflict that is skipped and
 * reported, never resolved by mtime. A path that disappeared from one
 * side but matches its last-synced state on the other is a deletion and
 * is propagated.
 *
 * Safety (AmiCloud docs/design/desktop-sync-plan.md, section 3): a scan
 * error or out of memory aborts before anything changes (S1); a break
 * (Ctrl-C) stops between actions and never deletes (S2); a baseline that
 * belongs to another server, user or remote root is discarded and the
 * run merges only (S3); with an existing baseline a missing remote root
 * is never recreated (S4).
 *
 * opts may be NULL (defaults). pr is caller-owned (reset internally at
 * the start of the execute pass) so a caller can show live progress. */
void cloudsync_run_ex(SyncTransport *t, const char *local_root, const char *remote_root,
                      const SyncOptions *opts, Progress *pr, SyncSummary *out);

#endif /* CLOUDSYNC_SYNC_H */
