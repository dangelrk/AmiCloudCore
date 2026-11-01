/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef FAKE_TRANSPORT_H
#define FAKE_TRANSPORT_H

/*
 * fake_transport.h -- an in-memory remote store behind the SyncTransport
 * interface, for the host unit tests of the sync kernel (test_sync.c).
 * No network, no curl: every remote file is a malloc'd buffer.
 *
 * Two flavours, matching the two real backends:
 *
 *   FAKE_AMICLOUD  like api.php: real MD5 in md5 and token, numeric
 *                  revision and id, global_rev() counter, a directory
 *                  delete refuses a non-empty directory (409).
 *   FAKE_WEBDAV    like ownCloud/Nextcloud: md5 "", token = ETag,
 *                  revision/id 0, no global_rev(), DELETE removes a
 *                  directory with everything below it.
 *
 * Paths are absolute ("/r/a/b.txt"); repeated slashes are collapsed, a
 * trailing slash is dropped. "/" is the always-present root.
 */

#include "transport.h"

typedef enum { FAKE_AMICLOUD, FAKE_WEBDAV } FakeKind;

typedef enum { FOP_LIST, FOP_GET, FOP_PUT, FOP_DEL, FOP_MKDIR, FOP_SETDATE, FOP_CHANGES, FOP__N } FakeOp;

typedef struct {
    char *path;
    int   is_dir;
    unsigned char *data;
    long  size;
    long  mtime;
    long  revision;
    long  id;
    long  etag;
} FakeNode;

typedef struct FakeTransport FakeTransport;

/* Called right before an operation executes (after the failure check):
 * lets a test simulate "another client changed something meanwhile". */
typedef void (*FakeHook)(FakeTransport *f, FakeOp op, const char *path, void *ud);

struct FakeTransport {
    SyncTransport t;      /* must stay first: sync.c only sees this */
    FakeKind kind;
    FakeNode *nodes;
    int n, cap;
    long global;          /* bumped by every change */
    long next_id, next_etag;
    int calls[FOP__N];
    /* Failure injection: an operation on a path that equals fail_path[op]
     * or lies below it fails (returns the error code of that op). */
    char fail_path[FOP__N][256];
    int no_conditions;    /* 1: ignore put() conditions (old server) */
    int fail_breaks;      /* 1: an injected failure also sets the break flag,
                           * like a transfer cancelled by Pause/Ctrl-C */
    FakeHook hook;
    void *hook_ud;
    /* Delta feed (AmiCloud kind only, like the server's action=changes):
     * every change appends (path, global) here. */
    struct FakeLog { char *path; long seq; } *log;
    int nlog, caplog;
    int changes_limit;    /* rows per page, 0 = all */
    int changes_too_old;  /* 1: answer like 410 */
    int changes_calls;    /* pages fetched since fake_reset_calls() */
};

void fake_init(FakeTransport *f, FakeKind kind);
void fake_free(FakeTransport *f);

/* Direct manipulation from a test (no call counting, no hook). Parent
 * directories are created as needed. */
void fake_put_data(FakeTransport *f, const char *path, const char *content, long mtime);
void fake_mkdir(FakeTransport *f, const char *path, long mtime);
int  fake_remove(FakeTransport *f, const char *path);  /* recursive */
FakeNode *fake_find(FakeTransport *f, const char *path);
/* 1 if path exists as a file with exactly this content */
int  fake_has(FakeTransport *f, const char *path, const char *content);
void fake_reset_calls(FakeTransport *f);
/* A change made behind the fake's helpers (direct FakeNode edits):
 * bumps the counter and logs the path, like a server write would. */
void fake_touch(FakeTransport *f, const char *path);
/* 1: the transport offers no delta feed (like WebDAV or an old server) */
void fake_no_changes(FakeTransport *f, int off);
void fake_fail(FakeTransport *f, FakeOp op, const char *path); /* path NULL = off */

#endif /* FAKE_TRANSPORT_H */
