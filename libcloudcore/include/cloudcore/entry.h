/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef CLOUDCORE_ENTRY_H
#define CLOUDCORE_ENTRY_H

/*
 * entry.h — shared directory-entry / info types, extracted from the old
 * client/cmd.h so libcloudhttp, libcloudfs, libcloudsync and every app can
 * depend on the same struct layout without depending on libcloudhttp's
 * cmd_*() API surface (module-encapsulation rule: types are core, HTTP
 * verbs are libcloudhttp's own business).
 */

/* Anzeige-Modi fuer die Remote-Auflistung (Reihenfolge immer: Name zuerst). */
typedef enum { LIST_FULL, LIST_NAMES, LIST_SIZE, LIST_DATE, LIST_MD5 } ListMode;

/* One directory entry as returned by cmd_list_raw(). */
typedef struct {
    char name[256];
    int  is_dir;
    long size;
    char modified[24]; /* ISO-8601 UTC, e.g. "2026-06-25T14:30:00Z"; "" if missing */
    long mtime;         /* UTC epoch seconds; 0 if missing */
    char md5[33];       /* 32 hex chars; "" for directories/missing value */
    long protection;    /* raw Amiga bits, RWED active-low, docs/API.md */
    char comment[80];   /* "" if no comment set */
    /* Server-side sync metadata (docs/architecture/07_Sync_Metadaten.md) --
     * all three 0 if the server doesn't send them (older deployment) or
     * the path has no metadata row yet; callers fall back to plain MD5
     * comparison in that case. */
    long id;            /* server-side file ID (files.id); 0 if unknown */
    long revision;      /* content revision, 1 at creation, ++ on content change */
    long change_seq;     /* global_revision at this row's last change */
} AcEntry;

/* Structured variant of the CLI's `info` command (GUI Commander's F3
 * dialog, and anything else that wants fields without printed output). */
typedef struct {
    char name[256];
    char type[8];       /* "file" or "dir" */
    long size;
    char mime[64];
    char created[24];
    char modified[24];
    long mtime;
    char md5[33];
    long protection;
    char comment[80];
    long id;            /* see AcEntry */
    long revision;
    long change_seq;
} AcInfo;

/* One row from cmd_changes() (action=changes, docs/architecture/
 * 07_Sync_Metadaten.md) -- the delta feed. Unlike AcEntry, entries here
 * are not scoped to one directory, so `path` is a full '/'-prefixed
 * path and needs the same length budget as any other full remote path
 * (AC_MAX_PATH elsewhere), not a directory-entry-sized name[256]. */
typedef struct {
    char path[1024];
    int  is_dir;
    long size;
    char md5[33];
    long mtime;
    long revision;
    long change_seq;
    long id;
    int  deleted;   /* 1 = tombstone (soft-deleted on the server) */
} AcChange;

/* One row from cmd_search() (action=search, docs/architecture/
 * search-plan.md) -- like AcChange, not scoped to one directory, so
 * `path` is a full '/'-prefixed path. Mirrors common's local-search
 * SearchHit (path/is_dir/size/mtime) so Commander can display cloud and
 * local hits through the same result-list shape. */
typedef struct {
    char path[1024];
    int  is_dir;
    long size;
    long mtime;
} AcSearchHit;

#endif /* CLOUDCORE_ENTRY_H */
