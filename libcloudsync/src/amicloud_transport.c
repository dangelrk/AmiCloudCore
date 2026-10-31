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
    return cmd_status(t->cfg, NULL, 0, global_out);
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
    t->close       = NULL;
    return t;
}

void amicloud_transport_close(SyncTransport *t)
{
    if (!t) return;
    if (t->close) t->close(t);
    free(t);
}
