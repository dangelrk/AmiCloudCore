/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * http_pc.c — HTTPS adapter for PC (Linux, macOS, Windows) via libcurl.
 * Compiled when PLATFORM_AMIGA is NOT defined.
 */
#ifndef PLATFORM_AMIGA

#include "http.h"
#include "base64.h"
#include "ui_hooks.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Same gate/default as http_amiga.c's ac_errors_to_stderr (see there for
 * the full rationale) — CLI/shell leave it at the default 1 (unchanged
 * behaviour), amicloud-commander-qt's main() sets it to 0 so a curl-level
 * failure can't print straight to a console the GUI user doesn't have. */
int ac_verbose        = 0;
int ac_errors_to_stderr = 1;

/* ------------------------------------------------------------------ */
/* Write callback: accumulate response into a heap buffer              */
/* ------------------------------------------------------------------ */
typedef struct {
    char  *data;
    size_t size;
} MemBuf;

static size_t mem_write(void *ptr, size_t sz, size_t nmemb, void *ud)
{
    MemBuf *buf   = (MemBuf *)ud;
    size_t  total = sz * nmemb;
    char   *tmp   = realloc(buf->data, buf->size + total + 1);
    if (!tmp) return 0;
    buf->data = tmp;
    memcpy(buf->data + buf->size, ptr, total);
    buf->size += total;
    buf->data[buf->size] = '\0';
    return total;
}

/* Write callback: stream directly to FILE*, optionally hashing as we go
 * (see http_download()'s md5ctx parameter). */
typedef struct {
    FILE   *fp;
    Md5Ctx *md5;
} DownloadSink;

static size_t file_write(void *ptr, size_t sz, size_t nmemb, void *ud)
{
    DownloadSink *sink = (DownloadSink *)ud;
    size_t got = fwrite(ptr, sz, nmemb, sink->fp);
    if (got == nmemb && sink->md5 && sz * nmemb > 0)
        md5_update(sink->md5, (const unsigned char *)ptr, sz * nmemb);
    return got * sz;
}

/* Case-insensitive "starts with" — strncasecmp isn't C89/MinGW-uniform. */
static int ci_starts_with(const char *s, size_t slen, const char *prefix)
{
    size_t i, plen = strlen(prefix);
    if (slen < plen) return 0;
    for (i = 0; i < plen; i++) {
        int a = s[i], b = prefix[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

/* Read callback: feed the upload body from a FILE* in fixed-size chunks
 * instead of one big in-memory buffer (see http_upload()). */
typedef struct {
    FILE *fp;
} UploadSource;

static size_t read_from_file(char *buffer, size_t size, size_t nitems, void *ud)
{
    UploadSource *src = (UploadSource *)ud;
    return fread(buffer, size, nitems, src->fp);
}

/* xferinfo callback shared by download and upload: forwards to
 * ui_progress() (GUI-Commander-plan section 5.2), aborting the transfer
 * if the hook returns 0. The default console hook always continues, so
 * this is a no-op for the plain CLI/shell. */
typedef struct {
    const char *name;
    int         aborted;
} ProgressCtx;

static int xferinfo_cb(void *clientp,
                       curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t ultotal, curl_off_t ulnow)
{
    ProgressCtx *pc = (ProgressCtx *)clientp;
    long total, done;

    if (ultotal > 0 || ulnow > 0) { total = (long)ultotal; done = (long)ulnow; }
    else                          { total = (long)dltotal; done = (long)dlnow; }
    if (total <= 0) total = -1;

    if (!ui_progress(done, total, pc->name)) {
        pc->aborted = 1;
        return 1; /* nonzero aborts the transfer (CURLE_ABORTED_BY_CALLBACK) */
    }
    return 0;
}

/* Header callback: pick out X-AmiCloud-MD5 into resp->md5_header. */
static size_t header_write(char *buffer, size_t size, size_t nitems, void *ud)
{
    HttpResponse *r = (HttpResponse *)ud;
    size_t len = size * nitems;
    static const char prefix[] = "X-AmiCloud-MD5:";

    if (ci_starts_with(buffer, len, prefix)) {
        const char *v = buffer + sizeof(prefix) - 1;
        const char *end = buffer + len;
        size_t n = 0;
        while (v < end && (*v == ' ' || *v == '\t')) v++;
        while (v < end && *v != '\r' && *v != '\n' && n + 1 < sizeof(r->md5_header)) {
            r->md5_header[n++] = *v++;
        }
        r->md5_header[n] = '\0';
    }
    return len;
}

/* ------------------------------------------------------------------ */
/* Build "Authorization: Basic ..." header value                       */
/* ------------------------------------------------------------------ */
static char *make_auth_header(const char *auth)
{
    char *b64 = base64_encode((const unsigned char *)auth, strlen(auth));
    if (!b64) return NULL;
    size_t hlen = strlen("Authorization: Basic ") + strlen(b64) + 1;
    char  *hdr  = malloc(hlen);
    if (hdr) snprintf(hdr, hlen, "Authorization: Basic %s", b64);
    free(b64);
    return hdr;
}

/* ------------------------------------------------------------------ */
/* Shared curl setup                                                    */
/*                                                                      */
/* One CURL easy handle, reused for every request instead of a fresh    */
/* curl_easy_init()/curl_easy_cleanup() pair per call: libcurl keeps a   */
/* per-handle connection cache internally and transparently reuses a    */
/* still-alive HTTP/1.1 (or HTTP/2) persistent connection to the same    */
/* host across curl_easy_perform() calls on the SAME handle -- exactly   */
/* the same win http_amiga.c's hand-rolled connection cache chases, but  */
/* entirely libcurl's problem here (chunked decoding, keep-alive,        */
/* stale-connection detection/retry, all handled internally). Low risk:  */
/* curl_easy_reset() below clears every option back to its default at    */
/* the START of each call, exactly as if a fresh handle had been used --*/
/* it does NOT touch the internal connection pool, which is the whole    */
/* point (see curl_easy_reset(3)). */
static CURL *g_curl = NULL;

static CURL *curl_setup(const char *url, const char *auth,
                        int verify_ssl, struct curl_slist **hdrs_out)
{
    CURL *curl;
    char *auth_hdr;

    if (!g_curl) g_curl = curl_easy_init(); /* lazy fallback if http_init() was skipped */
    curl = g_curl;
    if (!curl) return NULL;
    curl_easy_reset(curl);

    auth_hdr = make_auth_header(auth);
    if (!auth_hdr) return NULL;

    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, auth_hdr);
    free(auth_hdr);

    curl_easy_setopt(curl, CURLOPT_URL,            url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     hdrs);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, (long)verify_ssl);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, verify_ssl ? 2L : 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        60L);

    *hdrs_out = hdrs;
    return curl;
}

/* ------------------------------------------------------------------ */
HttpResponse *http_request(const char *method, const char *url,
                           const char *auth,
                           const char *upload_data, long upload_len,
                           int verify_ssl)
{
    return http_request_hdr(method, url, auth, NULL,
                            upload_data, upload_len, verify_ssl);
}

/* ------------------------------------------------------------------ */
HttpResponse *http_request_hdr(const char *method, const char *url,
                               const char *auth, const char *extra_headers,
                               const char *upload_data, long upload_len,
                               int verify_ssl)
{
    struct curl_slist *hdrs = NULL;
    CURL *curl = curl_setup(url, auth, verify_ssl, &hdrs);
    if (!curl) return NULL;

    HttpResponse *resp = calloc(1, sizeof(HttpResponse));
    if (!resp) { curl_slist_free_all(hdrs); return NULL; }

    MemBuf buf = {NULL, 0};
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, mem_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &buf);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_write);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA,     resp);

    /* Zusatz-Header zeilenweise anhaengen ("A: x\r\nB: y\r\n") */
    int have_ctype = 0;
    if (extra_headers && *extra_headers) {
        const char *p = extra_headers;
        while (*p) {
            const char *nl  = strpbrk(p, "\r\n");
            size_t      len = nl ? (size_t)(nl - p) : strlen(p);
            if (len > 0) {
                char line[AC_MAX_URL + 64];  /* Platz fuer "Destination: <url>" */
                size_t cp = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
                memcpy(line, p, cp);
                line[cp] = '\0';
                hdrs = curl_slist_append(hdrs, line);
                if (strncmp(line, "Content-Type:", 13) == 0 ||
                    strncmp(line, "content-type:", 13) == 0)
                    have_ctype = 1;
            }
            p += len;
            while (*p == '\r' || *p == '\n') p++;
        }
    }

    if (upload_data && upload_len > 0) {
        if (!have_ctype)
            hdrs = curl_slist_append(hdrs, "Content-Type: application/octet-stream");
        curl_easy_setopt(curl, CURLOPT_POST,          1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    upload_data);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, upload_len);
        if (strcmp(method, "POST") != 0)
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    } else if (strcmp(method, "POST") == 0) {
        /* mkdir and similar: POST with empty body */
        curl_easy_setopt(curl, CURLOPT_POST,          1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
    } else if (strcmp(method, "GET") != 0) {
        /* DELETE, PROPFIND, MKCOL, MOVE, ... ohne Body */
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    }
    /* GET is the default */
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, (long *)&resp->status_code);
    } else {
        fprintf(stderr, "amicloudcore: curl error: %s\n", curl_easy_strerror(rc));
        resp->status_code = 0;
    }

    resp->body     = buf.data;
    resp->body_len = (long)buf.size;

    curl_slist_free_all(hdrs);
    return resp;
}

/* ------------------------------------------------------------------ */
HttpResponse *http_download(const char *url, const char *auth,
                            FILE *fp, int verify_ssl, Md5Ctx *md5ctx,
                            const char *display_name)
{
    struct curl_slist *hdrs = NULL;
    CURL *curl = curl_setup(url, auth, verify_ssl, &hdrs);
    if (!curl) return NULL;

    HttpResponse *resp = calloc(1, sizeof(HttpResponse));
    if (!resp) { curl_slist_free_all(hdrs); return NULL; }

    DownloadSink sink;
    sink.fp  = fp;
    sink.md5 = md5ctx;
    ProgressCtx pc;
    pc.name = display_name;
    pc.aborted = 0;

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, file_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &sink);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_write);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA,     resp);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS,        0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,  xferinfo_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA,      &pc);

    CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, (long *)&resp->status_code);
    } else {
        if (!pc.aborted) fprintf(stderr, "amicloudcore: curl error: %s\n", curl_easy_strerror(rc));
        resp->status_code = 0;
    }
    resp->aborted = pc.aborted;

    curl_slist_free_all(hdrs);
    return resp;
}

/* ------------------------------------------------------------------ */
HttpResponse *http_upload(const char *url, const char *auth,
                          FILE *fp, long fsize, int verify_ssl,
                          const char *display_name)
{
    struct curl_slist *hdrs = NULL;
    CURL *curl = curl_setup(url, auth, verify_ssl, &hdrs);
    if (!curl) return NULL;

    HttpResponse *resp = calloc(1, sizeof(HttpResponse));
    if (!resp) { curl_slist_free_all(hdrs); return NULL; }

    MemBuf buf = {NULL, 0};
    UploadSource src;
    src.fp = fp;
    ProgressCtx pc;
    pc.name = display_name;
    pc.aborted = 0;

    hdrs = curl_slist_append(hdrs, "Content-Type: application/octet-stream");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    curl_easy_setopt(curl, CURLOPT_POST,          1L);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION,  read_from_file);
    curl_easy_setopt(curl, CURLOPT_READDATA,      &src);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, fsize);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, mem_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &buf);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_write);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA,     resp);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS,        0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,  xferinfo_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA,      &pc);

    CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, (long *)&resp->status_code);
    } else {
        if (!pc.aborted) fprintf(stderr, "amicloudcore: curl error: %s\n", curl_easy_strerror(rc));
        resp->status_code = 0;
    }
    resp->aborted = pc.aborted;

    resp->body     = buf.data;
    resp->body_len = (long)buf.size;

    curl_slist_free_all(hdrs);
    return resp;
}

/* ------------------------------------------------------------------ */
HttpResponse *http_upload_hdr(const char *method, const char *url, const char *auth,
                              const char *extra_headers, FILE *fp, long fsize,
                              int verify_ssl, const char *display_name)
{
    struct curl_slist *hdrs = NULL;
    CURL *curl = curl_setup(url, auth, verify_ssl, &hdrs);
    if (!curl) return NULL;

    HttpResponse *resp = calloc(1, sizeof(HttpResponse));
    if (!resp) { curl_slist_free_all(hdrs); return NULL; }

    MemBuf buf = {NULL, 0};
    UploadSource src;
    src.fp = fp;
    ProgressCtx pc;
    pc.name = display_name;
    pc.aborted = 0;

    /* Same "line-by-line, Content-Type overridable" handling as
     * http_request_hdr() above. */
    int have_ctype = 0;
    if (extra_headers && *extra_headers) {
        const char *p = extra_headers;
        while (*p) {
            const char *nl  = strpbrk(p, "\r\n");
            size_t      len = nl ? (size_t)(nl - p) : strlen(p);
            if (len > 0) {
                char line[AC_MAX_URL + 64];
                size_t cp = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
                memcpy(line, p, cp);
                line[cp] = '\0';
                hdrs = curl_slist_append(hdrs, line);
                if (strncmp(line, "Content-Type:", 13) == 0 ||
                    strncmp(line, "content-type:", 13) == 0)
                    have_ctype = 1;
            }
            p += len;
            while (*p == '\r' || *p == '\n') p++;
        }
    }
    if (!have_ctype)
        hdrs = curl_slist_append(hdrs, "Content-Type: application/octet-stream");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    curl_easy_setopt(curl, CURLOPT_UPLOAD,        1L);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION,  read_from_file);
    curl_easy_setopt(curl, CURLOPT_READDATA,      &src);
    curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)fsize);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, mem_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &buf);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_write);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA,     resp);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS,        0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,  xferinfo_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA,      &pc);

    CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, (long *)&resp->status_code);
    } else {
        if (!pc.aborted) fprintf(stderr, "amicloudcore: curl error: %s\n", curl_easy_strerror(rc));
        resp->status_code = 0;
    }
    resp->aborted = pc.aborted;

    resp->body     = buf.data;
    resp->body_len = (long)buf.size;

    curl_slist_free_all(hdrs);
    return resp;
}

/* ------------------------------------------------------------------ */
void http_response_free(HttpResponse *r)
{
    if (r) { free(r->body); free(r); }
}

static char g_last_error[256] = "";

const char *http_last_error(void)
{
    return g_last_error;
}

int http_init(void)
{
    g_last_error[0] = '\0';
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        snprintf(g_last_error, sizeof(g_last_error), "curl_global_init failed");
        return 0;
    }
    g_curl = curl_easy_init();
    if (!g_curl) snprintf(g_last_error, sizeof(g_last_error), "curl_easy_init failed");
    return g_curl ? 1 : 0;
}

void http_cleanup(void)
{
    if (g_curl) { curl_easy_cleanup(g_curl); g_curl = NULL; }
    curl_global_cleanup();
}

#endif /* !PLATFORM_AMIGA */
