/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 *
 * amicloud_transport.c -- SyncTransport for the AmiCloud server (api.php),
 * built on libcloudhttp's cmd_*() calls. Moved out of sync.c so the sync
 * kernel itself never includes cmd.h (module encapsulation, see
 * transport.h); the WebDAV counterpart is webdav.c.
 */
#include "amicloud_transport.h"
#include "cmd.h"

#include <stdlib.h>
#include <string.h>

static int amicloud_list(SyncTransport *t, const char *remote_path,
                         SyncRemoteEntry **out, int *out_n)
{
    AcEntry *entries;
    int n, i;

    *out = NULL; *out_n = 0;
    if (!cmd_list_raw(t->cfg, remote_path, &entries, &n)) return 0;

    if (n > 0) {
        SyncRemoteEntry *arr = (SyncRemoteEntry *)calloc((size_t)n, sizeof(SyncRemoteEntry));
        if (!arr) { cmd_entries_free(entries); return 0; }
        for (i = 0; i < n; i++) {
            strncpy(arr[i].name, entries[i].name, sizeof(arr[i].name) - 1);
            arr[i].is_dir   = entries[i].is_dir;
            arr[i].size     = entries[i].size;
            arr[i].mtime    = entries[i].mtime;
            /* AmiCloud provides a real content hash -- populate both
             * md5 (compared against local hashes/no-baseline checks) and
             * token (compared against the baseline's remembered remote
             * marker) with it; see transport.h/syncstate.h for why these
             * two roles must stay independently populated in general. */
            strncpy(arr[i].md5, entries[i].md5, sizeof(arr[i].md5) - 1);
            strncpy(arr[i].token, entries[i].md5, sizeof(arr[i].token) - 1);
            arr[i].revision = entries[i].revision;
            arr[i].id       = entries[i].id;
        }
        *out = arr;
    }
    *out_n = n;
    cmd_entries_free(entries);
    return 1;
}

static int amicloud_get(SyncTransport *t, const char *remote_path, const char *local_path,
                        const char *display_name)
{
    return cmd_get_named(t->cfg, remote_path, local_path, display_name);
}

/* Two internal requests, exactly like the pre-refactor code: setdate's
 * return value is deliberately ignored (fire-and-forget), unchanged
 * behaviour. */
static int amicloud_put(SyncTransport *t, const char *local_path,
                        const char *remote_path, long mtime,
                        const SyncPutCond *cond, SyncPutResult *res)
{
    AcPutInfo info;
    int rc = cmd_put_cond(t->cfg, local_path, remote_path,
                          cond ? cond->if_revision : 0, cond ? cond->if_id : 0,
                          cond ? cond->create_only : 0, &info);
    if (rc == AC_PUT_PRECONDITION) return SYNC_PUT_CONFLICT;
    if (rc != 0) return SYNC_PUT_ERROR;
    cmd_setdate_mtime(t->cfg, remote_path, mtime);   /* does not change the revision */
    if (res) {
        memset(res, 0, sizeof(*res));
        res->revision = info.revision;
        res->id       = info.id;
        strncpy(res->md5, info.md5, sizeof(res->md5) - 1);
        strncpy(res->token, info.md5, sizeof(res->token) - 1);   /* as amicloud_list() */
    }
    return SYNC_PUT_OK;
}

static int amicloud_del(SyncTransport *t, const char *remote_path, const SyncPutCond *cond)
{
    int rc = cmd_del_cond(t->cfg, remote_path, cond ? cond->if_revision : 0, cond ? cond->if_id : 0);
    if (rc == AC_PUT_PRECONDITION) return SYNC_PUT_CONFLICT;
    return rc == 0 ? SYNC_PUT_OK : SYNC_PUT_ERROR;
}

static int amicloud_mkdir_quiet(SyncTransport *t, const char *remote_path)
{
    return cmd_mkdir_quiet(t->cfg, remote_path);
}

static int amicloud_setdate(SyncTransport *t, const char *remote_path, long mtime)
{
    return cmd_setdate_mtime(t->cfg, remote_path, mtime);
}

static int amicloud_global_rev(SyncTransport *t, long *global_out)
{
    AcStatus st;
    int ok = cmd_status_ex(t->cfg, &st);
    *global_out = ok ? st.global_revision : 0;
    t->name_max = ok ? st.name_max : 0;   /* one status call serves both */
    return ok;
}

/* Rows per page: a 68k Amiga holds a page in memory twice (JSON and
 * array), so smaller pages there. */
#ifdef PLATFORM_AMIGA
#define CHANGES_PAGE 200
#else
#define CHANGES_PAGE 1000
#endif

/* The server answers with absolute paths; the kernel compares them with
 * remote_root as the caller wrote it ("texte", "" or "/texte", relative
 * to basepath). Rewrites path into that form: root + rest. 0 if path is
 * not below the root (left as it is). */
static size_t strip_slashes(const char *s)
{
    size_t l = strlen(s);
    while (l > 0 && s[l - 1] == '/') l--;
    return l;
}

static int to_caller_path(const AcConfig *cfg, const char *remote_root, const char *path,
                          char *out, size_t sz)
{
    char abs[AC_MAX_PATH];
    size_t al, rl;
    cmd_resolve_path(cfg, remote_root, abs, sizeof(abs));
    al = strip_slashes(abs);
    rl = strip_slashes(remote_root);
    if (strncmp(path, abs, al) != 0 || (path[al] != '/' && path[al] != '\0')) return 0;
    if (rl + strlen(path + al) + 1 > sz) return 0;
    memcpy(out, remote_root, rl);
    strcpy(out + rl, path + al);
    return 1;
}

static int amicloud_changes(SyncTransport *t, const char *remote_root, long since,
                            SyncRemoteChange **out, int *out_n, long *next, int *has_more)
{
    AcChange *ch;
    int n, i, rc;
    size_t strsz = 0;
    SyncRemoteChange *arr;
    char *strs;

    *out = NULL; *out_n = 0;
    rc = cmd_changes(t->cfg, remote_root, since, CHANGES_PAGE, &ch, &n, next, has_more);
    if (rc == AC_CHANGES_TOO_OLD) return SYNC_CHANGES_TOO_OLD;
    if (rc != AC_CHANGES_OK) return SYNC_CHANGES_ERROR;
    if (n == 0) { cmd_changes_free(ch); return SYNC_CHANGES_OK; }

    /* worst case per path: the caller's root form replaces the absolute one */
    for (i = 0; i < n; i++) strsz += strlen(ch[i].path) + strlen(remote_root) + 2;
    arr = (SyncRemoteChange *)calloc(1, sizeof(SyncRemoteChange) * (size_t)n + strsz);
    if (!arr) { cmd_changes_free(ch); return SYNC_CHANGES_ERROR; }
    strs = (char *)(arr + n);
    for (i = 0; i < n; i++) {
        char cp[AC_MAX_PATH];
        const char *src = to_caller_path(t->cfg, remote_root, ch[i].path, cp, sizeof(cp))
                          ? cp : ch[i].path;
        size_t pl = strlen(src) + 1;
        memcpy(strs, src, pl);
        arr[i].path       = strs;
        strs += pl;
        arr[i].is_dir     = ch[i].is_dir;
        arr[i].deleted    = ch[i].deleted;
        arr[i].size       = ch[i].size;
        arr[i].mtime      = ch[i].mtime;
        strncpy(arr[i].md5, ch[i].md5, sizeof(arr[i].md5) - 1);
        arr[i].revision   = ch[i].revision;
        arr[i].id         = ch[i].id;
        arr[i].change_seq = ch[i].change_seq;
    }
    cmd_changes_free(ch);
    *out = arr; *out_n = n;
    return SYNC_CHANGES_OK;
}

SyncTransport *amicloud_transport_open(const AcConfig *cfg)
{
    SyncTransport *t = (SyncTransport *)calloc(1, sizeof(SyncTransport));
    if (!t) return NULL;
    t->cfg         = cfg;
    t->list        = amicloud_list;
    t->get         = amicloud_get;
    t->put         = amicloud_put;
    t->del         = amicloud_del;
    t->mkdir_quiet = amicloud_mkdir_quiet;
    t->setdate     = amicloud_setdate;
    t->global_rev  = amicloud_global_rev;
    t->changes     = amicloud_changes;
    t->close       = NULL;
    return t;
}

void amicloud_transport_close(SyncTransport *t)
{
    if (!t) return;
    if (t->close) t->close(t);
    free(t);
}
