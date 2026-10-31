/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef LOCALFS_H
#define LOCALFS_H

/*
 * localfs.h — portable local-filesystem operations for amicloud-shell
 * (lcd/lpwd/lls and the local side of `get -r`/`put -r`).
 *
 * Two implementations exist, exactly like http_pc.c / http_amiga.c:
 *   localfs_pc.c    — macOS / Linux / Windows (POSIX dirent.h + chdir
 *                      family, or the Windows CRT equivalents)
 *   localfs_amiga.c — AmigaOS via dos.library (SetCurrentDirName/
 *                      GetCurrentDirName/CreateDir/Lock+Examine+ExNext)
 *
 * cmd.c and the plain `amicloud` CLI do NOT use this module — only
 * amicloud-shell links it.
 */

#include "amicloud.h"

typedef struct {
    char name[256];
    int  is_dir;
    long size;       /* 0 for directories */
    char modified[24]; /* lokale Zeit, "YYYY-MM-DD HH:MM:SS" (keine 'Z' —
                          im Gegensatz zu den Remote-Zeitstempeln ist das
                          KEIN UTC, sondern die Systemzeit der Maschine) */
    long mtime;        /* UTC-Epoche-Sekunden, fuer sync (plan Abschnitt 19);
                          auf dem Amiga per localfs_set_tz()-Config umgerechnet */
} LocalDirEntry;

typedef struct LocalDir LocalDir; /* opaque, defined per-platform */

/* All functions return 1 on success, 0 on failure, unless noted otherwise. */

int localfs_getcwd(char *buf, size_t sz);
int localfs_chdir(const char *path);

/* Creates a single directory level; "already exists" counts as success
 * (callers are expected to create parent directories in top-down order
 * themselves, so a plain single-level mkdir is always sufficient here). */
int localfs_mkdir(const char *path);

int localfs_is_dir(const char *path);
int localfs_exists(const char *path);

/* Directory enumeration. localfs_opendir() returns NULL on failure.
 * localfs_readdir() returns 1 with *out filled, or 0 at end of directory.
 * "." and ".." are never returned. */
LocalDir *localfs_opendir(const char *path);
int       localfs_readdir(LocalDir *dir, LocalDirEntry *out);
void      localfs_closedir(LocalDir *dir);

/* ------------------------------------------------------------------ */
/* Free local paths on any drive (plan section 21) — implemented once,   */
/* platform-generically, in localfs_util.c (needs only localfs_mkdir()   */
/* and localfs_is_dir(), both already portable per-platform).            */
/* ------------------------------------------------------------------ */

/* Joins a local directory and a filename with the platform-correct
 * separator — or no separator at all if dir already ends in one (a
 * volume/assign like "DH1:" or a drive like "C:", or an explicit "/"/"\").
 * Replaces the old, naive "%s/%s" joins so "DH1:" + "name" doesn't turn
 * into the invalid "DH1:/name" (AmigaDOS: parent-of-root). dir=="" or "."
 * yields just name — AmigaDOS has no "." alias for "current directory",
 * so no "./" prefix is ever generated. */
void localfs_join(char *out, size_t sz, const char *dir, const char *name);

/* Creates path and all missing parent directories, top-down. A leading
 * volume/assign or drive prefix ("DH1:", "Work:", "C:") is left alone —
 * only the segments after it are created (the volume itself always
 * exists). "Already exists" counts as success, like localfs_mkdir(). */
int localfs_mkdir_p(const char *path);

/* out = a file called name in the same directory as path ("Work:a/b" +
 * "x" -> "Work:a/x", "DH1:b" -> "DH1:x", "b" -> "x"). */
void localfs_sibling(char *out, size_t sz, const char *path, const char *name);

/* Moves src over dst, replacing dst if it exists (AmigaDOS Rename() and
 * the Windows CRT refuse to overwrite, so dst is deleted first). Both
 * must be on the same volume. Returns 1 on success. */
int localfs_replace(const char *src, const char *dst);

/* 1 if names in dir are case-sensitive (probe file, see localfs_util.c),
 * 0 if not or if the probe fails. */
int localfs_case_sensitive(const char *dir);

/* ------------------------------------------------------------------ */
/* UTC epoch mtimes for `sync` (plan section 19) — implemented per       */
/* platform in localfs_pc.c/localfs_amiga.c, like localfs_stat/utime()   */
/* need real platform APIs (stat/utime vs. Examine/SetFileDate).         */
/* ------------------------------------------------------------------ */

/* Amiga-only: the system clock has no timezone info at all, so
 * localfs_stat()/localfs_utime() need gmt_offset/dst (AcConfig, loaded
 * from the config file) to convert DateStamp<->UTC epoch. Call once at
 * shell startup with the loaded config's values. PC builds accept and
 * ignore the call — their libc mtimes are already UTC epoch. */
void localfs_set_tz(int gmt_offset_minutes, int dst);

/* size/mtime (UTC epoch seconds) of a single local file or directory —
 * the local half of sync's comparison index. Returns 1 on success, 0 if
 * the path doesn't exist. */
int localfs_stat(const char *path, long *size_out, long *mtime_out);

/* Sets a local file's modification time to the given UTC epoch seconds
 * (sync aligns mtimes after copying, plan section 20.2). Returns 1 on
 * success. */
int localfs_utime(const char *path, long mtime_epoch);

/* Removes a single local file, or a single EMPTY local directory (not
 * recursive -- a caller wanting to remove a non-empty directory tree
 * walks it with localfs_opendir/readdir first, exactly like the server's
 * own deleteTree() and cmd.c's del_recursive_fallback() already do for
 * the remote side). Used by sync's delete-propagation (AmiOwnCloud docs/design/
 * 07_Sync_Metadaten.md). Returns 1 on success. */
int localfs_remove(const char *path);

#endif /* LOCALFS_H */
