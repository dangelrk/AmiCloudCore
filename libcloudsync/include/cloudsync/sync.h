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
 * transport.h split the actual reconcile kernel (cloudsync_run_t) away
 * from any one wire protocol, so a second backend (WebDAV, see
 * webdav.h) can reuse the identical 3-way-merge/baseline logic without
 * this repo's own AmiCloud server (api.php) entering the picture at
 * all -- see transport.h's own comment for why. cloudsync_run() is now
 * just a one-line wrapper kept for source compatibility with every
 * existing caller (AmiCloud's apps/amicloud-sync CLI): same signature, same
 * behaviour, unchanged.
 */

#include "amicloud.h"
#include "progress.h"
#include "transport.h"
#include <stddef.h>

typedef struct { int uploaded, downloaded, unchanged, deleted, conflicts, errors; } SyncSummary;

/* Reconciles local_root and remote_root via a 3-way diff against
 * syncstate.h's persistent baseline (see sync.c for the full decision
 * matrix): a path changed on only one side transfers unambiguously in
 * that direction; unchanged on both sides just gets 2s-tolerant mtime
 * alignment; changed identically on both sides converges silently;
 * genuinely different content on both sides is an unresolvable conflict
 * that's skipped and reported, never resolved by mtime. A path that
 * disappeared from one live side but matches its last-synced state on
 * the other is recognised as a real deletion and propagated, rather than
 * recreated as if new -- the baseline is what makes that distinction
 * possible at all.
 *
 * verbose=1 emits the original shell do_sync()'s per-action ui_notify()
 * lines (banner / dry-run preview / conflict warnings); verbose=0 (a
 * future commodity/GUI caller) stays silent except the one always-
 * emitted final tally line. pr is caller-owned (reset internally at the
 * start of the execute pass) so a caller can point a live progress
 * display at it during a real (non-dry) run. dry_run performs the full
 * scan/decision pass and fills *out but changes nothing on either side,
 * including the baseline file itself.
 *
 * t is the transport to reconcile remote_root through (see transport.h)
 * -- everything else is identical to cloudsync_run() below, which is
 * simply this function called with an AmiCloud/api.php transport built
 * internally from cfg.
 */
void cloudsync_run_t(SyncTransport *t, const char *local_root, const char *remote_root,
                     int dry_run, int verbose, Progress *pr, SyncSummary *out);

/* Original entry point, unchanged in signature and behaviour -- builds
 * the AmiCloud (api.php, libcloudhttp/cmd.h) transport internally and
 * calls cloudsync_run_t(). Every existing caller (AmiCloud's apps/amicloud-sync)
 * keeps working exactly as before. */
void cloudsync_run(const AcConfig *cfg, const char *local_root, const char *remote_root,
                   int dry_run, int verbose, Progress *pr, SyncSummary *out);

#endif /* CLOUDSYNC_SYNC_H */
