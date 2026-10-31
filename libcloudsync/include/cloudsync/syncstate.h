/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef CLOUDSYNC_SYNCSTATE_H
#define CLOUDSYNC_SYNCSTATE_H

/*
 * syncstate.h -- persistent last-sync baseline (AmiOwnCloud docs/design/
 * 07_Sync_Metadaten.md's "Lokale Sync-Datenbank"), stored as JSON in
 * <local_root>/.amicloudsync. This is the missing piece that lets
 * cloudsync_run_ex() tell "deleted since the last sync" apart from "never
 * existed" -- see sync.c's 3-way decision matrix.
 *
 * Own struct, not libcloudcore's AcEntry/AcChange (module-encapsulation
 * rule): this is sync's own persisted view of a path, not a wire-format
 * response type.
 */

#include <stddef.h>

#define SYNCSTATE_FILE ".amicloudsync"
/* Format of the baseline file. A file with another (or no) format number
 * is discarded on load -- the run then counts as a first sync, which only
 * merges and never deletes. No migration: bump this whenever the meaning
 * of a field changes. */
#define SYNCSTATE_FORMAT 2
/* A download in progress lands here (same directory as its target) and
 * only replaces the target once complete -- a broken-off transfer must
 * never leave a half file where the next sync would see a local edit
 * and upload it. Skipped by the local scan at every level. */
#define SYNC_PART_FILE ".amicloudsync.part"

typedef struct {
    char *path;        /* relative to the sync root, '/'-joined, malloc'd */
    int   is_dir;
    long  size;
    long  mtime;       /* UTC epoch, as of the last successful sync */
    /* Real content hash of this path's LOCAL copy as of the last sync --
     * always populated (local hashing is backend-independent), always
     * compared only against a fresh local md5_file_hex() (sync.c's
     * local_changed). Widened from 32 hex chars to 64 purely for
     * headroom; existing shorter values still load fine. NEVER compare
     * this against a remote value directly -- see token below. */
    char  md5[64];
    /* Opaque marker for this path's REMOTE state as of the last sync
     * (see transport.h's SyncRemoteEntry.token) -- AmiCloud's own MD5
     * (mirrored from md5 above) or WebDAV's ETag. Compared only against
     * a fresh transport listing's token (sync.c's remote_changed), never
     * against md5. "" means "no remote token recorded yet" (e.g. right
     * after this path was uploaded but not yet re-listed) -- sync.c
     * treats that as "assume unchanged" rather than guessing, and the
     * very next sync's live listing fills it in for free. */
    char  token[64];
    long  revision;    /* server-side content revision at the last sync; 0 = unsupported (WebDAV) */
    long  server_id;   /* server-side file ID; 0 = unknown/never seen */
    long  last_sync;   /* UTC epoch of the last successful sync of this path */
} SyncStateEntry;

typedef struct {
    SyncStateEntry *items;
    int n, cap;
    long global_revision; /* server's global_revision as of the last full sync */
    /* Set whenever the last sync left an unresolved conflict anywhere in
     * the tree. A conflicted path's baseline entry deliberately still
     * holds its pre-divergence state (so the same conflict keeps getting
     * detected instead of the reference point silently vanishing) --
     * which means it is NOT an accurate mirror of that path's actual
     * current remote content. sync.c's delta-optimisation shortcut
     * (reuse the baseline as the remote index when global_revision
     * hasn't moved) is only sound if every entry IS such a mirror, so it
     * must be disabled -- forcing one full rescan -- for as long as this
     * flag is set, i.e. until every conflict is resolved. */
    int had_conflicts;
    /* Which pair this baseline belongs to (server, user, remote root, ...;
     * built by sync.c), malloc'd, NULL if unknown. A baseline with another
     * identity must never be used: its entries would make every file of
     * the new pair look deleted (S3). */
    char *identity;
} SyncState;

/* Loads <local_root>/.amicloudsync. A missing file is not an error -- it
 * means "no baseline yet" (first sync of this pair) and yields an empty,
 * zeroed *st; so does a file with another SYNCSTATE_FORMAT or that cannot
 * be parsed. Returns 1 on success (including the missing-file case), 0
 * only on an unexpected allocation failure. */
int syncstate_load(const char *local_root, SyncState *st);

/* Serializes *st to <local_root>/.amicloudsync, writing to a .tmp file
 * first and renaming it over the target -- an interrupted sync must
 * never leave a half-written, corrupt baseline behind. Returns 1 on
 * success. */
int syncstate_save(const char *local_root, const SyncState *st);

/* Linear lookup by path (small/medium trees -- see syncstate.c). NULL if
 * not present. */
SyncStateEntry *syncstate_find(SyncState *st, const char *path);

/* Inserts a new entry for path, or overwrites the existing one -- path is
 * copied internally, the caller keeps ownership of the strings passed
 * in. token may be NULL/"" (treated the same as md5's NULL handling). */
void syncstate_upsert(SyncState *st, const char *path, int is_dir, long size, long mtime,
                      const char *md5, const char *token,
                      long revision, long server_id, long last_sync);

/* Replaces st->identity with a copy of id (NULL clears it). 0 on out of
 * memory. */
int syncstate_set_identity(SyncState *st, const char *id);

/* Removes the entry for path, if present. Order of the remaining entries
 * is not preserved. */
void syncstate_remove(SyncState *st, const char *path);

void syncstate_free(SyncState *st);

#endif /* CLOUDSYNC_SYNCSTATE_H */
