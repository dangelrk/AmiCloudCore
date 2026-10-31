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
     * setdate exactly like the pre-refactor code did; WebDAV folds it
     * into a single PUT via the X-OC-Mtime header). */
    int (*put)(struct SyncTransport *t, const char *local_path,
               const char *remote_path, long mtime);

    int (*del)(struct SyncTransport *t, const char *remote_path);

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

    /* Optional; NULL if the backend holds no per-run resources to
     * release (true for both backends today). */
    void (*close)(struct SyncTransport *t);
} SyncTransport;

#endif /* CLOUDSYNC_TRANSPORT_H */
