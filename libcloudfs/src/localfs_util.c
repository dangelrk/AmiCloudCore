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
#include <stdlib.h>
#ifdef PLATFORM_AMIGA
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#endif

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
 * really became. Elsewhere 255. A failed probe gives the safe 30.
 * AMICLOUD_NAME_MAX=<n> in the environment overrides it (tests: a
 * classic FFS limit on the PC). */
static int probe_name_max(const char *dir);

int localfs_name_max(const char *dir)
{
    const char *env = getenv("AMICLOUD_NAME_MAX");
    int forced = env ? atoi(env) : 0;
    return forced > 0 ? forced : probe_name_max(dir);
}

int localfs_name_max_guess(const char *dir)
{
#ifdef PLATFORM_AMIGA
    const char *env = getenv("AMICLOUD_NAME_MAX");
    int forced = env ? atoi(env) : 0;
    struct InfoData *info;
    BPTR lock;
    int max = 30;
    if (forced > 0) return forced;
    info = (struct InfoData *)AllocMem(sizeof(struct InfoData), MEMF_PUBLIC | MEMF_CLEAR);
    if (!info) return 30;
    lock = Lock((STRPTR)(dir && *dir ? dir : ""), ACCESS_READ);
    if (lock) {
        if (Info(lock, info)) {
            ULONG t = (ULONG)info->id_DiskType;
            if ((t & 0xFFFFFF00UL) != 0x444F5300UL || (t & 0xFF) > 5) max = 255;
        }
        UnLock(lock);
    }
    FreeMem(info, sizeof(struct InfoData));
    return max;
#else
    return localfs_name_max(dir);
#endif
}

int localfs_name_fits(const char *path, int *max_out)
{
    static char last_dir[AC_MAX_PATH];
    static int  last_max = 0;
    char dir[AC_MAX_PATH];
    const char *cut = NULL, *p;
    size_t dlen;
    int max;

    for (p = path; *p; p++) {
        if (*p == '/' || *p == ':'
#ifdef PLATFORM_WIN
            || *p == '\\'
#endif
           ) cut = p;
    }
    dlen = cut ? (size_t)(cut - path) + (*cut == ':' || cut == path ? 1 : 0) : 0;   /* "DH0:", "/" */
    if (dlen >= sizeof(dir)) dlen = sizeof(dir) - 1;
    memcpy(dir, path, dlen);
    dir[dlen] = '\0';
    if (last_max > 0 && strcmp(dir, last_dir) == 0) {
        max = last_max;
    } else {
        max = localfs_name_max(dir);
        strcpy(last_dir, dir);
        last_max = max;
    }
    if (max_out) *max_out = max;
    return (int)strlen(cut ? cut + 1 : path) <= max;
}

static int probe_name_max(const char *dir)
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
    char bak[AC_MAX_PATH];
    if (!localfs_exists(dst)) return rename(src, dst) == 0;
    /* Never delete dst before src is in place: if the second rename
     * failed, both would be gone. Move dst aside, put src there, and only
     * then drop the old one -- or move it back. */
    localfs_sibling(bak, sizeof(bak), dst, ".amicloudsync.old");
    remove(bak);
    if (rename(dst, bak) != 0) return 0;
    if (rename(src, dst) != 0) {
        rename(bak, dst);
        return 0;
    }
    remove(bak);
    return 1;
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

/* ------------------------------------------------------------------ */
/* Virus check through xvs.library (AmigaOS)                            */
/* ------------------------------------------------------------------ */
#ifdef PLATFORM_AMIGA
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/libraries.h>
#include <proto/exec.h>

/* first fields of xvs.library's struct xvsFileInfo (libraries/xvs.h) */
struct lf_xvsFileInfo {
    APTR   File;
    ULONG  FileLen;
    STRPTR Name;
    UBYTE  FileType;
    UBYTE  ModifiedFlag;
};
#define LF_XVSOBJ_FILEINFO 3
#define LF_XVSFT_DATAVIRUS 4
#define LF_XVSFT_LINKVIRUS 6

/* jump table, xvs_lib.fd (bias 30) */
APTR  __lfxvsAllocObject(__reg("a6") struct Library *, __reg("d0") ULONG type) = "\tjsr\t-48(a6)";
void  __lfxvsFreeObject(__reg("a6") struct Library *, __reg("a1") APTR obj) = "\tjsr\t-54(a6)";
ULONG __lfxvsCheckFile(__reg("a6") struct Library *, __reg("a0") APTR info) = "\tjsr\t-96(a6)";

static struct Library *g_xvs;
static int g_xvs_tried;

static struct Library *xvs_base(void)
{
    if (!g_xvs_tried) {
        g_xvs_tried = 1;
        g_xvs = OpenLibrary((STRPTR)"xvs.library", 33);   /* closed by the OS at exit */
    }
    return g_xvs;
}

int localfs_virus_available(void) { return xvs_base() != NULL; }

int localfs_virus_check(const char *path, long max_bytes, char *name_out, size_t namesz)
{
    struct Library *xb = xvs_base();
    struct lf_xvsFileInfo *fi;
    FILE *fp;
    long size;
    UBYTE *buf;
    int infected = 0;

    if (name_out && namesz) name_out[0] = '\0';
    if (!xb) return 0;
    if (max_bytes <= 0) max_bytes = LOCALFS_VIRUS_MAX;
    fp = fopen(path, "rb");
    if (!fp) return -1;
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size <= 0 || size > max_bytes) { fclose(fp); return 0; }
    buf = (UBYTE *)AllocVec((ULONG)size, MEMF_ANY);
    if (!buf) { fclose(fp); return 0; }   /* no memory: unchecked, not an error */
    if (fread(buf, 1, (size_t)size, fp) != (size_t)size) { fclose(fp); FreeVec(buf); return -1; }
    fclose(fp);

    fi = (struct lf_xvsFileInfo *)__lfxvsAllocObject(xb, LF_XVSOBJ_FILEINFO);
    if (fi) {
        ULONG type;
        fi->File = buf;
        fi->FileLen = (ULONG)size;
        type = __lfxvsCheckFile(xb, fi);
        if (type >= LF_XVSFT_DATAVIRUS && type <= LF_XVSFT_LINKVIRUS) {
            infected = 1;
            if (name_out && namesz) {
                strncpy(name_out, fi->Name ? (const char *)fi->Name : "?", namesz - 1);
                name_out[namesz - 1] = '\0';
            }
        }
        __lfxvsFreeObject(xb, fi);
    }
    FreeVec(buf);
    return infected;
}
#else
int localfs_virus_available(void) { return 0; }

int localfs_virus_check(const char *path, long max_bytes, char *name_out, size_t namesz)
{
    (void)path; (void)max_bytes;
    if (name_out && namesz) name_out[0] = '\0';
    return 0;
}
#endif

/* ------------------------------------------------------------------ */
/* Local trash hook (see localfs.h)                                     */
/* ------------------------------------------------------------------ */
static LocalTrashFn g_trash_hook = NULL;

void localfs_set_trash_hook(LocalTrashFn fn)
{
    g_trash_hook = fn;
}

int localfs_trash_active(void)
{
    return g_trash_hook != NULL;
}

int localfs_trash(const char *path, int is_dir)
{
    if (g_trash_hook) return g_trash_hook(path, is_dir) ? 1 : 0;
    return localfs_remove(path);
}
