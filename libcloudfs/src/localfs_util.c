/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * localfs_util.c — platform-generic helpers on top of localfs_pc.c /
 * localfs_amiga.c: joining paths and creating missing parent directories
 * for local targets on arbitrary drives/volumes (plan section 21).
 *
 * Unlike localfs_{pc,amiga}.c this file is NOT platform-exclusive: both
 * functions only need localfs_mkdir()/localfs_is_dir() (already portable)
 * plus a couple of small #ifdef'd separator conventions, so one file
 * serves all four platforms — same reasoning as md5.c.
 */
#include "localfs.h"
#include <string.h>
#include <stdio.h>

void localfs_join(char *out, size_t sz, const char *dir, const char *name)
{
    size_t len;
    char   last;
    int    no_sep;

    if (!dir || !*dir || (dir[0] == '.' && dir[1] == '\0')) {
        snprintf(out, sz, "%s", name);
        return;
    }

    len  = strlen(dir);
    last = dir[len - 1];
#ifdef PLATFORM_AMIGA
    no_sep = (last == ':' || last == '/');
#elif defined(PLATFORM_WIN)
    no_sep = (last == ':' || last == '/' || last == '\\');
#else
    no_sep = (last == '/');
#endif

    if (no_sep) {
        snprintf(out, sz, "%s%s", dir, name);
#ifdef PLATFORM_WIN
    } else if (strchr(dir, '\\') != NULL) {
        snprintf(out, sz, "%s\\%s", dir, name);
#endif
    } else {
        snprintf(out, sz, "%s/%s", dir, name);
    }
}

void localfs_sibling(char *out, size_t sz, const char *path, const char *name)
{
    const char *cut = NULL, *p;

    for (p = path; *p; p++) {
        if (*p == '/' || *p == ':'
#ifdef PLATFORM_WIN
            || *p == '\\'
#endif
           ) cut = p;
    }
    if (!cut) { snprintf(out, sz, "%s", name); return; }
    snprintf(out, sz, "%.*s%s", (int)(cut - path + 1), path, name);
}

int localfs_replace(const char *src, const char *dst)
{
    remove(dst);
    return rename(src, dst) == 0;
}

int localfs_mkdir_p(const char *path)
{
    char   buf[AC_MAX_PATH];
    char  *p;
    size_t start = 0;

    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    /* Skip a leading volume/assign or drive prefix ("DH1:", "Work:",
     * "C:") — the volume itself always exists, mkdir() on it would just
     * fail. Then skip any separator(s) right after it ("C:\Daten"). */
    {
        char *colon = strchr(buf, ':');
        if (colon) start = (size_t)(colon - buf) + 1;
    }
    while (buf[start] == '/' || buf[start] == '\\') start++;

    for (p = buf + start; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char save = *p;
            *p = '\0';
            if (buf[0]) localfs_mkdir(buf);
            *p = save;
        }
    }
    return localfs_mkdir(buf);
}
