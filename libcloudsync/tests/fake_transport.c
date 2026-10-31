/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 *
 * fake_transport.c -- in-memory SyncTransport for the unit tests, see
 * fake_transport.h.
 */
#include "fake_transport.h"
#include "md5.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void norm(char *out, size_t sz, const char *in)
{
    size_t o = 0;
    if (in[0] != '/' && o + 1 < sz) out[o++] = '/';
    for (; *in && o + 1 < sz; in++) {
        if (*in == '/' && o > 0 && out[o - 1] == '/') continue;
        out[o++] = *in;
    }
    while (o > 1 && out[o - 1] == '/') o--;
    out[o] = '\0';
}

/* 1 if p equals base or lies below it */
static int under(const char *p, const char *base)
{
    size_t bl = strlen(base);
    if (strcmp(base, "/") == 0) return 1;
    return strncmp(p, base, bl) == 0 && (p[bl] == '\0' || p[bl] == '/');
}

static void parent_of(char *out, size_t sz, const char *p)
{
    const char *s = strrchr(p, '/');
    size_t l = s ? (size_t)(s - p) : 0;
    if (l == 0) { strncpy(out, "/", sz); return; }
    if (l >= sz) l = sz - 1;
    memcpy(out, p, l);
    out[l] = '\0';
}

FakeNode *fake_find(FakeTransport *f, const char *path)
{
    char p[1024];
    int i;
    norm(p, sizeof(p), path);
    for (i = 0; i < f->n; i++)
        if (strcmp(f->nodes[i].path, p) == 0) return &f->nodes[i];
    return NULL;
}

static FakeNode *add_node(FakeTransport *f, const char *p, int is_dir)
{
    FakeNode *n;
    if (f->n == f->cap) {
        f->cap = f->cap ? f->cap * 2 : 32;
        f->nodes = (FakeNode *)realloc(f->nodes, sizeof(FakeNode) * (size_t)f->cap);
    }
    n = &f->nodes[f->n++];
    memset(n, 0, sizeof(*n));
    n->path = strdup(p);
    n->is_dir = is_dir;
    n->id = ++f->next_id;
    n->etag = ++f->next_etag;
    return n;
}

static FakeNode *ensure_dir(FakeTransport *f, const char *p, long mtime)
{
    FakeNode *n = fake_find(f, p);
    char par[1024];
    if (n) return n;
    parent_of(par, sizeof(par), p);
    if (strcmp(par, p) != 0) ensure_dir(f, par, mtime);
    n = add_node(f, p, 1);
    n->mtime = mtime;
    n->revision = 1;
    f->global++;
    return n;
}

static void set_content(FakeTransport *f, FakeNode *n, const unsigned char *data, long size, long mtime)
{
    free(n->data);
    n->data = (unsigned char *)malloc(size > 0 ? (size_t)size : 1);
    if (size > 0) memcpy(n->data, data, (size_t)size);
    n->size = size;
    n->mtime = mtime;
    n->revision++;
    n->etag = ++f->next_etag;
    f->global++;
}

void fake_put_data(FakeTransport *f, const char *path, const char *content, long mtime)
{
    char p[1024], par[1024];
    FakeNode *n;
    norm(p, sizeof(p), path);
    parent_of(par, sizeof(par), p);
    ensure_dir(f, par, mtime);
    n = fake_find(f, p);
    if (!n) n = add_node(f, p, 0);
    set_content(f, n, (const unsigned char *)content, (long)strlen(content), mtime);
}

void fake_mkdir(FakeTransport *f, const char *path, long mtime)
{
    char p[1024];
    norm(p, sizeof(p), path);
    ensure_dir(f, p, mtime);
}

int fake_remove(FakeTransport *f, const char *path)
{
    char p[1024];
    int i, removed = 0;
    norm(p, sizeof(p), path);
    for (i = 0; i < f->n; ) {
        if (under(f->nodes[i].path, p)) {
            free(f->nodes[i].path);
            free(f->nodes[i].data);
            f->nodes[i] = f->nodes[--f->n];
            removed++;
        } else i++;
    }
    if (removed) f->global++;
    return removed;
}

int fake_has(FakeTransport *f, const char *path, const char *content)
{
    FakeNode *n = fake_find(f, path);
    size_t l = strlen(content);
    return n && !n->is_dir && (size_t)n->size == l && memcmp(n->data, content, l) == 0;
}

void fake_reset_calls(FakeTransport *f) { memset(f->calls, 0, sizeof(f->calls)); }

void fake_fail(FakeTransport *f, FakeOp op, const char *path)
{
    if (!path) { f->fail_path[op][0] = '\0'; return; }
    norm(f->fail_path[op], sizeof(f->fail_path[op]), path);
}

/* Counts the call, checks failure injection, runs the hook. 1 = fail. */
static int enter(FakeTransport *f, FakeOp op, const char *p)
{
    f->calls[op]++;
    if (f->fail_path[op][0] && under(p, f->fail_path[op])) return 1;
    if (f->hook) f->hook(f, op, p, f->hook_ud);
    return 0;
}

static void entry_md5(const FakeNode *n, char hex[33])
{
    md5_buffer_hex(n->data ? n->data : (const unsigned char *)"", (size_t)n->size, hex);
}

/* ---- SyncTransport ------------------------------------------------ */

static int ft_list(SyncTransport *t, const char *remote_path, SyncRemoteEntry **out, int *out_n)
{
    FakeTransport *f = (FakeTransport *)t;
    char p[1024], par[1024];
    FakeNode *dir;
    SyncRemoteEntry *arr;
    int i, k = 0;

    *out = NULL; *out_n = 0;
    norm(p, sizeof(p), remote_path);
    if (enter(f, FOP_LIST, p)) return 0;
    dir = fake_find(f, p);
    if (strcmp(p, "/") != 0 && (!dir || !dir->is_dir)) return 0;

    arr = (SyncRemoteEntry *)calloc((size_t)(f->n + 1), sizeof(SyncRemoteEntry));
    for (i = 0; i < f->n; i++) {
        const FakeNode *n = &f->nodes[i];
        SyncRemoteEntry *e;
        if (strcmp(n->path, "/") == 0) continue;
        parent_of(par, sizeof(par), n->path);
        if (strcmp(par, p) != 0) continue;
        e = &arr[k++];
        strncpy(e->name, strrchr(n->path, '/') + 1, sizeof(e->name) - 1);
        e->is_dir = n->is_dir;
        e->size = n->is_dir ? 0 : n->size;
        e->mtime = n->mtime;
        if (f->kind == FAKE_AMICLOUD) {
            if (!n->is_dir) {
                entry_md5(n, e->md5);
                strcpy(e->token, e->md5);
            }
            e->revision = n->revision;
            e->id = n->id;
        } else {
            snprintf(e->token, sizeof(e->token), "\"etag-%ld\"", n->etag);
        }
    }
    *out = arr;
    *out_n = k;
    return 1;
}

static int ft_get(SyncTransport *t, const char *remote_path, const char *local_path,
                  const char *display_name)
{
    FakeTransport *f = (FakeTransport *)t;
    char p[1024];
    FakeNode *n;
    FILE *fp;
    (void)display_name;
    norm(p, sizeof(p), remote_path);
    if (enter(f, FOP_GET, p)) return 1;
    n = fake_find(f, p);
    if (!n || n->is_dir) return 1;
    fp = fopen(local_path, "wb");
    if (!fp) return 1;
    if (n->size > 0) fwrite(n->data, 1, (size_t)n->size, fp);
    fclose(fp);
    return 0;
}

static int ft_put(SyncTransport *t, const char *local_path, const char *remote_path, long mtime,
                  const SyncPutCond *cond, SyncPutResult *res)
{
    FakeTransport *f = (FakeTransport *)t;
    char p[1024], par[1024];
    FakeNode *n, *pd;
    FILE *fp;
    unsigned char *buf;
    long size;

    norm(p, sizeof(p), remote_path);
    if (enter(f, FOP_PUT, p)) return 1;
    parent_of(par, sizeof(par), p);
    pd = fake_find(f, par);
    if (strcmp(par, "/") != 0 && (!pd || !pd->is_dir)) return 1; /* 409 Conflict */
    n = fake_find(f, p);
    if (n && n->is_dir) return 1;
    if (res) memset(res, 0, sizeof(*res));
    if (cond && !f->no_conditions) {   /* S8, like the real backends */
        char etag[64];
        if (cond->create_only && n) return SYNC_PUT_CONFLICT;
        if (f->kind == FAKE_AMICLOUD && cond->if_revision > 0 &&
            (!n || n->revision != cond->if_revision))
            return SYNC_PUT_CONFLICT;
        if (f->kind == FAKE_WEBDAV && cond->if_token && cond->if_token[0]) {
            if (n) snprintf(etag, sizeof(etag), "\"etag-%ld\"", n->etag);
            if (!n || strcmp(etag, cond->if_token) != 0) return SYNC_PUT_CONFLICT;
        }
    }

    fp = fopen(local_path, "rb");
    if (!fp) return 1;
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    buf = (unsigned char *)malloc(size > 0 ? (size_t)size : 1);
    if (size > 0 && fread(buf, 1, (size_t)size, fp) != (size_t)size) { fclose(fp); free(buf); return 1; }
    fclose(fp);

    if (!n) n = add_node(f, p, 0);
    set_content(f, n, buf, size, mtime);
    free(buf);
    if (res && f->kind == FAKE_AMICLOUD) {   /* WebDAV: the next list brings the ETag */
        entry_md5(n, res->md5);
        strcpy(res->token, res->md5);
        res->revision = n->revision;
        res->id = n->id;
    }
    return 0;
}

static int ft_del(SyncTransport *t, const char *remote_path)
{
    FakeTransport *f = (FakeTransport *)t;
    char p[1024], par[1024];
    FakeNode *n;
    int i;

    norm(p, sizeof(p), remote_path);
    if (enter(f, FOP_DEL, p)) return 1;
    n = fake_find(f, p);
    if (!n) return f->kind == FAKE_WEBDAV ? 0 : 1; /* WebDAV: 404 counts as done */
    if (n->is_dir && f->kind == FAKE_AMICLOUD) {
        for (i = 0; i < f->n; i++) {
            parent_of(par, sizeof(par), f->nodes[i].path);
            if (strcmp(par, p) == 0 && strcmp(f->nodes[i].path, p) != 0)
                return 1; /* 409 Directory not empty */
        }
    }
    fake_remove(f, p);
    return 0;
}

static int ft_mkdir_quiet(SyncTransport *t, const char *remote_path)
{
    FakeTransport *f = (FakeTransport *)t;
    char p[1024], par[1024];
    FakeNode *n, *pd;
    norm(p, sizeof(p), remote_path);
    if (enter(f, FOP_MKDIR, p)) return 1;
    n = fake_find(f, p);
    if (n) return n->is_dir ? 0 : 1;
    parent_of(par, sizeof(par), p);
    pd = fake_find(f, par);
    if (strcmp(par, "/") != 0 && (!pd || !pd->is_dir)) return 1;
    n = add_node(f, p, 1);
    n->revision = 1;
    f->global++;
    return 0;
}

static int ft_setdate(SyncTransport *t, const char *remote_path, long mtime)
{
    FakeTransport *f = (FakeTransport *)t;
    char p[1024];
    FakeNode *n;
    norm(p, sizeof(p), remote_path);
    if (enter(f, FOP_SETDATE, p)) return 1;
    n = fake_find(f, p);
    if (!n) return 1;
    n->mtime = mtime;
    return 0;
}

static int ft_global_rev(SyncTransport *t, long *global_out)
{
    FakeTransport *f = (FakeTransport *)t;
    if (f->kind != FAKE_AMICLOUD) return 0;
    *global_out = f->global;
    return 1;
}

void fake_init(FakeTransport *f, FakeKind kind)
{
    memset(f, 0, sizeof(*f));
    f->kind = kind;
    f->t.cfg = NULL;
    f->t.list = ft_list;
    f->t.get = ft_get;
    f->t.put = ft_put;
    f->t.del = ft_del;
    f->t.mkdir_quiet = ft_mkdir_quiet;
    f->t.setdate = ft_setdate;
    f->t.global_rev = ft_global_rev;
    f->t.close = NULL;
    f->global = 1;
}

void fake_free(FakeTransport *f)
{
    int i;
    for (i = 0; i < f->n; i++) { free(f->nodes[i].path); free(f->nodes[i].data); }
    free(f->nodes);
    memset(f, 0, sizeof(*f));
}
