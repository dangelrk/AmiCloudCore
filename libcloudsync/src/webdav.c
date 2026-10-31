/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * webdav.c — see webdav.h. Standard WebDAV transport for cloudsync_run_ex
 * (PROPFIND/GET/PUT/MKCOL/DELETE, RFC 4918), talking to a plain OwnCloud/
 * Nextcloud server. No AmiCloud/api.php code is reachable from here, and
 * none of this file is reachable from cmd.c/api.php's own callers either
 * -- the two stay completely separate, per this tool's own charter (see
 * webdav.h).
 *
 * The multistatus scanner below is deliberately NOT a general XML parser
 * (see its own comment) -- a real DOM would cost more RAM than this
 * whole tool's 4 MB Amiga target can spare for what is, in practice,
 * always ownCloud/Nextcloud's own well-known PROPFIND response shape.
 */
#include "webdav.h"
#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* URL / auth / percent-encoding helpers                                 */
/* ------------------------------------------------------------------ */

static void make_auth(char *out, size_t outsz, const AcConfig *cfg)
{
    snprintf(out, outsz, "%s:%s", cfg->username, cfg->password);
    /* Every request builds its credentials here first -- the one spot
     * that sees the config right before talking to the server. */
    http_set_timeout(cfg->timeout);
}

/* "Nur Hauptserver" (repo plan): cfg->server is now allowed to be just
 * the bare host the AmiOwnCloudSyncPrefs editor asks for, e.g.
 * "https://cloud.example.com" -- this derives the actual per-user
 * WebDAV collection root Nextcloud/ownCloud expects,
 * "<host>/remote.php/dav/files/<username>". Full collection URLs
 * already containing "/remote.php/" (old AmiOwnCloudSync.prefs files, and
 * amiowncloud-sync's own config -- see amiowncloud-sync.cfg.example) are recognised
 * and passed through unchanged, so neither format ever breaks.
 *
 * Exported (webdav_derive_base_url(), see webdav.h) rather than kept
 * static: webdavsynctest's build_test_url() deliberately bypasses this
 * whole transport to act as an independent third party (see its own
 * comment), but still needs to hit the exact same collection URL a real
 * transport would -- duplicating the URL-JOIN logic there is fine (that
 * really is a separate, independent implementation, on purpose), but
 * duplicating THIS derivation policy would be a second place to update
 * every time it changes, and a silent way for the two to drift apart. */
void webdav_derive_base_url(char *out, size_t outsz, const AcConfig *cfg)
{
    char host[AC_MAX_URL];
    size_t len;

    if (strstr(cfg->server, "/remote.php/") != NULL) {
        strncpy(out, cfg->server, outsz - 1);
        out[outsz - 1] = '\0';
        return;
    }

    strncpy(host, cfg->server, sizeof(host) - 1);
    host[sizeof(host) - 1] = '\0';
    len = strlen(host);
    if (len > 0 && host[len - 1] == '/') host[len - 1] = '\0';

    snprintf(out, outsz, "%s/remote.php/dav/files/%s", host, cfg->username);
}

/* Percent-encodes a path, leaving '/' alone (unlike cmd.c's url_encode,
 * which is only ever used on a single non-path value). */
static void url_encode_path(char *out, size_t outsz, const char *in)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in && o + 4 < outsz; in++) {
        unsigned char c = (unsigned char)*in;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0xF];
        }
    }
    out[o] = '\0';
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *out, size_t outsz, const char *in)
{
    size_t o = 0;
    while (*in && o + 1 < outsz) {
        if (*in == '%' && in[1] && in[2]) {
            int hi = hexval(in[1]), lo = hexval(in[2]);
            if (hi >= 0 && lo >= 0) {
                out[o++] = (char)((hi << 4) | lo);
                in += 3;
                continue;
            }
        }
        out[o++] = *in++;
    }
    out[o] = '\0';
}

/* `base` is the WebDAV collection's own base URL (see webdav.h) --
 * webdav_derive_base_url()'s result, not necessarily cfg->server
 * verbatim -- remote_path is joined onto it with exactly one '/'
 * between them. */
static void build_url(char *out, size_t outsz, const char *base, const char *remote_path)
{
    char enc[AC_MAX_PATH];
    size_t srvlen = strlen(base);
    int server_slash = srvlen > 0 && base[srvlen - 1] == '/';
    int path_slash   = remote_path && remote_path[0] == '/';

    url_encode_path(enc, sizeof(enc), remote_path ? remote_path : "");

    if (server_slash && path_slash)
        snprintf(out, outsz, "%s%s", base, enc + 1);
    else if (!server_slash && !path_slash && enc[0])
        snprintf(out, outsz, "%s/%s", base, enc);
    else
        snprintf(out, outsz, "%s%s", base, enc);
}

/* SyncTransport carries only `const AcConfig *cfg` (transport.h) --
 * cfg is the caller's own struct and must not be mutated just to stash
 * a derived base URL in it. WebdavCtx wraps the public SyncTransport
 * with that one extra private field instead; `pub` is the struct's
 * first member, so a `SyncTransport *` handed back to a caller and a
 * `WebdavCtx *` recovered from it via cast share the same address --
 * the free(t) in webdav_transport_close() below still frees the whole
 * block correctly. */
typedef struct {
    SyncTransport pub;
    char base[AC_MAX_URL];
} WebdavCtx;

static const char *tbase(SyncTransport *t) { return ((WebdavCtx *)t)->base; }

static int is_2xx(int status) { return status >= 200 && status < 300; }

/* ------------------------------------------------------------------ */
/* RFC 1123 date -> UTC epoch (WebDAV's getlastmodified format)          */
/* ------------------------------------------------------------------ */

static const char *rfc1123_months[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

/* Howard Hinnant's days-from-civil, the same algorithm cloudtime.h's
 * Amiga-side ac_civil_from_days() inverts -- deliberately not
 * timegm()/mktime() (local-time/DST landmines, and not reliably present
 * on VBCC/AmigaOS anyway); this needs no timezone at all since HTTP dates
 * are always GMT. */
static long days_from_civil(long y, int m, int d)
{
    long era;
    unsigned yoe, doy, doe;
    y -= (m <= 2);
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (unsigned)(y - era * 400);
    doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097L + (long)doe - 719468L;
}

/* Returns 0 on any parse failure -- caller just leaves mtime at 0, and
 * the merge logic's own mtime-tolerant alignment pass fixes it up on the
 * following sync (see sync.c's mtimes_equal()). */
static long parse_rfc1123(const char *s)
{
    const char *p;
    int day, year, hh, mm, ss, mon = -1;
    char monstr[4];
    int i;

    p = strchr(s, ',');
    if (!p) return 0;
    p++;
    while (*p == ' ') p++;

    monstr[0] = '\0';
    if (sscanf(p, "%d %3s %d %d:%d:%d", &day, monstr, &year, &hh, &mm, &ss) != 6)
        return 0;
    for (i = 0; i < 12; i++)
        if (strncmp(monstr, rfc1123_months[i], 3) == 0) { mon = i + 1; break; }
    if (mon < 0) return 0;

    return days_from_civil((long)year, mon, day) * 86400L + hh * 3600L + mm * 60L + ss;
}

/* ------------------------------------------------------------------ */
/* Small multistatus scanner (see this file's top comment) -- targets    */
/* exactly what ownCloud/Nextcloud emit: lowercase local element names,   */
/* one arbitrary namespace prefix per tag. Case-sensitive lowercase       */
/* match -- fine for real servers, not a general parser.                 */
/* ------------------------------------------------------------------ */

/* Finds the next "<[prefix:]name" opening tag at/after `from`. Returns a
 * pointer just past the tag's '>' (self-closing or not), or NULL. */
static const char *find_tag(const char *from, const char *name)
{
    size_t namelen = strlen(name);
    const char *p = from;
    for (;;) {
        const char *tagstart, *q, *colon, *localname, *gt;
        p = strchr(p, '<');
        if (!p) return NULL;
        p++;
        if (*p == '/' || *p == '!' || *p == '?') { p++; continue; }
        tagstart = p; colon = NULL; q = p;
        while (*q && *q != '>' && *q != ' ' && *q != '\t' && *q != '\r' && *q != '\n' && *q != '/') {
            if (*q == ':') colon = q;
            q++;
        }
        localname = colon ? colon + 1 : tagstart;
        if ((size_t)(q - localname) == namelen && strncmp(localname, name, namelen) == 0) {
            gt = strchr(q, '>');
            return gt ? gt + 1 : NULL;
        }
        p = q;
    }
}

/* Finds the next "</[prefix:]name>" closing tag at/after `from`. Returns
 * a pointer to its leading '<', or NULL. Response elements are never
 * nested within each other, so no depth-tracking is needed. */
static const char *find_close(const char *from, const char *name)
{
    size_t namelen = strlen(name);
    const char *p = from;
    for (;;) {
        const char *q, *colon, *start;
        p = strstr(p, "</");
        if (!p) return NULL;
        q = p + 2; colon = NULL; start = q;
        while (*q && *q != '>') { if (*q == ':') colon = q; q++; }
        {
            const char *localname = colon ? colon + 1 : start;
            if ((size_t)(q - localname) == namelen && strncmp(localname, name, namelen) == 0)
                return p;
        }
        p = q;
    }
}

/* Text content of <name>...</name> if found strictly within [from,end)
 * (a self-closing "<name/>" yields an empty string, same as "not
 * present" -- neither case is ever ambiguous for the props this file
 * reads). *out is always left a valid, terminated string. */
static void tag_text_bounded(const char *from, const char *end, const char *name,
                             char *out, size_t outsz)
{
    const char *body = find_tag(from, name);
    out[0] = '\0';
    if (!body || body > end) return;
    if (body[-2] == '/') return; /* self-closing, no text */
    {
        const char *close = strstr(body, "</");
        size_t len;
        if (!close || close > end) return;
        len = (size_t)(close - body);
        if (len >= outsz) len = outsz - 1;
        memcpy(out, body, len);
        out[len] = '\0';
    }
}

static int has_tag_bounded(const char *from, const char *end, const char *name)
{
    const char *pos = find_tag(from, name);
    return (pos && pos <= end) ? 1 : 0;
}

/* Parses a Depth:1 PROPFIND multistatus response into a flat malloc'd
 * array of SyncRemoteEntry, exactly like cmd_list_raw()'s AcEntry array
 * -- one entry per DIRECT CHILD. The very first <response> is always
 * the collection itself (universal Depth:1 behaviour of every WebDAV
 * server this backend targets) and is skipped unconditionally, rather
 * than matched by URL comparison -- simpler and just as reliable here. */
static int parse_multistatus(const char *xml, SyncRemoteEntry **out, int *out_n)
{
    const char *p = xml;
    SyncRemoteEntry *arr = NULL;
    int cap = 0, n = 0, index = 0;

    for (;;) {
        const char *resp_open, *resp_close;
        char href_raw[1024], href[1024], token[64], lastmod[64];
        const char *slash, *name_start;

        resp_open = find_tag(p, "response");
        if (!resp_open) break;
        resp_close = find_close(resp_open, "response");
        if (!resp_close) break;

        index++;
        if (index == 1) { p = resp_close; continue; }

        href_raw[0] = '\0';
        tag_text_bounded(resp_open, resp_close, "href", href_raw, sizeof(href_raw));
        url_decode(href, sizeof(href), href_raw);

        {
            size_t hl = strlen(href);
            if (hl > 0 && href[hl - 1] == '/') href[hl - 1] = '\0';
        }
        slash = strrchr(href, '/');
        name_start = slash ? slash + 1 : href;
        if (!name_start[0]) { p = resp_close; continue; } /* malformed/empty, skip */

        if (n == cap) {
            int newcap = cap ? cap * 2 : 16;
            SyncRemoteEntry *tmp = (SyncRemoteEntry *)realloc(arr, sizeof(SyncRemoteEntry) * (size_t)newcap);
            if (!tmp) { free(arr); return 0; }
            arr = tmp; cap = newcap;
        }
        memset(&arr[n], 0, sizeof(arr[n]));
        strncpy(arr[n].name, name_start, sizeof(arr[n].name) - 1);
        arr[n].is_dir = has_tag_bounded(resp_open, resp_close, "collection");

        token[0] = '\0';
        tag_text_bounded(resp_open, resp_close, "getetag", token, sizeof(token));
        {
            size_t tl = strlen(token);
            if (tl >= 2 && token[0] == '"' && token[tl - 1] == '"') {
                memmove(token, token + 1, tl - 2);
                token[tl - 2] = '\0';
            } else if (tl >= 12 && strncmp(token, "&quot;", 6) == 0 &&
                       strcmp(token + tl - 6, "&quot;") == 0) {   /* XML-escaped */
                memmove(token, token + 6, tl - 12);
                token[tl - 12] = '\0';
            }
        }
        strncpy(arr[n].token, token, sizeof(arr[n].token) - 1);

        lastmod[0] = '\0';
        tag_text_bounded(resp_open, resp_close, "getlastmodified", lastmod, sizeof(lastmod));
        arr[n].mtime = lastmod[0] ? parse_rfc1123(lastmod) : 0;

        if (!arr[n].is_dir) {
            char clen[32];
            clen[0] = '\0';
            tag_text_bounded(resp_open, resp_close, "getcontentlength", clen, sizeof(clen));
            arr[n].size = clen[0] ? atol(clen) : 0;
        }

        n++;
        p = resp_close;
    }

    *out = arr;
    *out_n = n;
    return 1;
}

/* ------------------------------------------------------------------ */
/* SyncTransport vtable                                                  */
/* ------------------------------------------------------------------ */

static int webdav_list(SyncTransport *t, const char *remote_path,
                       SyncRemoteEntry **out, int *out_n)
{
    char url[AC_MAX_URL], auth[192];
    HttpResponse *resp;
    int ok;
    static const char body[] =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<D:propfind xmlns:D=\"DAV:\">"
        "<D:prop>"
        "<D:resourcetype/><D:getetag/><D:getlastmodified/><D:getcontentlength/>"
        "</D:prop>"
        "</D:propfind>";

    *out = NULL; *out_n = 0;

    build_url(url, sizeof(url), tbase(t), remote_path);
    make_auth(auth, sizeof(auth), t->cfg);

    resp = http_request_hdr("PROPFIND", url, auth,
                            "Depth: 1\r\nContent-Type: text/xml; charset=utf-8\r\n",
                            body, (long)strlen(body), t->cfg->verify_ssl);
    if (!resp || resp->status_code == 0 || resp->status_code != 207 || !resp->body) {
        http_response_free(resp);
        return 0;
    }

    ok = parse_multistatus(resp->body, out, out_n);
    http_response_free(resp);
    return ok;
}

static int webdav_get(SyncTransport *t, const char *remote_path, const char *local_path,
                      const char *display_name)
{
    char url[AC_MAX_URL], auth[192];
    FILE *fp;
    HttpResponse *resp;

    build_url(url, sizeof(url), tbase(t), remote_path);
    make_auth(auth, sizeof(auth), t->cfg);

    fp = fopen(local_path, "wb");
    if (!fp) return 1;

    resp = http_download(url, auth, fp, t->cfg->verify_ssl, NULL,
                         display_name ? display_name : local_path);
    fclose(fp);

    if (!resp || resp->status_code == 0) {
        remove(local_path); /* cancelled or broken off -- never keep a partial file */
        http_response_free(resp);
        return 1;
    }
    if (!is_2xx(resp->status_code)) {
        remove(local_path);
        http_response_free(resp);
        return 1;
    }
    http_response_free(resp);
    return 0;
}

/* PUT + X-OC-Mtime in one request -- see transport.h's put() contract. */
static int webdav_put(SyncTransport *t, const char *local_path,
                      const char *remote_path, long mtime,
                      const SyncPutCond *cond, SyncPutResult *res)
{
    char url[AC_MAX_URL], auth[192], hdr[256];
    FILE *fp;
    long fsize;
    HttpResponse *resp;
    int ok;

    fp = fopen(local_path, "rb");
    if (!fp) return 1;
    fseek(fp, 0, SEEK_END);
    fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fsize < 0) { fclose(fp); return 1; }

    build_url(url, sizeof(url), tbase(t), remote_path);
    make_auth(auth, sizeof(auth), t->cfg);
    /* S8: If-Match with the ETag seen at the last sync, or If-None-Match
     * for a file that must not exist yet (RFC 7232, 412 otherwise). */
    if (cond && cond->if_token && cond->if_token[0])
        /* the listing stores the ETag without its quotes (see above),
         * If-Match needs the quoted entity tag (RFC 7232) */
        snprintf(hdr, sizeof(hdr), "X-OC-Mtime: %ld\r\nIf-Match: \"%s\"\r\n", mtime, cond->if_token);
    else if (cond && cond->create_only)
        snprintf(hdr, sizeof(hdr), "X-OC-Mtime: %ld\r\nIf-None-Match: *\r\n", mtime);
    else
        snprintf(hdr, sizeof(hdr), "X-OC-Mtime: %ld\r\n", mtime);
    if (res) memset(res, 0, sizeof(*res));   /* the next PROPFIND brings the ETag */

    resp = http_upload_hdr("PUT", url, auth, hdr, fp, fsize, t->cfg->verify_ssl, local_path);
    fclose(fp);

    if (!resp || resp->status_code == 0) {
        http_response_free(resp);
        return SYNC_PUT_ERROR;
    }
    if (resp->status_code == 412) {
        http_response_free(resp);
        return SYNC_PUT_CONFLICT;
    }
    ok = is_2xx(resp->status_code);
    http_response_free(resp);
    return ok ? SYNC_PUT_OK : SYNC_PUT_ERROR;
}

static int webdav_del(SyncTransport *t, const char *remote_path)
{
    char url[AC_MAX_URL], auth[192];
    HttpResponse *resp;
    int ok;

    build_url(url, sizeof(url), tbase(t), remote_path);
    make_auth(auth, sizeof(auth), t->cfg);

    resp = http_request("DELETE", url, auth, NULL, 0, t->cfg->verify_ssl);
    if (!resp || resp->status_code == 0) { http_response_free(resp); return 1; }
    ok = is_2xx(resp->status_code) || resp->status_code == 404; /* already gone */
    http_response_free(resp);
    return ok ? 0 : 1;
}

static int webdav_mkdir_quiet(SyncTransport *t, const char *remote_path)
{
    char url[AC_MAX_URL], auth[192];
    HttpResponse *resp;
    int ok;

    build_url(url, sizeof(url), tbase(t), remote_path);
    make_auth(auth, sizeof(auth), t->cfg);

    resp = http_request("MKCOL", url, auth, NULL, 0, t->cfg->verify_ssl);
    if (!resp || resp->status_code == 0) { http_response_free(resp); return 1; }
    /* 405 Method Not Allowed = collection already exists (RFC 4918) --
     * mirrors cmd_mkdir_quiet()'s 409 handling for the AmiCloud side. */
    ok = is_2xx(resp->status_code) || resp->status_code == 405;
    http_response_free(resp);
    return ok ? 0 : 1;
}

/* Best-effort only, exactly like the pre-refactor cmd_setdate_mtime()
 * call it replaces (whose result was likewise never checked, see
 * sync.c's SA_ALIGN_REMOTE case): PROPPATCH of {DAV:}lastmodified, a
 * de-facto Nextcloud/ownCloud extension. Silently doing nothing on a
 * plain/vanilla WebDAV server is fine -- next sync's mtime-tolerant
 * alignment pass (mtimes_equal(), sync.c) just tries again. */
static int webdav_setdate(SyncTransport *t, const char *remote_path, long mtime)
{
    char url[AC_MAX_URL], auth[192], body[256];
    HttpResponse *resp;
    int ok;

    build_url(url, sizeof(url), tbase(t), remote_path);
    make_auth(auth, sizeof(auth), t->cfg);
    snprintf(body, sizeof(body),
            "<?xml version=\"1.0\"?><D:propertyupdate xmlns:D=\"DAV:\">"
            "<D:set><D:prop><D:lastmodified>%ld</D:lastmodified></D:prop></D:set>"
            "</D:propertyupdate>", mtime);

    resp = http_request_hdr("PROPPATCH", url, auth,
                            "Content-Type: text/xml; charset=utf-8\r\n",
                            body, (long)strlen(body), t->cfg->verify_ssl);
    ok = resp && (resp->status_code == 207 || is_2xx(resp->status_code));
    http_response_free(resp);
    return ok ? 0 : 1;
}

/* WebDAV has no global change counter -- always force a full recursive
 * list() walk, same as an AmiCloud server whose cmd_status() ever
 * answers with global_revision 0 already forces today. */
static int webdav_global_rev(SyncTransport *t, long *global_out)
{
    if (t) *global_out = 0;
    return 0;
}

SyncTransport *webdav_transport_open(const AcConfig *cfg)
{
    WebdavCtx *ctx = (WebdavCtx *)calloc(1, sizeof(WebdavCtx));
    SyncTransport *t;
    if (!ctx) return NULL;

    webdav_derive_base_url(ctx->base, sizeof(ctx->base), cfg);

    t = &ctx->pub;
    t->cfg         = cfg;
    t->list        = webdav_list;
    t->get         = webdav_get;
    t->put         = webdav_put;
    t->del         = webdav_del;
    t->mkdir_quiet = webdav_mkdir_quiet;
    t->setdate     = webdav_setdate;
    t->global_rev  = webdav_global_rev;
    t->close       = NULL;
    return t;
}

void webdav_transport_close(SyncTransport *t)
{
    if (!t) return;
    if (t->close) t->close(t);
    free(t);
}
