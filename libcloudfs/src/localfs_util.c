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

/* Longest file name dir accepts (bytes). AmigaOS: classic FFS silently
 * truncates longer names to 30 -- when creating AND when opening, so two
 * long names with the same first 30 characters end up in one file -- and
 * the DiskType does not tell (RAM: says DOS\0 but takes long names). So:
 * create a file with a 120-character name and read back how long its name
 * really became. Elsewhere 255. A failed probe gives the safe 30. */
int localfs_name_max(const char *dir)
{
#ifdef PLATFORM_AMIGA
    static const char prefix[] = ".amicloudsync.namelen";
    char name[121], path[AC_MAX_PATH];
    LocalDir *d;
    LocalDirEntry e;
    FILE *fp;
    int got = 0;
    size_t i;

    strcpy(name, prefix);
    for (i = strlen(prefix); i < 120; i++) name[i] = (char)('a' + i % 26);
    name[120] = '\0';
    localfs_join(path, sizeof(path), dir, name);
    fp = fopen(path, "wb");
    if (!fp) return 30;
    fclose(fp);
    d = localfs_opendir(dir);
    if (d) {
        while (localfs_readdir(d, &e)) {
            if (strncmp(e.name, prefix, sizeof(prefix) - 1) == 0) {
                got = (int)strlen(e.name);
                localfs_join(path, sizeof(path), dir, e.name);
                break;
            }
        }
        localfs_closedir(d);
    }
    remove(path);
    if (got <= 0) return 30;
    return got < 120 ? got : 255;
#else
    (void)dir;
    return 255;
#endif
}

/* Probe: create a file with a lower-case name and look for it under the
 * upper-case one. Any failure counts as case-insensitive, the safe
 * assumption for sync (AmigaOS, macOS by default and Windows are). */
int localfs_case_sensitive(const char *dir)
{
    char lower[AC_MAX_PATH], upper[AC_MAX_PATH];
    FILE *fp;
    int sensitive;

    localfs_join(lower, sizeof(lower), dir, ".amicloudsync.case");
    localfs_join(upper, sizeof(upper), dir, ".AMICLOUDSYNC.CASE");
    fp = fopen(lower, "wb");
    if (!fp) return 0;
    fclose(fp);
    sensitive = !localfs_exists(upper);
    remove(lower);
    return sensitive;
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
