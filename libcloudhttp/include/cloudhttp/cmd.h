/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef CMD_H
#define CMD_H

#include "amicloud.h"
#include "entry.h" /* AcEntry/AcInfo/ListMode — libcloudcore (module-encapsulation:
                    * shared types are core's job, not this HTTP layer's). */
#include <stddef.h>

/* All commands return 0 on success, non-zero on error */
int cmd_list (const AcConfig *cfg, const char *remote_path);

/* cmd_list() ist ein Wrapper um cmd_list_mode(cfg, path, LIST_FULL). */
int cmd_list_mode(const AcConfig *cfg, const char *remote_path, ListMode mode);
int cmd_get  (const AcConfig *cfg, const char *remote_path, const char *local_path);
/* cmd_get() writing to local_path but naming display_name in progress
 * and messages -- for callers that download into a temporary file
 * (libcloudsync) and rename it into place afterwards. */
int cmd_get_named(const AcConfig *cfg, const char *remote_path, const char *local_path,
                  const char *display_name);
int cmd_put  (const AcConfig *cfg, const char *local_path,  const char *remote_path);

/* Overwrite-checked put/get for interactive frontends (amicloud CLI,
 * amicloud-shell): same transfer and same target naming as cmd_put()/
 * cmd_get(), but unless force is set they first check whether the
 * destination exists and then ask via ui_confirm() (default: no; on the
 * console EOF also counts as no, so a non-interactive call never
 * overwrites without force). put checks the server with one extra
 * request and transfers nothing if it cannot tell (network/server
 * error) or the target is a directory; get checks the local file.
 * cmd_put()/cmd_get() themselves stay unchecked -- the Commander's copy
 * engine (panel.c) asks via ui_overwrite() before calling them. */
int cmd_put_checked(const AcConfig *cfg, const char *local_path,
                    const char *remote_path, int force);
int cmd_get_checked(const AcConfig *cfg, const char *remote_path,
                    const char *local_path, int force);

int cmd_del  (const AcConfig *cfg, const char *remote_path);
int cmd_info (const AcConfig *cfg, const char *remote_path);

/* Structured variant of cmd_info() — no output, fields only (used by the
 * GUI Commander's F3 info dialog; cmd_info() itself is now a thin wrapper
 * that fetches this and prints it). Returns 0 on success. */
int cmd_info_raw(const AcConfig *cfg, const char *remote_path, AcInfo *out);
/* Silent existence check (no ui_notify on a plain "not found") — for
 * overwrite-confirmation prompts, where a missing destination is the
 * normal, expected case, not an error worth reporting. */
int cmd_exists(const AcConfig *cfg, const char *remote_path);
int cmd_mkdir(const AcConfig *cfg, const char *remote_path);
int cmd_rename(const AcConfig *cfg, const char *from_path, const char *to_path);

/* Sets a remote file's Amiga filecomment (action=setcomment&comment=,
 * URL-encoded internally; server truncates >79 chars and strips control
 * characters -- see AmiCloud docs/api/API.md). Empty comment clears it. Returns 0 on
 * success. */
int cmd_setcomment(const AcConfig *cfg, const char *remote_path, const char *comment);

/* Sets a remote file's raw Amiga protection bits (action=setprotect&
 * bits=) -- passed through unchanged, RWED active-low, never inverted
 * (AmiCloud docs/api/API.md). Returns 0 on success. */
int cmd_setprotect(const AcConfig *cfg, const char *remote_path, long bits);

/* ------------------------------------------------------------------ */
/* Additions for amicloud-shell (see AmiCloud docs/design/interactive-shell-plan.html)  */
/* ------------------------------------------------------------------ */

/* Structured (non-printing) directory listing. On success (1) *out points
 * to a malloc'd array of *out_n entries (may be NULL if *out_n==0) — free
 * with cmd_entries_free(). Returns 0 on error (already reported). */
int  cmd_list_raw(const AcConfig *cfg, const char *remote_path, AcEntry **out, int *out_n);
void cmd_entries_free(AcEntry *entries);

/* Silent remote checks/helpers (no stdout/stderr output on the "normal"
 * negative outcome — callers decide how to report). global_revision_out
 * (may be NULL) receives the server's current per-user change counter
 * (AmiOwnCloud docs/design/07_Sync_Metadaten.md) -- 0 on an older server that
 * doesn't send it. */
int cmd_is_dir(const AcConfig *cfg, const char *remote_path);
int cmd_status(const AcConfig *cfg, char *version_out, size_t version_sz, long *global_revision_out);

/* Full action=status result incl. account name and quota (AcStatus,
 * cloudcore/entry.h). Silent like cmd_status(); returns 1 if the server
 * answered "ok", 0 otherwise (out is zeroed either way first). cmd_status()
 * is a thin wrapper around this. */
int cmd_status_ex(const AcConfig *cfg, AcStatus *out);

/* Prints account and storage usage (amicloud quota, shell "quota"), like
 * cmd_info() prints file info. Returns 0 on success. */
int cmd_quota(const AcConfig *cfg);

/* Delta source for a metadata-based sync (AmiOwnCloud docs/design/
 * 07_Sync_Metadaten.md): every row for this user with change_seq >
 * since, tombstones (AcChange.deleted) included -- unlike cmd_list_raw(),
 * not scoped to one directory. global_revision_out (may be NULL)
 * receives the server's current counter, so a caller can persist "synced
 * up to here" without a second cmd_status() round-trip. Returns 1 on
 * success (even with zero changes, *out_n==0), 0 on error. */
int  cmd_changes(const AcConfig *cfg, long since, AcChange **out, int *out_n, long *global_revision_out);
void cmd_changes_free(AcChange *changes);

/* Server-side name search (action=search, AmiCloud docs/design/search-plan.md):
 * matches the "*"/"?"/"#?" glob pattern against the basename of every live
 * row under root (recursive; root=NULL or "/" = whole cloud), scoped to
 * this user. dirs_too=0 restricts hits to files. truncated_out (may be
 * NULL) is set to 1 if the server capped the result at its limit. On
 * success (1) *out points to a malloc'd array of *out_n hits (may be NULL
 * if *out_n==0) -- free with cmd_search_free(). Returns 0 on error
 * (already reported). */
int  cmd_search(const AcConfig *cfg, const char *root, const char *pattern,
                 int dirs_too, AcSearchHit **out, int *out_n, int *truncated_out);
void cmd_search_free(AcSearchHit *hits);

/* Fetches the MD5 of a single remote file via action=info (server computes
 * or serves it from the DB — plan section 18.5). hex_out must be at least
 * 33 bytes. Errors (network, directory target, no checksum) are reported
 * to stderr; returns 0 on success. Used by the shell's `md5` command. */
int cmd_get_md5(const AcConfig *cfg, const char *remote_path, char *hex_out, size_t hex_sz);

/* Same as cmd_mkdir(), but treats "already exists" (409) as success and
 * never prints anything. */
int cmd_mkdir_quiet(const AcConfig *cfg, const char *remote_path);

/* Sets a remote file's mtime to a UTC epoch (action=setdate&mtime=) —
 * used by `sync` (plan section 20.2) to align the destination's mtime to
 * the source's after a copy, without any timezone round-trip. */
int cmd_setdate_mtime(const AcConfig *cfg, const char *remote_path, long mtime_epoch);

/* ------------------------------------------------------------------ */
/* In-memory transfer helpers for the AMICLOUD: handler's worker process   */
/* (AmiCloud docs/design/handler-plan.md Stufe 2) -- the worker already needs the whole */
/* file in RAM either way (Stage 2's design has no streaming/partial    */
/* reads), so these skip cmd_get()/cmd_put()'s local-file round trip.   */
/* ------------------------------------------------------------------ */

/* Downloads remote_path entirely into *out (length *out_len), verifying
 * X-AmiCloud-MD5 if present. *out is allocated with AllocVec() on Amiga
 * builds (so a different process -- the DOS handler -- can FreeVec() it)
 * and malloc() on PC/Win builds; the caller frees it with the matching
 * platform function, never cmd.c's own free(). status_out, if non-NULL,
 * receives the raw HTTP status code (0 = no response/network error) --
 * used by the worker for precise DOS error mapping; may be NULL for
 * callers that only care about success/failure. Returns 0 on success. */
int cmd_get_buf(const AcConfig *cfg, const char *remote_path, char **out, long *out_len, int *status_out);

/* Uploads len bytes from buf (already in RAM) to remote_path, computing
 * and sending the MD5 up front like cmd_put(). status_out: see
 * cmd_get_buf(). Returns 0 on success. */
int cmd_put_buf(const AcConfig *cfg, const char *remote_path, const char *buf, long len, int *status_out);

/* Downloads only bytes offset..offset+len-1 of remote_path (fewer at the
 * end of the file, none past it) into *out -- same allocation and
 * status_out contract as cmd_get_buf(), no MD5 check (the server's
 * checksum covers whole files only). For the AMICLOUD: handler's reads:
 * opening a file must not pull all of it over the line just because
 * Workbench wants a few bytes to pick an icon. Returns 0 on success. */
int cmd_get_range(const AcConfig *cfg, const char *remote_path, long offset, long len,
                  char **out, long *out_len, int *status_out);

/* Recursive delete: tries the server-side action=delete&recursive=1
 * first, falls back to a client-side walk if the server doesn't support
 * it (older deployment, signalled by a 409 there). */
int cmd_del_recursive(const AcConfig *cfg, const char *remote_path);

/* Expose the basepath-relative path resolution used internally by all
 * cmd_*() functions, so callers can pre-compute an absolute remote path
 * (needed for recursive get/put/del bookkeeping in the shell). */
void cmd_resolve_path(const AcConfig *cfg, const char *path, char *out, size_t outsz);

/* Raw HTTP status of the most recently completed request (0 = no
 * response/transport failure). Every cmd_*() function in this module
 * updates it before returning, on success and on failure alike -- for
 * callers whose wrapper only returns 0/1 (e.g. the AMICLOUD: handler's
 * worker, worker.c) but still needs the real status for precise error
 * mapping, call this right after such a cmd_*() call returns. */
int cmd_last_status(void);

#endif /* CMD_H */
