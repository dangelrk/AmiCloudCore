/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#include "cmd.h"
#include "http.h"
#include "md5.h"
#include "ui_hooks.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef PLATFORM_AMIGA
#include <exec/memory.h>
#include <proto/exec.h>
#endif

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

/* Raw HTTP status of the most recently completed request (0 = no
 * response/transport failure), set right after every http_request()/
 * http_download()/http_upload() call in this file, success or failure.
 * Exposed via cmd_last_status() for callers whose cmd_*() wrapper only
 * returns 0/1 (the CLOUD: handler's worker needs the real status for
 * precise DOS error mapping, e.g. telling a 401 apart from a 404). */
static int g_last_status = 0;

int cmd_last_status(void)
{
    return g_last_status;
}

static void build_url(char *out, size_t sz,
                      const AcConfig *cfg,
                      const char *action,
                      const char *path)
{
    snprintf(out, sz, "%s/api.php?action=%s&path=%s",
             cfg->server, action, path);
}

static void full_path(char *out, size_t sz,
                      const AcConfig *cfg,
                      const char *path)
{
    if (!path || path[0] == '\0') {
        strncpy(out, cfg->basepath, sz - 1);
        out[sz - 1] = '\0';
        return;
    }
    if (path[0] == '/') {
        strncpy(out, path, sz - 1);
    } else {
        snprintf(out, sz, "%s/%s",
                 strcmp(cfg->basepath, "/") == 0 ? "" : cfg->basepath,
                 path);
    }
    out[sz - 1] = '\0';
}

static void make_auth(char *out, size_t sz, const AcConfig *cfg)
{
    snprintf(out, sz, "%s:%s", cfg->username, cfg->password);
}

/* Percent-encodes in into out (RFC 3986 unreserved chars pass through
 * unescaped, everything else becomes %XX -- including space, as %20 per
 * docs/API.md's setdate example, not '+'). Silently truncates if out is
 * too small for the fully-encoded result. */
static void url_encode(char *out, size_t outsz, const char *in)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;

    for (; *in && o + 4 < outsz; in++) {
        unsigned char c = (unsigned char)*in;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0xF];
        }
    }
    out[o] = '\0';
}

static const char *jstr(const JSON_Object *obj, const char *key)
{
    const char *s = json_object_get_string(obj, key);
    return s ? s : "";
}

static long jlong(const JSON_Object *obj, const char *key)
{
    return (long)json_object_get_number(obj, key);
}

/* Amiga-Protection-Bits als "hsparwed"-Maske (rwed: Bit gesetzt = verboten) */
static void prot_str(long bits, char out[9])
{
    out[0] = (bits & 0x80) ? 'h' : '-';   /* HIDDEN  */
    out[1] = (bits & 0x40) ? 's' : '-';   /* SCRIPT  */
    out[2] = (bits & 0x20) ? 'p' : '-';   /* PURE    */
    out[3] = (bits & 0x10) ? 'a' : '-';   /* ARCHIVE */
    out[4] = (bits & 0x08) ? '-' : 'r';   /* READ    (invertiert) */
    out[5] = (bits & 0x04) ? '-' : 'w';   /* WRITE   (invertiert) */
    out[6] = (bits & 0x02) ? '-' : 'e';   /* EXECUTE (invertiert) */
    out[7] = (bits & 0x01) ? '-' : 'd';   /* DELETE  (invertiert) */
    out[8] = '\0';
}

static int check_status(const JSON_Object *root, const char *url)
{
    const char *st = json_object_get_string(root, "status");
    if (!st) {
        ui_notify(UI_ERROR, "amicloud: no status in response from %s", url);
        return 0;
    }
    if (strcmp(st, "ok") != 0) {
        const char *msg = json_object_get_string(root, "message");
        ui_notify(UI_ERROR, "amicloud: server error: %s", msg ? msg : "");
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* list                                                                 */
/* ------------------------------------------------------------------ */

/* Fetch + parse "list" JSON, shared by cmd_list() (prints) and
 * cmd_list_raw() (returns structured entries). Caller owns the returned
 * JSON_Value root (json_value_free) on success; NULL on any error
 * (already reported to stderr). */
static JSON_Value *fetch_list_json(const AcConfig *cfg, const char *remote_path,
                              char *rpath_out, size_t rpath_sz)
{
    char auth[192], url[AC_MAX_URL];
    HttpResponse *resp;
    JSON_Value *root_val;
    JSON_Object *root;

    make_auth(auth, sizeof(auth), cfg);
    full_path(rpath_out, rpath_sz, cfg, remote_path);
    build_url(url, sizeof(url), cfg, "list", rpath_out);

    resp = http_request("GET", url, auth, NULL, 0, cfg->verify_ssl);
    g_last_status = resp ? resp->status_code : 0;
    if (!resp || resp->status_code == 0) {
        ui_notify(UI_ERROR, "amicloud: network error");
        http_response_free(resp); return NULL;
    }
    if (!resp->body) {
        ui_notify(UI_ERROR, "amicloud: empty response (HTTP %d)", resp->status_code);
        http_response_free(resp); return NULL;
    }

    root_val = json_parse_string(resp->body);
    http_response_free(resp);
    if (!root_val) { ui_notify(UI_ERROR, "amicloud: invalid JSON"); return NULL; }
    root = json_value_get_object(root_val);
    if (!root || !check_status(root, url)) { json_value_free(root_val); return NULL; }
    return root_val;
}

/* Spaltenreihenfolge immer: Name zuerst, dann (je nach Modus) Groesse/Datum,
 * zuletzt Type. LIST_FULL zeigt alle vier Spalten. */
int cmd_list_mode(const AcConfig *cfg, const char *remote_path, ListMode mode)
{
    char rpath[AC_MAX_PATH];
    JSON_Value *root_val = fetch_list_json(cfg, remote_path, rpath, sizeof(rpath));
    JSON_Object *root;
    JSON_Array *entries;
    int n, i;

    if (!root_val) return 1;
    root = json_value_get_object(root_val);

    entries = json_object_get_array(root, "entries");
    if (!entries) {
        ui_notify(UI_ERROR, "amicloud: missing entries");
        json_value_free(root_val); return 1;
    }

    n = (int)json_array_get_count(entries);

    if (mode == LIST_FULL)
        ui_notify(UI_INFO, "%-28s  %9s  %-19s  %s", "Name", "Size", "Modified", "Type");
    else if (mode == LIST_SIZE)
        ui_notify(UI_INFO, "%-28s  %9s", "Name", "Size");
    else if (mode == LIST_DATE)
        ui_notify(UI_INFO, "%-28s  %-19s", "Name", "Modified");
    else if (mode == LIST_MD5)
        ui_notify(UI_INFO, "%-28s  %s", "Name", "MD5");
    /* LIST_NAMES: keine Kopfzeile — reine Namensliste */

    for (i = 0; i < n; i++) {
        JSON_Object *e    = json_array_get_object(entries, i);
        const char  *name = json_object_get_string(e, "name");
        const char  *traw = jstr(e, "type");
        int          dir  = (strcmp(traw[0] ? traw : "file", "dir") == 0);
        long         s    = jlong(e, "size");
        const char  *m    = jstr(e, "modified");
        const char  *sum  = jstr(e, "md5");

        if (!name) continue;

        switch (mode) {
        case LIST_NAMES:
            ui_notify(UI_INFO, "%s%s", name, dir ? "/" : "");
            break;
        case LIST_SIZE:
            ui_notify(UI_INFO, "%-28s  %9ld", name, dir ? 0L : s);
            break;
        case LIST_DATE:
            ui_notify(UI_INFO, "%-28s  %-19s", name, m);
            break;
        case LIST_MD5:
            ui_notify(UI_INFO, "%-28.28s  %s", name, dir ? "-" : (sum[0] ? sum : "-"));
            break;
        case LIST_FULL:
        default:
            ui_notify(UI_INFO, "%-28.28s  %9ld  %-19s  %s",
                      name, dir ? 0L : s, m, dir ? "[dir]" : "file");
            break;
        }
    }
    json_value_free(root_val);
    return 0;
}

int cmd_list(const AcConfig *cfg, const char *remote_path)
{
    return cmd_list_mode(cfg, remote_path, LIST_FULL);
}

/* Structured variant of cmd_list(): no output, returns a malloc'd array
 * via *out (free with cmd_entries_free). Returns 1 on success (even for
 * an empty directory, *out_n==0), 0 on error. */
int cmd_list_raw(const AcConfig *cfg, const char *remote_path, AcEntry **out, int *out_n)
{
    char rpath[AC_MAX_PATH];
    JSON_Value *root_val;
    JSON_Object *root;
    JSON_Array *entries;
    int n, i;
    AcEntry *arr;

    *out = NULL; *out_n = 0;

    root_val = fetch_list_json(cfg, remote_path, rpath, sizeof(rpath));
    if (!root_val) return 0;
    root = json_value_get_object(root_val);

    entries = json_object_get_array(root, "entries");
    if (!entries) { json_value_free(root_val); return 0; }

    n = (int)json_array_get_count(entries);
    if (n == 0) { json_value_free(root_val); return 1; }

    arr = (AcEntry *)calloc((size_t)n, sizeof(AcEntry));
    if (!arr) { json_value_free(root_val); return 0; }

    for (i = 0; i < n; i++) {
        JSON_Object *e    = json_array_get_object(entries, i);
        const char  *name = json_object_get_string(e, "name");
        const char  *mod  = json_object_get_string(e, "modified");
        const char  *sum  = jstr(e, "md5");
        if (!name) continue;
        strncpy(arr[i].name, name, sizeof(arr[i].name) - 1);
        arr[i].is_dir = (strcmp(jstr(e, "type"), "dir") == 0);
        arr[i].size   = jlong(e, "size");
        arr[i].mtime  = jlong(e, "mtime");
        if (mod)
            strncpy(arr[i].modified, mod, sizeof(arr[i].modified) - 1);
        strncpy(arr[i].md5, sum, sizeof(arr[i].md5) - 1);
        arr[i].protection = jlong(e, "protection");
        strncpy(arr[i].comment, jstr(e, "comment"), sizeof(arr[i].comment) - 1);
        arr[i].id         = jlong(e, "id");
        arr[i].revision   = jlong(e, "revision");
        arr[i].change_seq = jlong(e, "change_seq");
    }
    json_value_free(root_val);

    *out = arr; *out_n = n;
    return 1;
}

void cmd_entries_free(AcEntry *entries)
{
    free(entries);
}

/* Silent remote directory check (no stdout/stderr output) — used by the
 * shell's `cd` to verify a target before switching remote_cwd. */
int cmd_is_dir(const AcConfig *cfg, const char *remote_path)
{
    char auth[192], url[AC_MAX_URL], rpath[AC_MAX_PATH];
    HttpResponse *resp;
    JSON_Value *root_val;
    JSON_Object *root;
    const char *status, *type;
    int result = 0;

    make_auth(auth, sizeof(auth), cfg);
    full_path(rpath, sizeof(rpath), cfg, remote_path);
    build_url(url, sizeof(url), cfg, "info", rpath);

    resp = http_request("GET", url, auth, NULL, 0, cfg->verify_ssl);
    g_last_status = resp ? resp->status_code : 0;
    if (!resp || resp->status_code == 0 || !resp->body) { http_response_free(resp); return 0; }
    root_val = json_parse_string(resp->body);
    http_response_free(resp);
    if (!root_val) return 0;
    root = json_value_get_object(root_val);

    status = json_object_get_string(root, "status");
    type   = json_object_get_string(root, "type");
    if (status && strcmp(status, "ok") == 0 &&
        type && strcmp(type, "dir") == 0)
        result = 1;

    json_value_free(root_val);
    return result;
}

/* Silent connectivity/version check against action=status — used for the
 * shell's start banner. version_out receives the server version string
 * (empty on failure). Returns 1 if the server answered "ok". */
int cmd_status(const AcConfig *cfg, char *version_out, size_t version_sz, long *global_revision_out)
{
    char auth[192], url[AC_MAX_URL];
    HttpResponse *resp;
    JSON_Value *root_val;
    JSON_Object *root;
    const char *status, *version;
    int ok = 0;

    if (version_out && version_sz) version_out[0] = '\0';
    if (global_revision_out) *global_revision_out = 0;

    make_auth(auth, sizeof(auth), cfg);
    snprintf(url, sizeof(url), "%s/api.php?action=status", cfg->server);

    resp = http_request("GET", url, auth, NULL, 0, cfg->verify_ssl);
    g_last_status = resp ? resp->status_code : 0;
    if (!resp || resp->status_code == 0 || !resp->body) { http_response_free(resp); return 0; }
    root_val = json_parse_string(resp->body);
    http_response_free(resp);
    if (!root_val) return 0;
    root = json_value_get_object(root_val);

    status  = json_object_get_string(root, "status");
    version = json_object_get_string(root, "version");
    if (status && strcmp(status, "ok") == 0) {
        ok = 1;
        if (version && version_out)
            strncpy(version_out, version, version_sz - 1);
        if (global_revision_out)
            *global_revision_out = jlong(root, "global_revision");
    }
    json_value_free(root_val);
    return ok;
}

/* ------------------------------------------------------------------ */
/* changes (delta since a given global revision, docs/architecture/       */
/* 07_Sync_Metadaten.md) -- unlike list, not scoped to a directory: one   */
/* call returns every changed path for this user, tombstones included.   */
/* ------------------------------------------------------------------ */
int cmd_changes(const AcConfig *cfg, long since, AcChange **out, int *out_n, long *global_revision_out)
{
    char auth[192], url[AC_MAX_URL];
    HttpResponse *resp;
    JSON_Value *root_val;
    JSON_Object *root;
    JSON_Array *entries;
    int n, i;
    AcChange *arr;

    *out = NULL; *out_n = 0;
    if (global_revision_out) *global_revision_out = 0;

    make_auth(auth, sizeof(auth), cfg);
    snprintf(url, sizeof(url), "%s/api.php?action=changes&since=%ld", cfg->server, since);

    resp = http_request("GET", url, auth, NULL, 0, cfg->verify_ssl);
    g_last_status = resp ? resp->status_code : 0;
    if (!resp || resp->status_code == 0 || !resp->body) {
        ui_notify(UI_ERROR, "amicloud: network error");
        http_response_free(resp); return 0;
    }
    root_val = json_parse_string(resp->body);
    http_response_free(resp);
    if (!root_val) { ui_notify(UI_ERROR, "amicloud: invalid JSON"); return 0; }
    root = json_value_get_object(root_val);
    if (!root || !check_status(root, url)) { json_value_free(root_val); return 0; }

    if (global_revision_out) *global_revision_out = jlong(root, "global_revision");

    entries = json_object_get_array(root, "entries");
    if (!entries) { json_value_free(root_val); return 1; } /* no changes -- still success */

    n = (int)json_array_get_count(entries);
    if (n == 0) { json_value_free(root_val); return 1; }

    arr = (AcChange *)calloc((size_t)n, sizeof(AcChange));
    if (!arr) { json_value_free(root_val); return 0; }

    for (i = 0; i < n; i++) {
        JSON_Object *e    = json_array_get_object(entries, i);
        const char  *path = json_object_get_string(e, "path");
        if (!path) continue;
        strncpy(arr[i].path, path, sizeof(arr[i].path) - 1);
        arr[i].is_dir     = (strcmp(jstr(e, "type"), "dir") == 0);
        arr[i].size       = jlong(e, "size");
        strncpy(arr[i].md5, jstr(e, "md5"), sizeof(arr[i].md5) - 1);
        arr[i].mtime      = jlong(e, "mtime");
        arr[i].revision   = jlong(e, "revision");
        arr[i].change_seq = jlong(e, "change_seq");
        arr[i].id         = jlong(e, "id");
        arr[i].deleted    = json_object_get_boolean(e, "deleted") == 1;
    }
    json_value_free(root_val);

    *out = arr; *out_n = n;
    return 1;
}

void cmd_changes_free(AcChange *changes)
{
    free(changes);
}

/* ------------------------------------------------------------------ */
/* search                                                                */
/* ------------------------------------------------------------------ */
int cmd_search(const AcConfig *cfg, const char *root, const char *pattern,
                int dirs_too, AcSearchHit **out, int *out_n, int *truncated_out)
{
    char auth[192], url[AC_MAX_URL], rpath[AC_MAX_PATH], enc[AC_MAX_PATH];
    HttpResponse *resp;
    JSON_Value *root_val;
    JSON_Object *robj;
    JSON_Array *entries;
    int n, i;
    AcSearchHit *arr;

    *out = NULL; *out_n = 0;
    if (truncated_out) *truncated_out = 0;

    make_auth(auth, sizeof(auth), cfg);
    full_path(rpath, sizeof(rpath), cfg, root);
    url_encode(enc, sizeof(enc), pattern);
    snprintf(url, sizeof(url), "%s/api.php?action=search&path=%s&q=%s&dirs=%d",
             cfg->server, rpath, enc, dirs_too ? 1 : 0);

    resp = http_request("GET", url, auth, NULL, 0, cfg->verify_ssl);
    g_last_status = resp ? resp->status_code : 0;
    if (!resp || resp->status_code == 0 || !resp->body) {
        ui_notify(UI_ERROR, "amicloud: network error");
        http_response_free(resp); return 0;
    }
    root_val = json_parse_string(resp->body);
    http_response_free(resp);
    if (!root_val) { ui_notify(UI_ERROR, "amicloud: invalid JSON"); return 0; }
    robj = json_value_get_object(root_val);
    if (!robj || !check_status(robj, url)) { json_value_free(root_val); return 0; }

    if (truncated_out) *truncated_out = json_object_get_boolean(robj, "truncated") == 1;

    entries = json_object_get_array(robj, "entries");
    if (!entries) { json_value_free(root_val); return 1; } /* no hits -- still success */

    n = (int)json_array_get_count(entries);
    if (n == 0) { json_value_free(root_val); return 1; }

    arr = (AcSearchHit *)calloc((size_t)n, sizeof(AcSearchHit));
    if (!arr) { json_value_free(root_val); return 0; }

    for (i = 0; i < n; i++) {
        JSON_Object *e    = json_array_get_object(entries, i);
        const char  *path = json_object_get_string(e, "path");
        if (!path) continue;
        strncpy(arr[i].path, path, sizeof(arr[i].path) - 1);
        arr[i].is_dir = (strcmp(jstr(e, "type"), "dir") == 0);
        arr[i].size   = jlong(e, "size");
        arr[i].mtime  = jlong(e, "mtime");
    }
    json_value_free(root_val);

    *out = arr; *out_n = n;
    return 1;
}

void cmd_search_free(AcSearchHit *hits)
{
    free(hits);
}

/* MD5 of a single remote file via action=info — the server computes it
 * on demand or serves it from the DB (lazy backfill, plan §18.2/18.5).
 * Used by the shell's `md5 <remote>` command. */
int cmd_get_md5(const AcConfig *cfg, const char *remote_path, char *hex_out, size_t hex_sz)
{
    char auth[192], url[AC_MAX_URL], rpath[AC_MAX_PATH];
    HttpResponse *resp;
    JSON_Value *root_val;
    JSON_Object *root;
    const char *sum;

    hex_out[0] = '\0';

    make_auth(auth, sizeof(auth), cfg);
    full_path(rpath, sizeof(rpath), cfg, remote_path);
    build_url(url, sizeof(url), cfg, "info", rpath);

    resp = http_request("GET", url, auth, NULL, 0, cfg->verify_ssl);
    g_last_status = resp ? resp->status_code : 0;
    if (!resp || resp->status_code == 0 || !resp->body) {
        ui_notify(UI_ERROR, "amicloud: network error");
        http_response_free(resp); return 1;
    }
    root_val = json_parse_string(resp->body);
    http_response_free(resp);
    root = root_val ? json_value_get_object(root_val) : NULL;
    if (!root || !check_status(root, url)) { if (root_val) json_value_free(root_val); return 1; }

    if (strcmp(jstr(root, "type"), "dir") == 0) {
        ui_notify(UI_ERROR, "amicloud: md5: '%s' is a directory", remote_path);
        json_value_free(root_val);
        return 1;
    }

    sum = jstr(root, "md5");
    if (!sum[0]) {
        ui_notify(UI_ERROR, "amicloud: md5: no checksum available for '%s'", remote_path);
        json_value_free(root_val);
        return 1;
    }
    strncpy(hex_out, sum, hex_sz - 1);
    hex_out[hex_sz - 1] = '\0';
    json_value_free(root_val);
    return 0;
}

/* Expose full_path()'s basepath-relative resolution to callers outside
 * cmd.c (the shell needs it to turn a relative arg into an absolute
 * remote path for recursive get/put/del). */
void cmd_resolve_path(const AcConfig *cfg, const char *path, char *out, size_t outsz)
{
    full_path(out, outsz, cfg, path);
}

/* ------------------------------------------------------------------ */
/* get (download)                                                        */
/* ------------------------------------------------------------------ */
int cmd_get(const AcConfig *cfg, const char *remote_path, const char *local_path)
{
    char auth[192], url[AC_MAX_URL], rpath[AC_MAX_PATH];
    const char *local;
    char local_buf[AC_MAX_PATH];
    FILE *fp;
    HttpResponse *resp;
    Md5Ctx md5ctx;
    unsigned char digest[16];
    char local_hex[33];

    make_auth(auth, sizeof(auth), cfg);
    full_path(rpath, sizeof(rpath), cfg, remote_path);
    build_url(url, sizeof(url), cfg, "download", rpath);

    if (local_path && *local_path) {
        local = local_path;
    } else {
        const char *slash = strrchr(rpath, '/');
        strncpy(local_buf, slash ? slash + 1 : rpath, sizeof(local_buf) - 1);
        local_buf[sizeof(local_buf) - 1] = '\0';
        local = local_buf;
    }

    fp = fopen(local, "wb");
    if (!fp) { ui_notify(UI_ERROR, "amicloud: get: cannot open local file '%s' for writing", local); return 1; }

    /* MD5 is verified while streaming, not re-read afterwards (plan
     * section 18.3) — cheap even on 68k, the bytes pass through the CPU
     * on the way to disk anyway. */
    md5_init(&md5ctx);
    resp = http_download(url, auth, fp, cfg->verify_ssl, &md5ctx, local);
    g_last_status = resp ? resp->status_code : 0;
    fclose(fp);

    if (!resp || resp->status_code == 0) {
        if (resp && resp->aborted) {
            remove(local); /* cancelled via the progress hook - no partial file left behind */
            http_response_free(resp);
            return 1;
        }
        ui_notify(UI_ERROR, "amicloud: network error");
        http_response_free(resp); return 1;
    }
    if (resp->status_code != 200) {
        ui_notify(UI_ERROR, "amicloud: server returned HTTP %d", resp->status_code);
        http_response_free(resp); return 1;
    }

    /* Missing header (older server, or a partial/offset download) means
     * "skip the check", not an error. */
    md5_final(&md5ctx, digest);
    md5_to_hex(digest, local_hex);
    if (resp->md5_header[0] && strcmp(resp->md5_header, local_hex) != 0) {
        ui_notify(UI_ERROR,
                  "amicloud: get: MD5 mismatch for %s (server %s, got %s) -- deleting incomplete file",
                  local, resp->md5_header, local_hex);
        remove(local);
        http_response_free(resp);
        return 1;
    }

    ui_notify(UI_INFO, "Downloaded %s", local);
    http_response_free(resp);
    return 0;
}

/* ------------------------------------------------------------------ */
/* put (upload)                                                          */
/* ------------------------------------------------------------------ */
int cmd_put(const AcConfig *cfg, const char *local_path, const char *remote_path)
{
    char auth[192], url[AC_MAX_URL], rpath[AC_MAX_PATH];
    long fsize;
    FILE *fp;
    HttpResponse *resp;
    JSON_Value *root_val;
    JSON_Object *root;
    int ok;
    char md5hex[33];

    /* &md5= lets the server reject a corrupted upload with 422 before it
     * ever hits disk (plan section 18.2/18.3); an older server just
     * ignores the unknown parameter. Computed via its own streaming pass
     * (constant memory) rather than the whole-file buffer this used to
     * read for md5_buffer_hex() — see GUI-Commander-plan section 5.3: on
     * an 8 MB Amiga, malloc'ing an entire upload was a hard ceiling on
     * file size. The extra disk read is cheap next to the network transfer. */
    if (!md5_file_hex(local_path, md5hex)) {
        ui_notify(UI_ERROR, "amicloud: put: cannot open local file '%s' for reading", local_path);
        return 1;
    }

    fp = fopen(local_path, "rb");
    if (!fp) { ui_notify(UI_ERROR, "amicloud: put: cannot open local file '%s' for reading", local_path); return 1; }
    fseek(fp, 0, SEEK_END);
    fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fsize < 0) { fclose(fp); ui_notify(UI_ERROR, "amicloud: cannot determine file size"); return 1; }

    make_auth(auth, sizeof(auth), cfg);
    if (remote_path && *remote_path) {
        full_path(rpath, sizeof(rpath), cfg, remote_path);
    } else {
        const char *slash = strrchr(local_path, '/');
#ifdef PLATFORM_WIN
        const char *bslash = strrchr(local_path, '\\');
        if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
        const char *name = slash ? slash + 1 : local_path;
        snprintf(rpath, sizeof(rpath), "%s/%s",
                 strcmp(cfg->basepath, "/") == 0 ? "" : cfg->basepath, name);
    }
    snprintf(url, sizeof(url), "%s/api.php?action=upload&path=%s&md5=%s",
             cfg->server, rpath, md5hex);

    resp = http_upload(url, auth, fp, fsize, cfg->verify_ssl, local_path);
    g_last_status = resp ? resp->status_code : 0;
    fclose(fp);

    if (!resp || resp->status_code == 0) {
        if (resp && resp->aborted) { http_response_free(resp); return 1; }
        ui_notify(UI_ERROR, "amicloud: network error");
        http_response_free(resp); return 1;
    }
    root_val = json_parse_string(resp->body ? resp->body : "{}");
    http_response_free(resp);
    root = root_val ? json_value_get_object(root_val) : NULL;
    ok = root && check_status(root, url);
    if (root_val) json_value_free(root_val);
    if (ok) ui_notify(UI_INFO, "Uploaded %s -> %s", local_path, rpath);
    return ok ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* del (delete)                                                          */
/* ------------------------------------------------------------------ */
int cmd_del(const AcConfig *cfg, const char *remote_path)
{
    char auth[192], url[AC_MAX_URL], rpath[AC_MAX_PATH];
    HttpResponse *resp;
    JSON_Value *root_val;
    JSON_Object *root;
    int ok;

    make_auth(auth, sizeof(auth), cfg);
    full_path(rpath, sizeof(rpath), cfg, remote_path);
    build_url(url, sizeof(url), cfg, "delete", rpath);

    resp = http_request("DELETE", url, auth, NULL, 0, cfg->verify_ssl);
    g_last_status = resp ? resp->status_code : 0;
    if (!resp || resp->status_code == 0) {
        ui_notify(UI_ERROR, "amicloud: network error");
        http_response_free(resp); return 1;
    }
    root_val = json_parse_string(resp->body ? resp->body : "{}");
    http_response_free(resp);
    root = root_val ? json_value_get_object(root_val) : NULL;
    ok = root && check_status(root, url);
    if (root_val) json_value_free(root_val);
    if (ok) ui_notify(UI_INFO, "Deleted %s", rpath);
    return ok ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* info                                                                  */
/* ------------------------------------------------------------------ */
int cmd_info_raw(const AcConfig *cfg, const char *remote_path, AcInfo *out)
{
    char auth[192], url[AC_MAX_URL], rpath[AC_MAX_PATH];
    HttpResponse *resp;
    JSON_Value *root_val;
    JSON_Object *root;

    memset(out, 0, sizeof(*out));

    make_auth(auth, sizeof(auth), cfg);
    full_path(rpath, sizeof(rpath), cfg, remote_path);
    build_url(url, sizeof(url), cfg, "info", rpath);

    resp = http_request("GET", url, auth, NULL, 0, cfg->verify_ssl);
    g_last_status = resp ? resp->status_code : 0;
    if (!resp || resp->status_code == 0 || !resp->body) {
        ui_notify(UI_ERROR, "amicloud: network error");
        http_response_free(resp); return 1;
    }
    root_val = json_parse_string(resp->body);
    http_response_free(resp);
    root = root_val ? json_value_get_object(root_val) : NULL;
    if (!root || !check_status(root, url)) { if (root_val) json_value_free(root_val); return 1; }

    strncpy(out->name, jstr(root, "name"), sizeof(out->name) - 1);
    strncpy(out->type, jstr(root, "type"), sizeof(out->type) - 1);
    out->size = jlong(root, "size");
    strncpy(out->mime, jstr(root, "mime"), sizeof(out->mime) - 1);
    strncpy(out->created, jstr(root, "created"), sizeof(out->created) - 1);
    strncpy(out->modified, jstr(root, "modified"), sizeof(out->modified) - 1);
    out->mtime = jlong(root, "mtime");
    strncpy(out->md5, jstr(root, "md5"), sizeof(out->md5) - 1);
    out->protection = jlong(root, "protection");
    strncpy(out->comment, jstr(root, "comment"), sizeof(out->comment) - 1);
    out->id         = jlong(root, "id");
    out->revision   = jlong(root, "revision");
    out->change_seq = jlong(root, "change_seq");

    json_value_free(root_val);
    return 0;
}

int cmd_exists(const AcConfig *cfg, const char *remote_path)
{
    char auth[192], url[AC_MAX_URL], rpath[AC_MAX_PATH];
    HttpResponse *resp;
    JSON_Value *root_val;
    int status_ok;

    make_auth(auth, sizeof(auth), cfg);
    full_path(rpath, sizeof(rpath), cfg, remote_path);
    build_url(url, sizeof(url), cfg, "info", rpath);

    resp = http_request("GET", url, auth, NULL, 0, cfg->verify_ssl);
    g_last_status = resp ? resp->status_code : 0;
    if (!resp || resp->status_code == 0 || !resp->body) {
        http_response_free(resp);
        return 0;
    }
    root_val = json_parse_string(resp->body);
    http_response_free(resp);
    if (!root_val) return 0;

    {
        const char *st = json_object_get_string(json_value_get_object(root_val), "status");
        status_ok = st && strcmp(st, "ok") == 0;
    }
    json_value_free(root_val);
    return status_ok;
}

int cmd_info(const AcConfig *cfg, const char *remote_path)
{
    AcInfo info;
    char pbuf[9];

    if (cmd_info_raw(cfg, remote_path, &info) != 0) return 1;

    ui_notify(UI_INFO, "Name     : %s", info.name);
    ui_notify(UI_INFO, "Type     : %s", info.type);
    ui_notify(UI_INFO, "Size     : %ld bytes", info.size);
    ui_notify(UI_INFO, "MIME     : %s", info.mime);
    ui_notify(UI_INFO, "Created  : %s", info.created);
    ui_notify(UI_INFO, "Modified : %s", info.modified);
    prot_str(info.protection, pbuf);
    ui_notify(UI_INFO, "Protect  : %s (%ld)", pbuf, info.protection);
    if (info.comment[0])
        ui_notify(UI_INFO, "Comment  : %s", info.comment);

    return 0;
}

/* ------------------------------------------------------------------ */
/* rename                                                                */
/* ------------------------------------------------------------------ */
int cmd_rename(const AcConfig *cfg, const char *from_path, const char *to_path)
{
    char auth[192], url[AC_MAX_URL], rfrom[AC_MAX_PATH], rto[AC_MAX_PATH];
    HttpResponse *resp;
    JSON_Value *root_val;
    JSON_Object *root;
    int ok;

    make_auth(auth, sizeof(auth), cfg);
    full_path(rfrom, sizeof(rfrom), cfg, from_path);
    full_path(rto,   sizeof(rto),   cfg, to_path);
    snprintf(url, sizeof(url), "%s/api.php?action=rename&path=%s&to=%s",
             cfg->server, rfrom, rto);

    resp = http_request("POST", url, auth, NULL, 0, cfg->verify_ssl);
    g_last_status = resp ? resp->status_code : 0;
    if (!resp || resp->status_code == 0) {
        ui_notify(UI_ERROR, "amicloud: network error");
        http_response_free(resp); return 1;
    }
    root_val = json_parse_string(resp->body ? resp->body : "{}");
    http_response_free(resp);
    root = root_val ? json_value_get_object(root_val) : NULL;
    ok = root && check_status(root, url);
    if (root_val) json_value_free(root_val);
    if (ok) ui_notify(UI_INFO, "Renamed %s -> %s", rfrom, rto);
    return ok ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* setcomment / setprotect                                              */
/* ------------------------------------------------------------------ */
int cmd_setcomment(const AcConfig *cfg, const char *remote_path, const char *comment)
{
    char auth[192], url[AC_MAX_URL], rpath[AC_MAX_PATH], enc[256];
    HttpResponse *resp;
    JSON_Value *root_val;
    JSON_Object *root;
    int ok;

    make_auth(auth, sizeof(auth), cfg);
    full_path(rpath, sizeof(rpath), cfg, remote_path);
    url_encode(enc, sizeof(enc), comment);
    snprintf(url, sizeof(url), "%s/api.php?action=setcomment&path=%s&comment=%s",
             cfg->server, rpath, enc);

    resp = http_request("POST", url, auth, NULL, 0, cfg->verify_ssl);
    g_last_status = resp ? resp->status_code : 0;
    if (!resp || resp->status_code == 0) {
        ui_notify(UI_ERROR, "amicloud: network error");
        http_response_free(resp); return 1;
    }
    root_val = json_parse_string(resp->body ? resp->body : "{}");
    http_response_free(resp);
    root = root_val ? json_value_get_object(root_val) : NULL;
    ok = root && check_status(root, url);
    if (root_val) json_value_free(root_val);
    return ok ? 0 : 1;
}

int cmd_setprotect(const AcConfig *cfg, const char *remote_path, long bits)
{
    char auth[192], url[AC_MAX_URL], rpath[AC_MAX_PATH];
    HttpResponse *resp;
    JSON_Value *root_val;
    JSON_Object *root;
    int ok;

    make_auth(auth, sizeof(auth), cfg);
    full_path(rpath, sizeof(rpath), cfg, remote_path);
    snprintf(url, sizeof(url), "%s/api.php?action=setprotect&path=%s&bits=%ld",
             cfg->server, rpath, bits);

    resp = http_request("POST", url, auth, NULL, 0, cfg->verify_ssl);
    g_last_status = resp ? resp->status_code : 0;
    if (!resp || resp->status_code == 0) {
        ui_notify(UI_ERROR, "amicloud: network error");
        http_response_free(resp); return 1;
    }
    root_val = json_parse_string(resp->body ? resp->body : "{}");
    http_response_free(resp);
    root = root_val ? json_value_get_object(root_val) : NULL;
    ok = root && check_status(root, url);
    if (root_val) json_value_free(root_val);
    return ok ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* mkdir                                                                 */
/* ------------------------------------------------------------------ */
int cmd_mkdir(const AcConfig *cfg, const char *remote_path)
{
    char auth[192], url[AC_MAX_URL], rpath[AC_MAX_PATH];
    HttpResponse *resp;
    JSON_Value *root_val;
    JSON_Object *root;
    int ok;

    make_auth(auth, sizeof(auth), cfg);
    full_path(rpath, sizeof(rpath), cfg, remote_path);
    build_url(url, sizeof(url), cfg, "mkdir", rpath);

    resp = http_request("POST", url, auth, NULL, 0, cfg->verify_ssl);
    g_last_status = resp ? resp->status_code : 0;
    if (!resp || resp->status_code == 0) {
        ui_notify(UI_ERROR, "amicloud: network error");
        http_response_free(resp); return 1;
    }
    root_val = json_parse_string(resp->body ? resp->body : "{}");
    http_response_free(resp);
    root = root_val ? json_value_get_object(root_val) : NULL;
    ok = root && check_status(root, url);
    if (root_val) json_value_free(root_val);
    if (ok) ui_notify(UI_INFO, "Created directory %s", rpath);
    return ok ? 0 : 1;
}

/* Silent variant: HTTP 409 (already exists) counts as success, no output
 * either way. Used while walking a local tree for `put -r`, where most
 * subdirectories already existing on a repeat run is the normal case,
 * not an error worth reporting. */
int cmd_mkdir_quiet(const AcConfig *cfg, const char *remote_path)
{
    char auth[192], url[AC_MAX_URL], rpath[AC_MAX_PATH];
    HttpResponse *resp;
    JSON_Value *root_val;
    JSON_Object *root;
    int result;

    make_auth(auth, sizeof(auth), cfg);
    full_path(rpath, sizeof(rpath), cfg, remote_path);
    build_url(url, sizeof(url), cfg, "mkdir", rpath);

    resp = http_request("POST", url, auth, NULL, 0, cfg->verify_ssl);
    g_last_status = resp ? resp->status_code : 0;
    if (!resp) return 1;
    if (resp->status_code == 409) { http_response_free(resp); return 0; }

    root_val = json_parse_string(resp->body ? resp->body : "{}");
    http_response_free(resp);
    root = root_val ? json_value_get_object(root_val) : NULL;
    result = (root && check_status(root, url)) ? 0 : 1;
    if (root_val) json_value_free(root_val);
    return result;
}

/* ------------------------------------------------------------------ */
/* setdate&mtime= (used by sync to align mtimes, plan §20.2)             */
/* ------------------------------------------------------------------ */
int cmd_setdate_mtime(const AcConfig *cfg, const char *remote_path, long mtime_epoch)
{
    char auth[192], url[AC_MAX_URL], rpath[AC_MAX_PATH];
    HttpResponse *resp;
    JSON_Value *root_val;
    JSON_Object *root;
    int ok;

    make_auth(auth, sizeof(auth), cfg);
    full_path(rpath, sizeof(rpath), cfg, remote_path);
    snprintf(url, sizeof(url), "%s/api.php?action=setdate&path=%s&mtime=%ld",
             cfg->server, rpath, mtime_epoch);

    resp = http_request("POST", url, auth, NULL, 0, cfg->verify_ssl);
    g_last_status = resp ? resp->status_code : 0;
    if (!resp || resp->status_code == 0) {
        ui_notify(UI_ERROR, "amicloud: network error");
        http_response_free(resp); return 1;
    }
    root_val = json_parse_string(resp->body ? resp->body : "{}");
    http_response_free(resp);
    root = root_val ? json_value_get_object(root_val) : NULL;
    ok = root && check_status(root, url);
    if (root_val) json_value_free(root_val);
    return ok ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* del -r (recursive delete)                                            */
/* ------------------------------------------------------------------ */

/* Client-side fallback (Variante A, see docs/interactive-shell-plan.html
 * §9.3): walk the tree ourselves and delete bottom-up using the plain
 * `delete` action. Used only when the server doesn't understand
 * recursive=1 (older deployment). */
static int del_recursive_fallback(const AcConfig *cfg, const char *remote_path)
{
    AcEntry *entries;
    int n, i, had_error = 0;

    if (!cmd_list_raw(cfg, remote_path, &entries, &n)) {
        /* Not a directory (or nothing there) -- treat as a plain delete
         * target; this also correctly surfaces file-not-found errors. */
        return cmd_del(cfg, remote_path);
    }

    for (i = 0; i < n; i++) {
        char child[AC_MAX_PATH];
        snprintf(child, sizeof(child), "%s/%s", remote_path, entries[i].name);
        if (entries[i].is_dir) {
            if (del_recursive_fallback(cfg, child) != 0) had_error = 1;
        } else if (cmd_del(cfg, child) != 0) {
            had_error = 1;
        }
    }
    cmd_entries_free(entries);

    if (had_error) return 1;
    return cmd_del(cfg, remote_path); /* now empty */
}

int cmd_del_recursive(const AcConfig *cfg, const char *remote_path)
{
    char auth[192], url[AC_MAX_URL], rpath[AC_MAX_PATH];
    HttpResponse *resp;
    JSON_Value *root_val;
    JSON_Object *root;
    int ok;

    make_auth(auth, sizeof(auth), cfg);
    full_path(rpath, sizeof(rpath), cfg, remote_path);
    snprintf(url, sizeof(url), "%s/api.php?action=delete&path=%s&recursive=1",
             cfg->server, rpath);

    resp = http_request("DELETE", url, auth, NULL, 0, cfg->verify_ssl);
    g_last_status = resp ? resp->status_code : 0;
    if (!resp || resp->status_code == 0) {
        ui_notify(UI_ERROR, "amicloud: network error");
        http_response_free(resp);
        return 1;
    }
    if (resp->status_code == 409) {
        /* Server doesn't understand recursive=1 (pre-upgrade deployment)
         * -> fall back to a client-side walk. */
        http_response_free(resp);
        return del_recursive_fallback(cfg, remote_path);
    }

    root_val = json_parse_string(resp->body ? resp->body : "{}");
    http_response_free(resp);
    root = root_val ? json_value_get_object(root_val) : NULL;
    ok = root && check_status(root, url);
    if (root_val) json_value_free(root_val);
    if (ok) ui_notify(UI_INFO, "Deleted %s (recursive)", rpath);
    return ok ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* get_buf / put_buf (in-memory transfers for the CLOUD: handler's       */
/* worker process, see docs/handler-plan.md Stufe 2)                     */
/* ------------------------------------------------------------------ */
int cmd_get_buf(const AcConfig *cfg, const char *remote_path, char **out, long *out_len, int *status_out)
{
    char auth[192], url[AC_MAX_URL], rpath[AC_MAX_PATH];
    HttpResponse *resp;
    char hex[33];

    *out = NULL; *out_len = 0;
    if (status_out) *status_out = 0;

    make_auth(auth, sizeof(auth), cfg);
    full_path(rpath, sizeof(rpath), cfg, remote_path);
    build_url(url, sizeof(url), cfg, "download", rpath);

    resp = http_request("GET", url, auth, NULL, 0, cfg->verify_ssl);
    g_last_status = resp ? resp->status_code : 0;
    if (!resp || resp->status_code == 0) {
        ui_notify(UI_ERROR, "amicloud: network error");
        http_response_free(resp);
        return 1;
    }
    if (status_out) *status_out = resp->status_code;
    if (resp->status_code != 200) {
        ui_notify(UI_ERROR, "amicloud: server returned HTTP %d", resp->status_code);
        http_response_free(resp);
        return 1;
    }

#ifdef PLATFORM_AMIGA
    ac_log_printf("get_buf: status 200 OK, body_len=%ld, md5_header='%s'\n",
                  resp->body_len, resp->md5_header);
#endif

    if (resp->md5_header[0]) {
#ifdef PLATFORM_AMIGA
        ac_log_printf("get_buf: computing MD5 over %ld bytes...\n", resp->body_len);
#endif
        md5_buffer_hex(resp->body ? resp->body : "", (size_t)resp->body_len, hex);
#ifdef PLATFORM_AMIGA
        ac_log_printf("get_buf: MD5 done, got '%s'\n", hex);
#endif
        if (strcmp(resp->md5_header, hex) != 0) {
            ui_notify(UI_ERROR, "amicloud: get_buf: MD5 mismatch for %s", remote_path);
            http_response_free(resp);
            return 1;
        }
    }

    {
        long len = resp->body_len > 0 ? resp->body_len : 0;
        char *buf;
#ifdef PLATFORM_AMIGA
        ac_log_printf("get_buf: AllocVec(%ld)...\n", (long)(len > 0 ? len : 1));
        buf = (char *)AllocVec((ULONG)(len > 0 ? len : 1), MEMF_PUBLIC);
        ac_log_printf("get_buf: AllocVec -> %p\n", (void *)buf);
#else
        buf = (char *)malloc((size_t)(len > 0 ? len : 1));
#endif
        if (!buf) {
            ui_notify(UI_ERROR, "amicloud: get_buf: out of memory");
            http_response_free(resp);
            return 1;
        }
#ifdef PLATFORM_AMIGA
        ac_log_printf("get_buf: memcpy(%p, %p, %ld)...\n", (void *)buf, (void *)resp->body, len);
#endif
        if (len > 0) memcpy(buf, resp->body, (size_t)len);
#ifdef PLATFORM_AMIGA
        ac_log_printf("get_buf: memcpy done\n");
#endif
        *out = buf;
        *out_len = len;
    }

#ifdef PLATFORM_AMIGA
    ac_log_printf("get_buf: freeing HttpResponse...\n");
#endif
    http_response_free(resp);
#ifdef PLATFORM_AMIGA
    ac_log_printf("get_buf: returning success\n");
#endif
    return 0;
}

int cmd_put_buf(const AcConfig *cfg, const char *remote_path, const char *buf, long len, int *status_out)
{
    char auth[192], url[AC_MAX_URL], rpath[AC_MAX_PATH];
    HttpResponse *resp;
    JSON_Value *root_val;
    JSON_Object *root;
    int ok;
    char md5hex[33];

    if (status_out) *status_out = 0;
    md5_buffer_hex(buf, (size_t)len, md5hex);

    make_auth(auth, sizeof(auth), cfg);
    full_path(rpath, sizeof(rpath), cfg, remote_path);
    snprintf(url, sizeof(url), "%s/api.php?action=upload&path=%s&md5=%s",
             cfg->server, rpath, md5hex);

    resp = http_request("POST", url, auth, buf, len, cfg->verify_ssl);
    g_last_status = resp ? resp->status_code : 0;
    if (!resp || resp->status_code == 0) {
        ui_notify(UI_ERROR, "amicloud: network error");
        http_response_free(resp);
        return 1;
    }
    if (status_out) *status_out = resp->status_code;

    root_val = json_parse_string(resp->body ? resp->body : "{}");
    http_response_free(resp);
    root = root_val ? json_value_get_object(root_val) : NULL;
    ok = root && check_status(root, url);
    if (root_val) json_value_free(root_val);
    return ok ? 0 : 1;
}
