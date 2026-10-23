/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef WEBDAVCMD_H
#define WEBDAVCMD_H

#include "amicloud.h"
#include "entry.h" /* AcEntry/AcInfo -- shared types are core's job, not this
                    * HTTP layer's (module-encapsulation rule, see cmd.h's
                    * own comment). Returning the exact same structs cmd.c
                    * does lets owncloud-worker's WREQ_* dispatch stay
                    * byte-for-byte identical to amicloud-worker's, just
                    * calling webdavcmd_*() instead of cmd_*(). */
#include <stddef.h>

/*
 * webdavcmd.h -- WebDAV (RFC 4918) command layer for the OwnCloud: DOS
 * handler's worker process (AmiOwnCloud's handler/owncloudhandler/src/worker.c),
 * parallel to cmd.h's AmiCloud/api.php command layer. Both talk to
 * different servers over different wire protocols but expose the same
 * buffer-based, AcEntry/AcInfo-shaped surface, so the two worker.c's stay
 * structurally identical.
 *
 * Deliberately independent of libcloudsync/src/webdav.c (the
 * SyncTransport-based WebDAV backend AmiCloudSync/webdavsync use): that
 * backend is file-based (local path in, local path out) and its
 * SyncTransport vtable has no buffer-oriented get/put, which is what a
 * DOS handler's in-RAM-per-open-file model (see owncloudhandler's
 * actions.h OpenFile) needs. Sharing the wire-level primitives
 * (http_request_hdr/http_upload_hdr, already in libcloudhttp/http.h) is
 * enough reuse; duplicating the ~150 lines of PROPFIND-multistatus/
 * RFC1123-date parsing here keeps owncloud-worker's link set to just
 * libcloudcore+libcloudhttp (no libcloudsync, matching amicloud-worker's
 * footprint) and keeps libcloudsync/AmiCloudSync completely untouched.
 *
 * Auth: HTTP Basic (cfg->username/password, an app password recommended
 * for Nextcloud). Base URL: cfg->server, either a bare host (derived to
 * "<host>/remote.php/dav/files/<username>") or an already-complete
 * "/remote.php/..." collection URL -- see webdavcmd_derive_base_url().
 * cfg->basepath is honoured as a remote path prefix within that
 * collection (unlike the sync-side webdav.c, which ignores it).
 */

/* Structured directory listing (PROPFIND Depth:1), analogous to
 * cmd_list_raw(). *out points to a malloc'd array of *out_n entries
 * (NULL/0 if empty) on success -- free with webdavcmd_entries_free().
 * WebDAV has no server-side Amiga protection bits, comment or content
 * MD5 concept, so AcEntry.protection/comment/md5 are always 0/""/"" for
 * every entry (see also SETCOMMENT/SETPROTECT below). Returns 1 on
 * success (even with zero entries), 0 on error -- same convention as
 * cmd_list_raw(), the one cmd_*() function that isn't "0 == success". */
int  webdavcmd_list_raw(const AcConfig *cfg, const char *remote_path, AcEntry **out, int *out_n);
void webdavcmd_entries_free(AcEntry *entries);

/* Downloads remote_path entirely into *out (length *out_len) via a plain
 * GET. *out is AllocVec()'d on Amiga builds (so the DOS handler process
 * can FreeVec() it across the process boundary, see proto.h's ownership
 * rules) and malloc()'d on PC/Win builds -- exactly cmd_get_buf()'s
 * contract. status_out (may be NULL) receives the raw HTTP status (0 =
 * network error). No content-hash verification (unlike cmd_get_buf()'s
 * X-AmiCloud-MD5 check) -- plain WebDAV has no equivalent response
 * header. Returns 0 on success, non-zero on error. */
int webdavcmd_get_buf(const AcConfig *cfg, const char *remote_path, char **out, long *out_len, int *status_out);

/* Uploads len bytes from buf (already in RAM) to remote_path via PUT.
 * status_out: see webdavcmd_get_buf(). Returns 0 on success. */
int webdavcmd_put_buf(const AcConfig *cfg, const char *remote_path, const char *buf, long len, int *status_out);

/* Downloads only bytes offset..offset+len-1 via a Range request (fewer
 * at the end of the file, none past it) -- cmd_get_range()'s contract.
 * A server ignoring Range (200 with the whole file) is handled by
 * slicing. Returns 0 on success. */
int webdavcmd_get_range(const AcConfig *cfg, const char *remote_path, long offset, long len,
                        char **out, long *out_len, int *status_out);

/* DELETE; 404 (already gone) counts as success, matching cmd_del(). */
int webdavcmd_del(const AcConfig *cfg, const char *remote_path);

/* MKCOL; 405 Method Not Allowed (collection already exists, RFC 4918)
 * counts as success, matching cmd_mkdir_quiet(). */
int webdavcmd_mkdir_quiet(const AcConfig *cfg, const char *remote_path);

/* MOVE with a Destination: header, matching cmd_rename()'s contract
 * (from_path/to_path both relative to cfg->basepath). */
int webdavcmd_rename(const AcConfig *cfg, const char *from_path, const char *to_path);

/* Structured single-resource info (PROPFIND Depth:0), analogous to
 * cmd_info_raw(). Same all-zero/empty caveat for protection/comment/md5
 * as webdavcmd_list_raw(). Returns 0 on success. */
int webdavcmd_info_raw(const AcConfig *cfg, const char *remote_path, AcInfo *out);

/* Best-effort PROPPATCH of {DAV:}lastmodified (a de-facto Nextcloud/
 * ownCloud extension, not RFC 4918) -- matches cmd_setdate_mtime()'s
 * contract. A plain/vanilla WebDAV server that ignores this still
 * returns 0 here as long as the request itself didn't fail outright. */
int webdavcmd_setdate_mtime(const AcConfig *cfg, const char *remote_path, long mtime_epoch);

/* Raw HTTP status of the most recently completed request (0 = no
 * response/transport failure) -- same role as cmd_last_status(), for
 * worker.c's precise DOS error mapping after a webdavcmd_*() call that
 * only returns 0/1. */
int webdavcmd_last_status(void);

#endif /* WEBDAVCMD_H */
