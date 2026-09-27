/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * webdavcmd.c -- see webdavcmd.h. WebDAV (RFC 4918) command layer for the
 * OwnCloud: DOS handler's worker process, parallel to cmd.c's AmiCloud/
 * api.php layer. The URL/auth helpers, the RFC1123 date parser and the
 * multistatus scanner below are adapted from libcloudsync/src/
 * webdav.c (deliberately duplicated rather than shared -- see webdavcmd.h's
 * file header for why).
 */
#include "webdavcmd.h"
#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef PLATFORM_AMIGA
#include <exec/memory.h>
#include <proto/exec.h>
#endif

/* ------------------------------------------------------------------ */
/* Status tracking                                                      */
/* ------------------------------------------------------------------ */

static int g_last_status = 0;

int webdavcmd_last_status(void)
{
    return g_last_status;
}

/* ------------------------------------------------------------------ */
/* URL / auth / percent-encoding helpers                                */
/* ------------------------------------------------------------------ */

static void make_auth(char *out, size_t outsz, const AcConfig *cfg)
{
    snprintf(out, outsz, "%s:%s", cfg->username, cfg->password);
}

/* Same "bare host or full /remote.php/... collection URL" derivation as
 * libcloudsync/webdav.c's webdav_derive_base_url() -- see webdavcmd.h for
 * why this is a separate copy rather than a shared call. */
static void derive_base_url(char *out, size_t outsz, const AcConfig *cfg)
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

/* cfg->basepath prefix, same convention as cmd.c's full_path(). */
static void full_path(char *out, size_t sz, const AcConfig *cfg, const char *path)
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

/* Percent-encodes a path, leaving '/' alone (webdav.c's url_encode_path). */
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

/* Combines derive_base_url()+full_path() into the final request URL. */
static void build_url(char *out, size_t outsz, const AcConfig *cfg, const char *remote_path)
{
    char base[AC_MAX_URL];
    char rpath[AC_MAX_PATH];
    char enc[AC_MAX_PATH];
    size_t srvlen;
    int server_slash, path_slash;

    derive_base_url(base, sizeof(base), cfg);
    full_path(rpath, sizeof(rpath), cfg, remote_path);
    url_encode_path(enc, sizeof(enc), rpath);

    srvlen = strlen(base);
    server_slash = srvlen > 0 && base[srvlen - 1] == '/';
    path_slash   = enc[0] == '/';

    if (server_slash && path_slash)
        snprintf(out, outsz, "%s%s", base, enc + 1);
    else if (!server_slash && !path_slash && enc[0])
        snprintf(out, outsz, "%s/%s", base, enc);
    else
        snprintf(out, outsz, "%s%s", base, enc);
}

static int is_2xx(int status) { return status >= 200 && status < 300; }

/* ------------------------------------------------------------------ */
/* RFC 1123 date -> UTC epoch (WebDAV's getlastmodified format)          */
/* ------------------------------------------------------------------ */

static const char *rfc1123_months[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

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
/* Small multistatus scanner (see libcloudsync/webdav.c's identical        */
/* comment) -- targets ownCloud/Nextcloud's PROPFIND response shape only,  */
/* not a general XML parser.                                               */
/* ------------------------------------------------------------------ */

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

/* Depth:1 multistatus -> AcEntry[] (one entry per direct child); the very
 * first <response> is always the collection itself, always skipped --
 * same reasoning as libcloudsync/webdav.c's parse_multistatus(). No
 * WebDAV equivalent for AcEntry.protection/comment/md5/modified/id/
 * revision/change_seq -- left at 0/"" (see webdavcmd.h). */
static int parse_multistatus_list(const char *xml, AcEntry **out, int *out_n)
{
    const char *p = xml;
    AcEntry *arr = NULL;
    int cap = 0, n = 0, index = 0;

    for (;;) {
        const char *resp_open, *resp_close;
        char href_raw[1024], href[1024], lastmod[64];
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
        if (!name_start[0]) { p = resp_close; continue; }

        if (n == cap) {
            int newcap = cap ? cap * 2 : 16;
            AcEntry *tmp = (AcEntry *)realloc(arr, sizeof(AcEntry) * (size_t)newcap);
            if (!tmp) { free(arr); return 0; }
            arr = tmp; cap = newcap;
        }
        memset(&arr[n], 0, sizeof(arr[n]));
        strncpy(arr[n].name, name_start, sizeof(arr[n].name) - 1);
        arr[n].is_dir = has_tag_bounded(resp_open, resp_close, "collection");

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

/* Depth:0 multistatus -> AcInfo, exactly one <response> for the resource
 * itself (not skipped, unlike the Depth:1 case above). */
static int parse_single_response(const char *xml, const char *remote_path, AcInfo *out)
{
    const char *resp_open, *resp_close;
    char lastmod[64];
    const char *base;

    resp_open = find_tag(xml, "response");
    if (!resp_open) return 0;
    resp_close = find_close(resp_open, "response");
    if (!resp_close) return 0;

    base = strrchr(remote_path, '/');
    base = (base && base[1]) ? base + 1 : remote_path;
    strncpy(out->name, base, sizeof(out->name) - 1);

    if (has_tag_bounded(resp_open, resp_close, "collection")) {
        strncpy(out->type, "dir", sizeof(out->type) - 1);
        out->size = 0;
    } else {
        char clen[32];
        strncpy(out->type, "file", sizeof(out->type) - 1);
        clen[0] = '\0';
        tag_text_bounded(resp_open, resp_close, "getcontentlength", clen, sizeof(clen));
        out->size = clen[0] ? atol(clen) : 0;
    }

    lastmod[0] = '\0';
    tag_text_bounded(resp_open, resp_close, "getlastmodified", lastmod, sizeof(lastmod));
    out->mtime = lastmod[0] ? parse_rfc1123(lastmod) : 0;

    return 1;
}

/* ------------------------------------------------------------------ */
/* Public API (webdavcmd.h)                                             */
/* ------------------------------------------------------------------ */

int webdavcmd_list_raw(const AcConfig *cfg, const char *remote_path, AcEntry **out, int *out_n)
{
    char url[AC_MAX_URL], auth[192];
    HttpResponse *resp;
    int ok;
    static const char body[] =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<D:propfind xmlns:D=\"DAV:\">"
        "<D:prop>"
        "<D:resourcetype/><D:getlastmodified/><D:getcontentlength/>"
        "</D:prop>"
        "</D:propfind>";

    *out = NULL; *out_n = 0;

    build_url(url, sizeof(url), cfg, remote_path);
    make_auth(auth, sizeof(auth), cfg);

    resp = http_request_hdr("PROPFIND", url, auth,
                            "Depth: 1\r\nContent-Type: text/xml; charset=utf-8\r\n",
                            body, (long)strlen(body), cfg->verify_ssl);
    g_last_status = resp ? resp->status_code : 0;
    if (!resp || resp->status_code == 0 || resp->status_code != 207 || !resp->body) {
        http_response_free(resp);
        return 0;
    }

    ok = parse_multistatus_list(resp->body, out, out_n);
    http_response_free(resp);
    return ok;
}

void webdavcmd_entries_free(AcEntry *entries)
{
    free(entries);
}

int webdavcmd_get_buf(const AcConfig *cfg, const char *remote_path, char **out, long *out_len, int *status_out)
{
    char url[AC_MAX_URL], auth[192];
    HttpResponse *resp;

    *out = NULL; *out_len = 0;
    if (status_out) *status_out = 0;

    build_url(url, sizeof(url), cfg, remote_path);
    make_auth(auth, sizeof(auth), cfg);

    resp = http_request("GET", url, auth, NULL, 0, cfg->verify_ssl);
    g_last_status = resp ? resp->status_code : 0;
    if (!resp || resp->status_code == 0) {
        http_response_free(resp);
        return 1;
    }
    if (status_out) *status_out = resp->status_code;
    if (resp->status_code != 200) {
        http_response_free(resp);
        return 1;
    }

    {
        long len = resp->body_len > 0 ? resp->body_len : 0;
        char *buf;
#ifdef PLATFORM_AMIGA
        buf = (char *)AllocVec((ULONG)(len > 0 ? len : 1), MEMF_PUBLIC);
#else
        buf = (char *)malloc((size_t)(len > 0 ? len : 1));
#endif
        if (!buf) {
            http_response_free(resp);
            return 1;
        }
        if (len > 0) memcpy(buf, resp->body, (size_t)len);
        *out = buf;
        *out_len = len;
    }

    http_response_free(resp);
    return 0;
}

int webdavcmd_put_buf(const AcConfig *cfg, const char *remote_path, const char *buf, long len, int *status_out)
{
    char url[AC_MAX_URL], auth[192];
    HttpResponse *resp;
    int ok;

    if (status_out) *status_out = 0;

    build_url(url, sizeof(url), cfg, remote_path);
    make_auth(auth, sizeof(auth), cfg);

    resp = http_request_hdr("PUT", url, auth, (const char *)0, buf, len, cfg->verify_ssl);
    g_last_status = resp ? resp->status_code : 0;
    if (!resp || resp->status_code == 0) {
        http_response_free(resp);
        return 1;
    }
    if (status_out) *status_out = resp->status_code;
    ok = is_2xx(resp->status_code);
    http_response_free(resp);
    return ok ? 0 : 1;
}

int webdavcmd_del(const AcConfig *cfg, const char *remote_path)
{
    char url[AC_MAX_URL], auth[192];
    HttpResponse *resp;
    int ok;

    build_url(url, sizeof(url), cfg, remote_path);
    make_auth(auth, sizeof(auth), cfg);

    resp = http_request("DELETE", url, auth, NULL, 0, cfg->verify_ssl);
    g_last_status = resp ? resp->status_code : 0;
    if (!resp || resp->status_code == 0) { http_response_free(resp); return 1; }
    ok = is_2xx(resp->status_code) || resp->status_code == 404; /* already gone */
    http_response_free(resp);
    return ok ? 0 : 1;
}

int webdavcmd_mkdir_quiet(const AcConfig *cfg, const char *remote_path)
{
    char url[AC_MAX_URL], auth[192];
    HttpResponse *resp;
    int ok;

    build_url(url, sizeof(url), cfg, remote_path);
    make_auth(auth, sizeof(auth), cfg);

    resp = http_request("MKCOL", url, auth, NULL, 0, cfg->verify_ssl);
    g_last_status = resp ? resp->status_code : 0;
    if (!resp || resp->status_code == 0) { http_response_free(resp); return 1; }
    /* 405 Method Not Allowed = collection already exists (RFC 4918). */
    ok = is_2xx(resp->status_code) || resp->status_code == 405;
    http_response_free(resp);
    return ok ? 0 : 1;
}

int webdavcmd_rename(const AcConfig *cfg, const char *from_path, const char *to_path)
{
    char url[AC_MAX_URL], dest[AC_MAX_URL], auth[192], hdr[AC_MAX_URL + 32];
    HttpResponse *resp;
    int ok;

    build_url(url, sizeof(url), cfg, from_path);
    build_url(dest, sizeof(dest), cfg, to_path);
    make_auth(auth, sizeof(auth), cfg);
    snprintf(hdr, sizeof(hdr), "Destination: %s\r\nOverwrite: T\r\n", dest);

    resp = http_request_hdr("MOVE", url, auth, hdr, NULL, 0, cfg->verify_ssl);
    g_last_status = resp ? resp->status_code : 0;
    if (!resp || resp->status_code == 0) { http_response_free(resp); return 1; }
    ok = is_2xx(resp->status_code);
    http_response_free(resp);
    return ok ? 0 : 1;
}

int webdavcmd_info_raw(const AcConfig *cfg, const char *remote_path, AcInfo *out)
{
    char url[AC_MAX_URL], auth[192];
    HttpResponse *resp;
    int ok;
    static const char body[] =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<D:propfind xmlns:D=\"DAV:\">"
        "<D:prop>"
        "<D:resourcetype/><D:getlastmodified/><D:getcontentlength/>"
        "</D:prop>"
        "</D:propfind>";

    memset(out, 0, sizeof(*out));

    build_url(url, sizeof(url), cfg, remote_path);
    make_auth(auth, sizeof(auth), cfg);

    resp = http_request_hdr("PROPFIND", url, auth,
                            "Depth: 0\r\nContent-Type: text/xml; charset=utf-8\r\n",
                            body, (long)strlen(body), cfg->verify_ssl);
    g_last_status = resp ? resp->status_code : 0;
    if (!resp || resp->status_code == 0 || resp->status_code != 207 || !resp->body) {
        http_response_free(resp);
        return 1;
    }

    ok = parse_single_response(resp->body, remote_path, out);
    http_response_free(resp);
    return ok ? 0 : 1;
}

int webdavcmd_setdate_mtime(const AcConfig *cfg, const char *remote_path, long mtime_epoch)
{
    char url[AC_MAX_URL], auth[192], body[256];
    HttpResponse *resp;
    int ok;

    build_url(url, sizeof(url), cfg, remote_path);
    make_auth(auth, sizeof(auth), cfg);
    snprintf(body, sizeof(body),
            "<?xml version=\"1.0\"?><D:propertyupdate xmlns:D=\"DAV:\">"
            "<D:set><D:prop><D:lastmodified>%ld</D:lastmodified></D:prop></D:set>"
            "</D:propertyupdate>", mtime_epoch);

    resp = http_request_hdr("PROPPATCH", url, auth,
                            "Content-Type: text/xml; charset=utf-8\r\n",
                            body, (long)strlen(body), cfg->verify_ssl);
    g_last_status = resp ? resp->status_code : 0;
    ok = resp && (resp->status_code == 207 || is_2xx(resp->status_code));
    http_response_free(resp);
    return ok ? 0 : 1;
}
