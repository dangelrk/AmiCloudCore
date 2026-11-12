/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef CLOUDSYNC_TRANSPORT_H
#define CLOUDSYNC_TRANSPORT_H

/*
 * transport.h — the sync engine's own transport-neutral view of "a
 * remote store", extracted so cloudsync_run_ex() (sync.h) never calls a
 * cmd_*() function (AmiCloud's api.php REST client, libcloudhttp/cmd.h)
 * directly. Two things motivated this:
 *
 *   1. Module-encapsulation rule (see repo memory): the sync KERNEL must
 *      not know which wire protocol it's talking to.
 *   2. A second backend (OwnCloud/Nextcloud via WebDAV, see webdav.h)
 *      needs the exact same 3-way-merge/baseline logic without dragging
 *      in api.php at all — this repo's AmiCloud server and its own
 *      future sync commodity are a DIFFERENT, unrelated deliverable;
 *      this header and everything built on it must stay free of any
 *      api.php/cmd.h dependency.
 *
 * SyncTransport lives in libcloudsync, not libcloudhttp: libcloudsync
 * already depends on libcloudhttp (for the existing AmiCloud cmd_*()
 * calls sync.c wraps internally, see sync.c's amicloud_* adapter), so a
 * WebDAV implementation of this interface belongs on the SAME side of
 * that dependency, in libcloudsync/src/webdav.c — never the reverse
 * (libcloudhttp must not depend on libcloudsync's types).
 */

#include "amicloud.h"

/* One directory entry as returned by a transport's list() — a single
 * path component within the listed directory, exactly like
 * libcloudhttp's AcEntry.name (the caller, sync.c's
 * index_remote_tree_sync(), does the relative-path joining and
 * recursion itself; a transport only ever sees one directory at a time).
 *
 * Two independent, NOT-interchangeable markers -- keeping them separate
 * is what makes a second backend safe to add without perturbing the
 * first (see sync.c's remote_changed comment for the full reasoning):
 *
 *   md5   — a REAL content hash, comparable directly against a local
 *           file's own md5_file_hex() result. Populated by AmiCloud
 *           (the server computes/stores real MD5s); left "" by any
 *           backend that has no such thing (WebDAV -- an ETag is not a
 *           content hash in any portable sense, and must never be
 *           compared against one).
 *   token — an opaque per-backend change marker, only ever compared
 *           against a PREVIOUS token from the same backend (never
 *           against md5, never interpreted). AmiCloud mirrors its own
 *           md5 here too (harmless redundancy, see sync.c's
 *           amicloud_list()); WebDAV puts its ETag here and leaves md5
 *           empty.
 *
 * revision/id are AmiCloud-only content-revision bookkeeping (AmiOwnCloud docs/design/
 * 07_Sync_Metadaten.md); a backend with no equivalent
 * (WebDAV) always leaves both at 0, which is exactly the "unknown, fall
 * back to md5/token comparison" case the merge logic already handles. */
typedef struct {
    char name[256];
    int  is_dir;
    long size;
    long mtime;     /* UTC epoch seconds */
    char md5[64];   /* real content hash; "" if unavailable (WebDAV) */
    char token[64];
    long revision;  /* 0 = unknown/unsupported */
    long id;        /* 0 = unknown/unsupported */
} SyncRemoteEntry;

/* Condition for put() and del() (S8): never overwrite or delete a remote
 * change the kernel has not seen. Each backend uses what it has --
 * AmiCloud the numeric revision and file ID, WebDAV the ETag (If-Match)
 * -- and ignores the rest. */
typedef struct {
    long        if_revision; /* > 0: only if the remote file has this revision */
    long        if_id;       /* > 0: only if the remote file still has this ID */
    const char *if_token;    /* non-empty: only if the remote token (ETag) is this */
    int         create_only; /* only if the remote file does not exist yet */
} SyncPutCond;

/* What the backend learned about the stored file (S10); zero/"" where
 * it does not know -- the next listing fills that in. */
typedef struct {
    long revision;
    long id;
    char md5[64];
    char token[64];
} SyncPutResult;

/* One row of the delta feed (changes(), below): the state of a server
 * path as of change_seq. path is the full server path ('/'-prefixed),
 * pointing into the same malloc'd block. */
typedef struct {
    const char *path;
    int  is_dir;
    int  deleted;    /* 1 = gone (deleted, or the old name of a rename) */
    long size;
    long mtime;      /* UTC epoch seconds, like list() */
    char md5[64];
    long revision;
    long id;
    long change_seq;
} SyncRemoteChange;

#define SYNC_CHANGES_OK       1
#define SYNC_CHANGES_ERROR    0
#define SYNC_CHANGES_TOO_OLD (-1)

#define SYNC_PUT_OK        0
#define SYNC_PUT_ERROR     1
#define SYNC_PUT_CONFLICT  2   /* condition failed, nothing was written */

typedef struct SyncTransport {
    /* Backend-owned context (server URL/credentials/TLS flag). Both the
     * AmiCloud and the WebDAV backend reuse AcConfig as-is — server holds
     * the api.php base URL for one, the WebDAV collection URL for the
     * other; no new config fields needed for either. */
    const AcConfig *cfg;

    /* Lists remote_path's direct children. 1 on success (an empty
     * directory is a successful, valid 0-entry result), 0 on error.
     * *out is malloc'd (a single flat block, no internal pointers) —
     * the caller frees it with a plain free(). */
    int (*list)(struct SyncTransport *t, const char *remote_path,
                SyncRemoteEntry **out, int *out_n);

    /* Downloads remote_path into local_path; display_name is what the
     * progress line and messages call it (sync downloads into a
     * temporary file, see SYNC_PART_FILE). 0 on success. */
    int (*get)(struct SyncTransport *t, const char *remote_path, const char *local_path,
               const char *display_name);

    /* Uploads local_path to remote_path and sets its remote mtime to
     * mtime, as one logical operation (the AmiCloud backend still does
     * this as two internal requests — upload, then a fire-and-forget
     * setdate; WebDAV folds it into a single PUT via the X-OC-Mtime
     * header). cond may be NULL (unconditional), res may be NULL.
     * Returns SYNC_PUT_OK, SYNC_PUT_ERROR or SYNC_PUT_CONFLICT. */
    int (*put)(struct SyncTransport *t, const char *local_path,
               const char *remote_path, long mtime,
               const SyncPutCond *cond, SyncPutResult *res);

    /* Deletes a file, or a directory (WebDAV: with everything below it --
     * the kernel lists it right before and only deletes what it knows).
     * cond may be NULL. Returns SYNC_PUT_OK, SYNC_PUT_ERROR or
     * SYNC_PUT_CONFLICT (condition failed, nothing deleted). */
    int (*del)(struct SyncTransport *t, const char *remote_path, const SyncPutCond *cond);

    /* "Already exists" must count as success (see cmd_mkdir_quiet) --
     * most subdirectories already existing on a repeat sync is the
     * normal case, not an error worth reporting. */
    int (*mkdir_quiet)(struct SyncTransport *t, const char *remote_path);

    /* Aligns an already-synced path's remote mtime alone (no content
     * change) -- used only for SA_ALIGN_REMOTE, where put() doesn't
     * apply. Fire-and-forget, like the pre-refactor cmd_setdate_mtime()
     * call it replaces: its return value is deliberately not treated as
     * a sync error. */
    int (*setdate)(struct SyncTransport *t, const char *remote_path, long mtime);

    /* Cheap "did anything change remotely since last time" probe (see
     * sync.c's delta-optimisation comment). *global_out receives an
     * opaque counter the caller only ever compares for equality across
     * runs. Returns 0 if the backend has no such counter (WebDAV) --
     * sync.c then always falls back to a full recursive list() walk,
     * same as it always has for any AmiCloud server that answered
     * cmd_status() with global_revision 0. */
    int (*global_rev)(struct SyncTransport *t, long *global_out);

    /* Optional (NULL = not supported: WebDAV). One page of the delta
     * feed: what changed at or below remote_root since the counter
     * `since` (the global_rev() value of an earlier run), tombstones
     * included, ordered by change_seq; one change_seq never spans two
     * pages. *next is the since of the next page, *has_more whether there
     * is one. *out is one malloc'd block (plain free()). Returns
     * SYNC_CHANGES_OK, SYNC_CHANGES_ERROR or SYNC_CHANGES_TOO_OLD (the
     * server no longer knows all deletions since then). The kernel uses it
     * instead of a full list() walk when the counter has moved (phase 6). */
    int (*changes)(struct SyncTransport *t, const char *remote_root, long since,
                   SyncRemoteChange **out, int *out_n, long *next, int *has_more);

    /* Optional; NULL if the backend holds no per-run resources to
     * release (true for both backends today). */
    void (*close)(struct SyncTransport *t);

    /* Set by global_rev() from the server's status: the longest NEW name
     * (characters) the account may create (AmiCloud Amiga mode: 30); 0 =
     * no limit or not known (WebDAV, older servers). The kernel uses it
     * when SyncOptions.remote_name_max is 0. */
    int name_max;
} SyncTransport;

#endif /* CLOUDSYNC_TRANSPORT_H */
