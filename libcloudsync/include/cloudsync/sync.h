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
} SyncOptions;

void sync_options_init(SyncOptions *o);

/* 1 if a file or directory of this name is never synced (S13): the
 * kernel's own .amicloudsync*, .DS_Store, ._*, Thumbs.db, desktop.ini,
 * ~$* (Office locks), *.part, names with a backslash or control
 * characters. Same list on every client. */
int sync_name_excluded(const char *name);

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
