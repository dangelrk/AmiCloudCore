/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * localfs_amiga.c — local filesystem adapter for AmigaOS via dos.library.
 * Compiled only when PLATFORM_AMIGA is defined (VBCC +aos68k).
 *
 * SetCurrentDirName()/GetCurrentDirName() do exactly what chdir()/getcwd()
 * do on the other platforms (no manual Lock()/CurrentDir()/UnLock()
 * juggling needed, so there is no lock-leak risk here). Directory
 * enumeration uses the classic Lock()+Examine()+ExNext() idiom.
 */
#ifdef PLATFORM_AMIGA

#include <dos/dos.h>
#include <dos/dosextens.h>
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "localfs.h"
#include "cloudtime.h" /* ac_civil_from_days/ac_datestamp_to_epoch/ac_epoch_to_datestamp
                        * — libcloudcore, the ONE place this conversion lives
                        * (module-encapsulation rule; was duplicated here and
                        * in cloudhandler/worker.c before the refactor). */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

struct LocalDir {
    BPTR                   lock;
    struct FileInfoBlock  *fib;
};

static void format_datestamp(const struct DateStamp *ds, char *out, size_t sz)
{
    int  y, mo, d;
    long mins = ds->ds_Minute;
    long secs = ds->ds_Tick / 50;

    if (mins < 0) mins = 0;
    if (mins > 1439) mins = 1439;
    if (secs < 0) secs = 0;
    if (secs > 59) secs = 59;

    ac_civil_from_days((long)ds->ds_Days + AC_AMIGA_EPOCH_SHIFT_DAYS, &y, &mo, &d);
    snprintf(out, sz, "%04d-%02d-%02d %02ld:%02ld:%02ld",
             y, mo, d, mins / 60, mins % 60, secs);
}

/* The Amiga system clock has no timezone info at all, so converting a
 * DateStamp to/from UTC epoch needs gmt_offset/dst from the loaded config
 * (set once via localfs_set_tz() at shell startup). */
static int g_tz_gmt_offset = 0; /* minutes relative to GMT/UTC, e.g. CET=60 */
static int g_tz_dst        = 0; /* 1 = DST active (+60 minutes) */

void localfs_set_tz(int gmt_offset_minutes, int dst)
{
    g_tz_gmt_offset = gmt_offset_minutes;
    g_tz_dst        = dst;
}

/* AmigaDOS kennt "." nicht als "aktuelles Verzeichnis" (anders als POSIX) —
 * Lock()/SetCurrentDirName() suchen sonst buchstaeblich eine Datei namens
 * ".". Die dokumentierte AmigaDOS-Konvention dafuer ist ein LEERER String:
 * "Lock(\"\", ...) liefert einen Lock auf das aktuelle Verzeichnis." */
static const char *amiga_path(const char *path)
{
    return (path[0] == '.' && path[1] == '\0') ? "" : path;
}

int localfs_getcwd(char *buf, size_t sz)
{
    return GetCurrentDirName((STRPTR)buf, (LONG)sz) ? 1 : 0;
}

int localfs_chdir(const char *path)
{
    return SetCurrentDirName((CONST_STRPTR)amiga_path(path)) ? 1 : 0;
}

int localfs_mkdir(const char *path)
{
    BPTR lock = CreateDir((CONST_STRPTR)path);
    if (lock) { UnLock(lock); return 1; }
    return IoErr() == ERROR_OBJECT_EXISTS;
}

int localfs_is_dir(const char *path)
{
    BPTR lock = Lock((CONST_STRPTR)amiga_path(path), ACCESS_READ);
    struct FileInfoBlock *fib;
    int is_dir = 0;

    if (!lock) return 0;
    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (fib) {
        if (Examine(lock, fib)) is_dir = (fib->fib_DirEntryType >= 0);
        FreeDosObject(DOS_FIB, fib);
    }
    UnLock(lock);
    return is_dir;
}

int localfs_exists(const char *path)
{
    BPTR lock = Lock((CONST_STRPTR)amiga_path(path), ACCESS_READ);
    if (!lock) return 0;
    UnLock(lock);
    return 1;
}

LocalDir *localfs_opendir(const char *path)
{
    LocalDir *d;
    BPTR lock = Lock((CONST_STRPTR)amiga_path(path), ACCESS_READ);
    if (!lock) return NULL;

    d = (LocalDir *)malloc(sizeof(LocalDir));
    if (!d) { UnLock(lock); return NULL; }

    d->fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (!d->fib) { free(d); UnLock(lock); return NULL; }

    if (!Examine(lock, d->fib)) {
        FreeDosObject(DOS_FIB, d->fib);
        free(d);
        UnLock(lock);
        return NULL;
    }

    d->lock = lock;
    return d;
}

int localfs_readdir(LocalDir *d, LocalDirEntry *out)
{
    if (!ExNext(d->lock, d->fib)) return 0;

    strncpy(out->name, d->fib->fib_FileName, sizeof(out->name) - 1);
    out->name[sizeof(out->name) - 1] = '\0';
    out->is_dir = (d->fib->fib_DirEntryType >= 0);
    out->size   = out->is_dir ? 0L : (long)d->fib->fib_Size;
    out->mtime  = ac_datestamp_to_epoch(&d->fib->fib_Date, g_tz_gmt_offset, g_tz_dst);
    format_datestamp(&d->fib->fib_Date, out->modified, sizeof(out->modified));
    return 1;
}

void localfs_closedir(LocalDir *d)
{
    if (!d) return;
    FreeDosObject(DOS_FIB, d->fib);
    UnLock(d->lock);
    free(d);
}

int localfs_stat(const char *path, long *size_out, long *mtime_out)
{
    BPTR lock = Lock((CONST_STRPTR)amiga_path(path), ACCESS_READ);
    struct FileInfoBlock *fib;
    int ok = 0;

    if (!lock) return 0;
    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (fib) {
        if (Examine(lock, fib)) {
            if (size_out)  *size_out  = (fib->fib_DirEntryType >= 0) ? 0L : (long)fib->fib_Size;
            if (mtime_out) *mtime_out = ac_datestamp_to_epoch(&fib->fib_Date, g_tz_gmt_offset, g_tz_dst);
            ok = 1;
        }
        FreeDosObject(DOS_FIB, fib);
    }
    UnLock(lock);
    return ok;
}

int localfs_remove(const char *path)
{
    /* DeleteFile() removes a file or an empty directory uniformly on
     * AmigaDOS -- no separate rmdir-style call needed, unlike some other
     * dos.library operations. */
    return DeleteFile((CONST_STRPTR)amiga_path(path)) ? 1 : 0;
}

int localfs_utime(const char *path, long mtime_epoch)
{
    struct DateStamp ds;
    ac_epoch_to_datestamp(mtime_epoch, g_tz_gmt_offset, g_tz_dst, &ds);
    return SetFileDate((CONST_STRPTR)amiga_path(path), &ds) ? 1 : 0;
}

#endif /* PLATFORM_AMIGA */
