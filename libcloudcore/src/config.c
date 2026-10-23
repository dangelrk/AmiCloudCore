/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Strip leading/trailing whitespace in-place */
static void strip(char *s)
{
    char *p = s + strlen(s) - 1;
    while (p >= s && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
        *p-- = '\0';
    p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

int config_load(const char *filepath, AcConfig *cfg)
{
    FILE *fp;
    /* line/val must hold the longest field this file can contain --
     * cfg->server is AC_MAX_URL (2048) chars. A value longer than val[]
     * doesn't just get truncated: fgets() stops mid-line (no '\n' seen
     * yet), and the next fgets() call resumes with the *leftover* raw
     * text as if it were a brand-new line. That fragment almost never
     * matches "key = val" (no '=' in it), so it's silently skipped --
     * and worse, so is whatever real line follows, until fgets()
     * re-syncs on the next actual '\n'. Sized to AC_MAX_URL so no
     * legitimately-sized field can ever trigger that desync (see
     * pairs.c's pairs_load(), which had the same bug for pair= lines
     * and is sized the same way for the same reason). */
    char  line[AC_MAX_URL + 128], key[64], val[AC_MAX_URL];

    /* Set safe defaults */
    cfg->basepath[0] = '/';
    cfg->basepath[1] = '\0';
    cfg->verify_ssl  = 1;
    cfg->debug[0]    = '\0';
    cfg->gmt_offset  = 0;
    cfg->dst         = 0;
    cfg->timeout     = 0;
    cfg->cache_ttl   = 0;

    fp = fopen(filepath, "r");
    if (!fp) return 0;

    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        /* 2047 == sizeof(val) - 1 (AC_MAX_URL - 1) -- keep in sync with
         * val[]'s declaration above. */
        if (sscanf(line, " %63[^= \t] = %2047[^\n]", key, val) != 2) continue;
        strip(key);
        strip(val);

        if      (strcmp(key, "server")   == 0) strncpy(cfg->server,   val, sizeof(cfg->server)   - 1);
        else if (strcmp(key, "username") == 0 ||
                 strcmp(key, "user")     == 0) strncpy(cfg->username, val, sizeof(cfg->username) - 1);
        else if (strcmp(key, "password") == 0 ||
                 strcmp(key, "pass")     == 0) strncpy(cfg->password, val, sizeof(cfg->password) - 1);
        else if (strcmp(key, "basepath") == 0) strncpy(cfg->basepath, val, sizeof(cfg->basepath) - 1);
        else if (strcmp(key, "verify_ssl") == 0) cfg->verify_ssl = atoi(val);
        else if (strcmp(key, "debug")    == 0) strncpy(cfg->debug,    val, sizeof(cfg->debug)    - 1);
        else if (strcmp(key, "gmt_offset") == 0) cfg->gmt_offset = atoi(val);
        else if (strcmp(key, "dst")       == 0) cfg->dst        = atoi(val);
        else if (strcmp(key, "timeout")   == 0) cfg->timeout    = atoi(val);
        else if (strcmp(key, "cache_ttl") == 0) cfg->cache_ttl  = atoi(val);
    }
    fclose(fp);
    return 1;
}

void config_default_path(const char *basename,
                         char *primary,  size_t primary_sz,
                         char *fallback, size_t fallback_sz)
{
#ifdef PLATFORM_AMIGA
    snprintf(primary, primary_sz, "ENV:%s.cfg", basename);
    if (fallback_sz) snprintf(fallback, fallback_sz, "%s.cfg", basename);
#elif defined(PLATFORM_WIN)
    snprintf(primary, primary_sz, "%s.cfg", basename);
    if (fallback_sz) fallback[0] = '\0';
#else
    {
        const char *home = getenv("HOME");
        if (home) snprintf(primary, primary_sz, "%s/.%src", home, basename);
        else      snprintf(primary, primary_sz, ".%src", basename);
    }
    /* No CWD-relative fallback here (unlike this function's own history)
     * -- either it's in $HOME or it's simply not configured yet, same
     * "no third guess" rule the Amiga side settled on for
     * ENV:/ENVARC:AmiCloudSync.cfg. Matches PLATFORM_WIN's own
     * no-fallback behaviour above. */
    if (fallback_sz) fallback[0] = '\0';
#endif
}

int config_save(const char *filepath, const AcConfig *cfg, unsigned flags)
{
    char tmp_path[AC_MAX_PATH + 8];
    FILE *fp;

    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", filepath);
    fp = fopen(tmp_path, "w");
    if (!fp) return 0;

    /* user=/pass=, not username=/password= -- the spelling every config
     * file/example in this repo already uses (see pairs_save() in
     * amicloud-sync-cx/src/pairs.c, which this mirrors). */
    fprintf(fp, "server=%s\n",      cfg->server);
    fprintf(fp, "user=%s\n",        cfg->username);
    fprintf(fp, "pass=%s\n",        cfg->password);
    fprintf(fp, "verify_ssl=%d\n",  cfg->verify_ssl);
    if (flags & CONFIG_SAVE_BASEPATH)
        fprintf(fp, "basepath=%s\n", cfg->basepath);
    if ((flags & CONFIG_SAVE_DEBUG) && cfg->debug[0])
        fprintf(fp, "debug=%s\n",    cfg->debug);
    /* No Prefs editor has fields for these (yet) -- but hand-set values
     * must survive a save from one. */
    if (cfg->timeout > 0)
        fprintf(fp, "timeout=%d\n", cfg->timeout);
    if (cfg->cache_ttl > 0)
        fprintf(fp, "cache_ttl=%d\n", cfg->cache_ttl);
    if (flags & CONFIG_SAVE_TZ) {
        fprintf(fp, "gmt_offset=%d\n", cfg->gmt_offset);
        fprintf(fp, "dst=%d\n",        cfg->dst);
    }

    if (fclose(fp) != 0) { remove(tmp_path); return 0; }
    /* AmigaDOS Rename() fails outright if the destination already
     * exists (see dos.doc's Rename() autodoc) -- unlike POSIX rename(),
     * it never replaces an existing target. The Windows C runtime's
     * rename() has the identical restriction. Only the very first save
     * of a given file (target doesn't exist yet) would ever succeed
     * without this; every save after that would silently do nothing --
     * confirmed as the actual cause of a 2026-07-15 field report where
     * a Prefs editor's Speichern/Benutzen stopped updating the file at
     * all after its first use. Delete the old target first; if there
     * isn't one yet (first save), this simply fails harmlessly. */
    remove(filepath);
    if (rename(tmp_path, filepath) != 0) { remove(tmp_path); return 0; }
    return 1;
}

void config_merge(AcConfig *cfg,
                  const char *server,
                  const char *username,
                  const char *password,
                  const char *basepath)
{
    if (server   && *server)   strncpy(cfg->server,   server,   sizeof(cfg->server)   - 1);
    if (username && *username) strncpy(cfg->username, username, sizeof(cfg->username) - 1);
    if (password && *password) strncpy(cfg->password, password, sizeof(cfg->password) - 1);
    if (basepath && *basepath) strncpy(cfg->basepath, basepath, sizeof(cfg->basepath) - 1);
}

void config_usage(const char *prog)
{
    fprintf(stderr,
        "AmiCloud CLI v" AC_VERSION "\n"
        "Usage: %s [options] <command> [args]\n"
        "\n"
        "Options:\n"
        "  -s <url>     Server base URL  (e.g. https://host/amicloud)\n"
        "  -u <user>    Username\n"
        "  -p <pass>    Password\n"
        "  -d <path>    Remote base path (default: /)\n"
        "  -c <file>    Config file      (default: ~/.amicloudrc or ENV:amicloud.cfg)\n"
        "  -k           Disable TLS certificate verification (development only)\n"
        "\n"
        "Commands:\n"
        "  list    [/path]              List directory\n"
        "  get     [-f] <remote> [local] Download file\n"
        "  put     [-f] <local> [remote] Upload file\n"
        "          get/put ask before overwriting an existing file;\n"
        "          -f (--force, FORCE) overwrites without asking\n"
        "  del     <remote>             Delete file or empty directory\n"
        "  info    <remote>             Show file info\n"
        "  mkdir   <remote>             Create directory\n"
        "  rename  <old> <new>          Rename/move file or directory\n"
        "  search  <pattern> [path]     Find files/dirs by name (*, ?, #?), alias: find\n"
        "  quota                        Show account and storage usage, alias: df\n"
        "  version                      Print version and exit\n",
        prog);
    exit(1);
}
