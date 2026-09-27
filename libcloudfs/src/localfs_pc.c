/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * localfs_pc.c — local filesystem adapter for macOS, Linux and Windows.
 * Compiled when PLATFORM_AMIGA is NOT defined.
 */
#ifndef PLATFORM_AMIGA

#include "localfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>

#ifdef PLATFORM_WIN
#include <direct.h>
#include <sys/utime.h>
#define AC_GETCWD(buf, sz) _getcwd((buf), (int)(sz))
#define AC_CHDIR(p)        _chdir(p)
#define AC_MKDIR(p)        _mkdir(p)
#define AC_UTIME(p, buf)   _utime((p), (buf))
typedef struct _utimbuf AcUtimbuf;
#else
#include <unistd.h>
#include <utime.h>
#define AC_GETCWD(buf, sz) getcwd((buf), (sz))
#define AC_CHDIR(p)        chdir(p)
#define AC_MKDIR(p)        mkdir((p), 0755)
#define AC_UTIME(p, buf)   utime((p), (buf))
typedef struct utimbuf AcUtimbuf;
#endif

struct LocalDir {
    DIR  *dp;
    char  path[AC_MAX_PATH]; /* needed to stat() children for is_dir/size */
};

int localfs_getcwd(char *buf, size_t sz)
{
    return AC_GETCWD(buf, sz) != NULL;
}

int localfs_chdir(const char *path)
{
    return AC_CHDIR(path) == 0;
}

int localfs_mkdir(const char *path)
{
    if (AC_MKDIR(path) == 0) return 1;
    return errno == EEXIST;
}

int localfs_is_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (st.st_mode & S_IFMT) == S_IFDIR;
}

int localfs_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

LocalDir *localfs_opendir(const char *path)
{
    LocalDir *d;
    DIR *dp = opendir(path);
    if (!dp) return NULL;

    d = (LocalDir *)malloc(sizeof(LocalDir));
    if (!d) { closedir(dp); return NULL; }
    d->dp = dp;
    strncpy(d->path, path, sizeof(d->path) - 1);
    d->path[sizeof(d->path) - 1] = '\0';
    return d;
}

int localfs_readdir(LocalDir *d, LocalDirEntry *out)
{
    struct dirent *e;

    while ((e = readdir(d->dp)) != NULL) {
        char full[AC_MAX_PATH];
        struct stat st;

        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;

        strncpy(out->name, e->d_name, sizeof(out->name) - 1);
        out->name[sizeof(out->name) - 1] = '\0';

        snprintf(full, sizeof(full), "%s/%s", d->path, e->d_name);
        if (stat(full, &st) == 0) {
            out->is_dir = (st.st_mode & S_IFMT) == S_IFDIR;
            out->size   = out->is_dir ? 0L : (long)st.st_size;
            out->mtime  = (long)st.st_mtime; /* bereits UTC-Epoche auf POSIX/Win */
            strftime(out->modified, sizeof(out->modified), "%Y-%m-%d %H:%M:%S",
                     localtime(&st.st_mtime));
        } else {
            out->is_dir = 0;
            out->size   = 0L;
            out->mtime  = 0L;
            out->modified[0] = '\0';
        }
        return 1;
    }
    return 0;
}

void localfs_closedir(LocalDir *d)
{
    if (!d) return;
    closedir(d->dp);
    free(d);
}

/* PC libc mtimes are already UTC epoch — gmt_offset/dst are an
 * Amiga-only concept (plan section 19.2), nothing to do here. */
void localfs_set_tz(int gmt_offset_minutes, int dst)
{
    (void)gmt_offset_minutes;
    (void)dst;
}

int localfs_stat(const char *path, long *size_out, long *mtime_out)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    if (size_out)  *size_out  = (st.st_mode & S_IFMT) == S_IFDIR ? 0L : (long)st.st_size;
    if (mtime_out) *mtime_out = (long)st.st_mtime;
    return 1;
}

int localfs_utime(const char *path, long mtime_epoch)
{
    AcUtimbuf buf;
    buf.actime  = (time_t)mtime_epoch;
    buf.modtime = (time_t)mtime_epoch;
    return AC_UTIME(path, &buf) == 0;
}

int localfs_remove(const char *path)
{
    /* Plain C89 remove() -- unlinks a file or rmdir()s an empty directory
     * uniformly on POSIX and the Windows CRT, no platform split needed. */
    return remove(path) == 0;
}

#endif /* !PLATFORM_AMIGA */
