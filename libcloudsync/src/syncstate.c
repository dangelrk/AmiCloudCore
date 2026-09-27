/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * syncstate.c -- see syncstate.h. JSON load/save via parson (libcloudcore);
 * the actual last-sync-baseline array is a flat, unsorted list, exactly
 * like sync.c's own SyncIndex -- sync roots are directories a user works
 * in by hand, not millions of files, so a linear syncstate_find() is
 * plenty fast and keeps this file simple.
 */
#include "syncstate.h"
#include "localfs.h"
#include "parson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *ss_dupstr(const char *s)
{
    size_t len = strlen(s) + 1;
    char *p = (char *)malloc(len);
    if (p) memcpy(p, s, len);
    return p;
}

static void state_init(SyncState *st)
{
    st->items = NULL;
    st->n = 0;
    st->cap = 0;
    st->global_revision = 0;
    st->had_conflicts = 0;
}

int syncstate_load(const char *local_root, SyncState *st)
{
    char path[1024];
    JSON_Value *root_val;
    JSON_Object *root;
    JSON_Array *entries;
    int n, i;

    state_init(st);

    localfs_join(path, sizeof(path), local_root, SYNCSTATE_FILE);
    root_val = json_parse_file(path);
    if (!root_val) return 1; /* no baseline yet -- first sync, not an error */

    root = json_value_get_object(root_val);
    if (!root) { json_value_free(root_val); return 1; }

    st->global_revision = (long)json_object_get_number(root, "global_revision");
    st->had_conflicts   = (json_object_get_boolean(root, "had_conflicts") == 1);

    entries = json_object_get_array(root, "entries");
    n = entries ? (int)json_array_get_count(entries) : 0;
    for (i = 0; i < n; i++) {
        JSON_Object *e = json_array_get_object(entries, i);
        const char *p  = e ? json_object_get_string(e, "path") : NULL;
        if (!p) continue;
        syncstate_upsert(st, p,
                        json_object_get_boolean(e, "is_dir") == 1,
                        (long)json_object_get_number(e, "size"),
                        (long)json_object_get_number(e, "mtime"),
                        json_object_get_string(e, "md5"),
                        json_object_get_string(e, "token"), /* NULL on an old,
                                                              * pre-WebDAV baseline file -- upsert treats that the same as "" */
                        (long)json_object_get_number(e, "revision"),
                        (long)json_object_get_number(e, "server_id"),
                        (long)json_object_get_number(e, "last_sync"));
    }

    json_value_free(root_val);
    return 1;
}

int syncstate_save(const char *local_root, const SyncState *st)
{
    char path[1024], tmp_path[1040];
    JSON_Value *root_val;
    JSON_Object *root;
    JSON_Value *entries_val;
    JSON_Array *entries;
    int i, ok;

    localfs_join(path, sizeof(path), local_root, SYNCSTATE_FILE);
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    /* parson escapes '/' by default ("xml/html-embeddable" upstream
     * rationale) -- pointless noise for a local file whose only readers
     * are this module and, occasionally, a human grepping their own sync
     * paths. Global parson setting, but harmless: nothing else in the
     * tree serializes JSON (only parses), so there's no other writer to
     * clash with. */
    json_set_escape_slashes(0);

    root_val = json_value_init_object();
    if (!root_val) return 0;
    root = json_value_get_object(root_val);
    json_object_set_number (root, "global_revision", (double)st->global_revision);
    json_object_set_boolean(root, "had_conflicts", st->had_conflicts);

    entries_val = json_value_init_array();
    if (!entries_val) { json_value_free(root_val); return 0; }
    entries = json_value_get_array(entries_val);

    for (i = 0; i < st->n; i++) {
        const SyncStateEntry *e = &st->items[i];
        JSON_Value *ev = json_value_init_object();
        JSON_Object *eo;
        if (!ev) continue;
        eo = json_value_get_object(ev);
        json_object_set_string (eo, "path",      e->path);
        json_object_set_boolean(eo, "is_dir",    e->is_dir);
        json_object_set_number (eo, "size",      (double)e->size);
        json_object_set_number (eo, "mtime",     (double)e->mtime);
        json_object_set_string (eo, "md5",       e->md5);
        json_object_set_string (eo, "token",     e->token);
        json_object_set_number (eo, "revision",  (double)e->revision);
        json_object_set_number (eo, "server_id", (double)e->server_id);
        json_object_set_number (eo, "last_sync", (double)e->last_sync);
        json_array_append_value(entries, ev);
    }
    json_object_set_value(root, "entries", entries_val);

    ok = (json_serialize_to_file_pretty(root_val, tmp_path) == JSONSuccess);
    json_value_free(root_val);
    if (!ok) { remove(tmp_path); return 0; }

    /* AmigaDOS Rename() fails outright if the destination already
     * exists (see dos.doc's Rename() autodoc) -- unlike POSIX rename(),
     * it never replaces an existing target. Only the very first save of
     * a given baseline (no prior sync yet) would ever succeed without
     * this; every save after that would silently leave the OLD baseline
     * in place, forcing every file to look "new" again on the next sync
     * forever (see libcloudcore's config_save(), which had the
     * identical bug -- same fix, same reasoning). Delete the old target
     * first; if there isn't one yet (first sync), this simply fails
     * harmlessly. */
    remove(path);
    if (rename(tmp_path, path) != 0) { remove(tmp_path); return 0; }
    return 1;
}

SyncStateEntry *syncstate_find(SyncState *st, const char *path)
{
    int i;
    for (i = 0; i < st->n; i++) {
        if (strcmp(st->items[i].path, path) == 0) return &st->items[i];
    }
    return NULL;
}

void syncstate_upsert(SyncState *st, const char *path, int is_dir, long size, long mtime,
                      const char *md5, const char *token,
                      long revision, long server_id, long last_sync)
{
    SyncStateEntry *e = syncstate_find(st, path);
    if (!e) {
        if (st->n == st->cap) {
            int newcap = st->cap ? st->cap * 2 : 64;
            SyncStateEntry *tmp = (SyncStateEntry *)realloc(st->items, sizeof(SyncStateEntry) * (size_t)newcap);
            if (!tmp) return;
            st->items = tmp;
            st->cap = newcap;
        }
        e = &st->items[st->n++];
        memset(e, 0, sizeof(*e));
        e->path = ss_dupstr(path);
    }
    e->is_dir    = is_dir;
    e->size      = size;
    e->mtime     = mtime;
    strncpy(e->md5, md5 ? md5 : "", sizeof(e->md5) - 1);
    e->md5[sizeof(e->md5) - 1] = '\0';
    strncpy(e->token, token ? token : "", sizeof(e->token) - 1);
    e->token[sizeof(e->token) - 1] = '\0';
    e->revision  = revision;
    e->server_id = server_id;
    e->last_sync = last_sync;
}

void syncstate_remove(SyncState *st, const char *path)
{
    int i;
    for (i = 0; i < st->n; i++) {
        if (strcmp(st->items[i].path, path) == 0) {
            free(st->items[i].path);
            st->items[i] = st->items[st->n - 1];
            st->n--;
            return;
        }
    }
}

void syncstate_free(SyncState *st)
{
    int i;
    for (i = 0; i < st->n; i++) free(st->items[i].path);
    free(st->items);
    st->items = NULL;
    st->n = st->cap = 0;
    st->global_revision = 0;
}
