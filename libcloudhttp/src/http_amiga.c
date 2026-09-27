/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * http_amiga.c — HTTPS adapter for AmigaOS via bsdsocket.library + AmiSSL 5.x.
 * Compiled only when PLATFORM_AMIGA is defined (VBCC +aos68k).
 *
 * HTTP/1.1 with a single-slot persistent-connection cache (see
 * g_conn_host/conn_cache_*() below): consecutive requests to the same
 * host:port reuse the same TCP+TLS connection instead of paying a fresh
 * TCP handshake + full TLS handshake every time -- the single biggest
 * cost for WebDAV traffic, which issues far more requests per directory
 * (one PROPFIND per level plus one per file/icon touched) than AmiCloud's
 * flatter api.php calls ever did. One slot is enough: this worker only
 * ever has one request in flight at a time (synchronous request/reply,
 * see backend.c), never concurrent connections to different hosts.
 *
 * HTTP/1.1 lets a server answer with "Transfer-Encoding: chunked"
 * instead of a Content-Length (routinely the case for PROPFIND's
 * dynamically-generated XML) -- read_body_chunked() below decodes that
 * IN PLACE in the response buffer, so every other body-extraction call
 * site in this file (find_body()/do_request()) stays exactly as
 * chunking-unaware as it was under the old HTTP/1.0 code. A response is
 * only ever handed back to the connection cache for reuse if its framing
 * was unambiguous (exact Content-Length reached, or a chunked
 * terminator seen) AND the server didn't send "Connection: close" --
 * anything else (timeout, malformed chunk, a response with neither
 * Content-Length nor chunking, a cancelled progress hook) falls back to
 * the original "read until the peer closes" behaviour and the
 * connection is discarded afterward, never reused. A connection taken
 * from the cache that turns out to have been silently closed by the
 * server in the meantime (idle keep-alive timeout) is retried exactly
 * once against a guaranteed-fresh connection -- see do_request()'s
 * try_reuse loop.
 */
#ifdef PLATFORM_AMIGA

/* dos/dos.h MUSS als erstes kommen: es inkludiert exec/types.h intern und    */
/* definiert damit BPTR exakt einmal. Alle spaeten Versuche (bsdsocket.h,    */
/* exec/types.h direkt) feuern dann nur noch die Include-Guards — kein       */
/* warning 226 "redeclaration of typedef BPTR", kein VBCC-Typverlust.        */
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <exec/types.h>       /* guard fires — already pulled in by dos/dos.h */
#include <exec/libraries.h>
#include <proto/exec.h>
#include <proto/dos.h>        /* Write(), Open(), Close() inline stubs */
#include <proto/socket.h>     /* bsdsocket.h -> dos/dos.h -> guard fires, clean */
#include <libraries/amisslmaster.h>
#include <libraries/amissl.h>
#include <proto/amisslmaster.h>
#include <proto/amissl.h>
#include <amissl/amissl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
/* sys/time.h nicht im Roadshow-SDK — struct timeval kommt aus exec/types.h */
#include <netinet/in.h>

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "http.h"
#include "base64.h"
#include "ui_hooks.h"

/* ------------------------------------------------------------------ */
/* Library bases (opened in http_init)                                 */
/* ------------------------------------------------------------------ */
struct Library  *SocketBase       = NULL;
struct Library  *AmiSSLMasterBase = NULL;
struct Library  *AmiSSLBase       = NULL;
struct Library  *AmiSSLExtBase    = NULL;
struct Library  *UtilityBase      = NULL;
SSL_CTX         *g_ssl_ctx        = NULL;
int              ac_verbose        = 0;
/* Gate for herr_fn's plain-stderr fallback below (client mode only — the
 * http_log_bptr branch already goes to http_log_path/g_log_buf, never
 * stderr). Default 1 = unchanged behaviour for the CLI (amicloud) and
 * shell (amicloud-shell). The MUI commander (main_mui.c) sets this to 0
 * except when its own debug toggle (-v / DEBUG tooltype) is active, so a
 * stray HTTP error can't pop an unwanted console/output window. */
int              ac_errors_to_stderr = 1;
/* http_log_bptr: Sentinel-Flag (!=0 = HTTP-Log aktiv); kein offenes Handle.
 * http_log_path: Zieldatei fuer HTTP-Verbose-Log; wird von main() gesetzt. */
BPTR             http_log_bptr     = 0;
char             http_log_path[256] = "";
/* In-Memory-Log-Puffer: bleibt im CLI NULL (Datei-/stderr-Fallback greift).
 * Ein Handler/Worker darf ihn setzen, um DOS-freies Logging zu bekommen. */
char            *g_log_buf         = NULL;
ULONG            g_log_pos         = 0;
ULONG            g_log_size        = 0;

static BOOL      AmiSSLInitialized = FALSE;

/* ------------------------------------------------------------------ */
/* Logging helpers                                                      */
/*                                                                     */
/* HLOG — verbose/debug output:                                        */
/*   handler mode (http_log_bptr set): open/write/close nach http_log_path */
/*   client mode (http_log_bptr 0): nur bei ac_verbose (-v) nach stderr    */
/*                                                                     */
/* HERR — error/warning output: wie HLOG, aber client immer nach stderr    */
/*                                                                     */
/* open/write/close pro Eintrag: Datei nie dauerhaft gesperrt,         */
/* jederzeit lesbar/kopierbar waehrend der Handler laeuft.             */
/* ------------------------------------------------------------------ */
static void http_write_log(const char *buf, LONG len)
{
    /* During packet_loop g_log_buf is set — write there (no DOS calls).
     * Before startup reply (backend_init phase) g_log_buf is also set.
     * Fallback file I/O is kept for the case where neither is available. */
    if (g_log_buf && g_log_pos + (ULONG)len < g_log_size) {
        memcpy(g_log_buf + g_log_pos, buf, (size_t)len);
        g_log_pos += (ULONG)len;
    } else if (!g_log_buf && http_log_path[0]) {
        BPTR f = Open(http_log_path, MODE_READWRITE);
        if (f) { Seek(f, 0, OFFSET_END); Write(f, buf, len); Close(f); }
    }
}

static void hlog_fn(const char *fmt, ...)
{
    va_list ap;
    if (http_log_bptr && http_log_path[0]) {
        char buf[512]; LONG len;
        va_start(ap, fmt); len = (LONG)vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
        if (len > 0) http_write_log(buf, len);
    } else if (ac_verbose) {
        va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    }
}

static void herr_fn(const char *fmt, ...)
{
    va_list ap;
    if (http_log_bptr && http_log_path[0]) {
        char buf[512]; LONG len;
        va_start(ap, fmt); len = (LONG)vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
        if (len > 0) http_write_log(buf, len);
    } else if (ac_errors_to_stderr) {
        va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    }
}

/* Public wrapper around the same sink as HLOG/hlog_fn above, for callers
 * outside this file (see http.h). Identical body to hlog_fn -- kept
 * separate rather than having cmd.c call hlog_fn directly since that one
 * stays static/private to this translation unit on purpose. */
void ac_log_printf(const char *fmt, ...)
{
    va_list ap;
    if (http_log_bptr && http_log_path[0]) {
        char buf[512]; LONG len;
        va_start(ap, fmt); len = (LONG)vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
        if (len > 0) http_write_log(buf, len);
    } else if (ac_verbose) {
        va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    }
}

#define HLOG hlog_fn
#define HERR herr_fn

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int parse_url(const char *url,
                     char *host, size_t host_sz,
                     int  *port,
                     char *path, size_t path_sz)
{
    const char *p;
    const char *slash;

    if (strncmp(url, "https://", 8) != 0) return 0;
    p = url + 8;

    slash = strchr(p, '/');
    if (!slash) {
        strncpy(host, p, host_sz - 1);
        host[host_sz - 1] = '\0';
        strncpy(path, "/", path_sz - 1);
        path[path_sz - 1] = '\0';
    } else {
        size_t hlen = (size_t)(slash - p);
        if (hlen >= host_sz) hlen = host_sz - 1;
        strncpy(host, p, hlen);
        host[hlen] = '\0';
        strncpy(path, slash, path_sz - 1);
        path[path_sz - 1] = '\0';
    }

    {
        char *col = strchr(host, ':');
        if (col) {
            *port = atoi(col + 1);
            *col  = '\0';
        } else {
            *port = 443;
        }
    }
    return 1;
}

/* One-entry DNS cache: after warmup the handler never calls gethostbyname
 * again during packet_loop, avoiding any DOS calls that the TCP/IP stack
 * might make to read ENVARC:sys/hosts.  The cache is filled by http_warmup()
 * before devnode->dn_Task is set. */
static struct in_addr g_dns_cache_addr;
static char           g_dns_cache_host[256] = "";

static int resolve_host(const char *host, struct sockaddr_in *addr)
{
    unsigned long ip;

    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;

    /* Fast path: dotted-decimal IP — no gethostbyname, no DOS */
    ip = inet_addr((char *)host);
    if (ip != (unsigned long)0xFFFFFFFFUL) {
        addr->sin_addr.s_addr = ip;
        HLOG("dns: IP literal %s\n", host);
        return 1;
    }

    /* Cached result (filled by http_warmup before dn_Task is set) */
    if (g_dns_cache_host[0] && strcmp(g_dns_cache_host, host) == 0) {
        addr->sin_addr = g_dns_cache_addr;
        HLOG("dns: cache hit %s\n", host);
        return 1;
    }

    /* Real lookup — may call dos.library; safe ONLY before dn_Task is set */
    HLOG("dns: resolving '%s'...\n", host);
    {
        const struct hostent *he = gethostbyname((char *)host);
        if (!he) { HERR("amicloud: cannot resolve '%s'\n", host); return 0; }
        memcpy(&addr->sin_addr, he->h_addr_list[0], 4);
        /* Cache for all subsequent calls in packet_loop */
        g_dns_cache_addr = addr->sin_addr;
        strncpy(g_dns_cache_host, host,
                sizeof(g_dns_cache_host) - 1);
        g_dns_cache_host[sizeof(g_dns_cache_host) - 1] = '\0';
        HLOG("dns: resolved '%s' OK\n", host);
    }
    return 1;
}

static char *make_basic_auth(const char *auth)
{
    char *b64 = base64_encode((const unsigned char *)auth, strlen(auth));
    if (!b64) return NULL;
    size_t len = strlen("Basic ") + strlen(b64) + 1;
    char  *hdr = malloc(len);
    if (hdr) snprintf(hdr, len, "Basic %s", b64);
    free(b64);
    return hdr;
}

/* ------------------------------------------------------------------ */
/* Low-level: open TCP socket + TLS, return SSL* or NULL on error      */
/* ------------------------------------------------------------------ */
static SSL *tls_connect(const char *host, int port, int verify,
                        AC_SOCKET *sock_out)
{
    struct sockaddr_in addr;
    AC_SOCKET sock;
    SSL      *ssl;

    HLOG("tls: resolving '%s'...\n", host);
    if (!resolve_host(host, &addr)) {
        HERR("amicloud: cannot resolve '%s'\n", host);
        return NULL;
    }
    HLOG("tls: resolved OK\n");
    addr.sin_port = htons((unsigned short)port);

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        HERR("amicloud: socket() failed\n");
        return NULL;
    }
    HLOG("tls: socket fd=%ld\n", (long)sock);

    /* 30-second receive timeout — covers SSL_read in ssl_read_all */
    {
        struct timeval tv;
        tv.tv_secs  = 30;
        tv.tv_micro = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char *)&tv, sizeof(tv));
        HLOG("tls: SO_RCVTIMEO/SO_SNDTIMEO = 30s\n");
    }

    /* Non-blocking connect + WaitSelect so connect() never hangs forever */
    {
        LONG nb = 1;
        IoctlSocket(sock, FIONBIO, (char *)&nb);
    }

    HLOG("tls: connect() to %s:%d...\n", host, port);
    connect(sock, (struct sockaddr *)&addr, sizeof(addr)); /* EINPROGRESS expected */

    {
        fd_set wfds;
        struct timeval tv;
        int sel, soerr;
        LONG soerr_len;  /* LONG* — Roadshow getsockopt erwartet LONG-Laenge */

        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        tv.tv_secs  = 30;
        tv.tv_micro = 0;

        HLOG("tls: WaitSelect(30s)...\n");
        /* (void*) cast: Roadshow-struct timeval vs exec/types.h — gleiche Binaerlayout auf 68k */
        sel = WaitSelect(sock + 1, NULL, &wfds, NULL, (void *)&tv, NULL);
        HLOG("tls: WaitSelect returned %d\n", sel);

        if (sel <= 0) {
            HERR("amicloud: connect to %s:%d timed out\n", host, port);
            CloseSocket(sock);
            return NULL;
        }
        soerr = 0; soerr_len = (LONG)sizeof(soerr);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&soerr, (void *)&soerr_len);
        if (soerr != 0) {
            HERR("amicloud: connect to %s:%d failed (err %d)\n", host, port, soerr);
            CloseSocket(sock);
            return NULL;
        }
        HLOG("tls: connect OK\n");
    }

    /* Switch to blocking for SSL_connect — simpler than non-blocking handshake */
    {
        LONG nb = 0;
        IoctlSocket(sock, FIONBIO, (char *)&nb);
    }

    HLOG("tls: SSL_new...\n");
    ssl = SSL_new(g_ssl_ctx);
    if (!ssl) {
        HERR("amicloud: SSL_new failed\n");
        CloseSocket(sock);
        return NULL;
    }

    SSL_set_verify(ssl, verify ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, NULL);
    SSL_set_fd(ssl, (int)sock);
    SSL_set_tlsext_host_name(ssl, host); /* SNI */
    HLOG("tls: SSL_set_fd OK, verify=%d, SNI=%s\n", verify, host);

    HLOG("tls: SSL_connect...\n");
    if (SSL_connect(ssl) != 1) {
        HERR("amicloud: TLS handshake with %s failed\n", host);
        SSL_free(ssl);
        CloseSocket(sock);
        return NULL;
    }
    HLOG("tls: SSL_connect OK, cipher=%s\n", SSL_get_cipher(ssl));

    /* Re-enable non-blocking for data reads.
     * ssl_read_all uses WaitSelect(30s) so SO_RCVTIMEO is not needed. */
    {
        LONG nb = 1;
        IoctlSocket(sock, FIONBIO, (char *)&nb);
    }

    *sock_out = sock;
    return ssl;
}

static void tls_close(SSL *ssl, AC_SOCKET sock)
{
    if (ssl)  { SSL_shutdown(ssl); SSL_free(ssl); }
    if (sock != INVALID_SOCKET) CloseSocket(sock);
}

/* ------------------------------------------------------------------ */
/* Single-slot persistent-connection cache (see file header comment).   */
/* ------------------------------------------------------------------ */
static char      g_conn_host[256] = "";
static int       g_conn_port      = 0;
static AC_SOCKET g_conn_sock      = INVALID_SOCKET;
static SSL      *g_conn_ssl       = NULL;

static void conn_cache_close(void)
{
    if (g_conn_ssl) {
        HLOG("conn: closing cached connection to %s:%d\n", g_conn_host, g_conn_port);
        tls_close(g_conn_ssl, g_conn_sock);
    }
    g_conn_ssl     = NULL;
    g_conn_sock    = INVALID_SOCKET;
    g_conn_host[0] = '\0';
}

/* Discards whatever's currently cached (closing it if it's a different,
 * still-open connection) and stores ssl/sock as the new cached
 * connection for host:port. Caller must only call this with a
 * connection it has just finished using and confirmed reusable (see
 * do_request()) -- this function itself does no framing validation. */
static void conn_cache_store(const char *host, int port, SSL *ssl, AC_SOCKET sock)
{
    conn_cache_close();
    strncpy(g_conn_host, host, sizeof(g_conn_host) - 1);
    g_conn_host[sizeof(g_conn_host) - 1] = '\0';
    g_conn_port = port;
    g_conn_ssl  = ssl;
    g_conn_sock = sock;
    HLOG("conn: cached connection to %s:%d for reuse\n", host, port);
}

/* Removes and returns the cached connection if it matches host:port
 * (cache slot is empty afterward either way -- a caller that decides not
 * to use a hit must tls_close() it itself, never leave it half-taken).
 * Returns 1 on a hit (ssl/sock filled in), 0 on a miss. */
static int conn_cache_take(const char *host, int port, SSL **ssl, AC_SOCKET *sock)
{
    if (g_conn_ssl && g_conn_port == port && strcmp(g_conn_host, host) == 0) {
        HLOG("conn: reusing cached connection to %s:%d\n", host, port);
        *ssl  = g_conn_ssl;
        *sock = g_conn_sock;
        g_conn_ssl     = NULL;
        g_conn_sock    = INVALID_SOCKET;
        g_conn_host[0] = '\0';
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Read the full HTTP response into a malloc'd buffer.                  */
/* ------------------------------------------------------------------ */
#define READ_CHUNK 4096

/* find_body()/extract_header()/ci_eqn() are defined further down this
 * file, but ssl_read_response() needs them early (as soon as the header
 * block is fully received) to learn Content-Length/Transfer-Encoding/
 * Connection before the body finishes — forward declared here rather
 * than moving ~70 lines of code around. */
static const char *find_body(const char *resp, size_t len);
static void extract_header(const char *headers, size_t headers_len,
                           const char *name, char *out, size_t outsz);
static int ci_eqn(const char *a, const char *b, size_t n);

/* Single WaitSelect(30s)+SSL_read cycle, appending whatever arrives to
 * *buf/*size (realloc'd as needed) and reporting progress. Shared by all
 * three body-framing strategies below. Returns 1 if >=1 byte was
 * appended, 0 on clean shutdown/timeout/error/malloc failure/user-abort
 * (check *aborted_out to tell a genuine user cancel from the rest) --
 * callers interpret a 0 return according to which framing they're
 * using (see each function's own comment). */
static int read_more(SSL *ssl, AC_SOCKET sock, char **buf, size_t *size,
                     long progress_done, long progress_total,
                     const char *display_name, int *aborted_out)
{
    char tmp[READ_CHUNK];
    for (;;) {
        int n;
        if (SSL_pending(ssl) == 0) {
            fd_set rfds;
            struct timeval tv;
            int sel;
            FD_ZERO(&rfds);
            FD_SET(sock, &rfds);
            tv.tv_secs  = 30;
            tv.tv_micro = 0;
            sel = WaitSelect(sock + 1, &rfds, NULL, NULL, (void *)&tv, NULL);
            HLOG("read: WaitSelect=%d\n", sel);
            if (sel <= 0) {
                HERR("amicloud: read timeout (30s) after %lu bytes\n", (unsigned long)*size);
                return 0;
            }
        }
        n = SSL_read(ssl, tmp, READ_CHUNK);
        if (n > 0) {
            char *nb = realloc(*buf, *size + (size_t)n + 1);
            if (!nb) return 0;
            *buf = nb;
            memcpy(*buf + *size, tmp, (size_t)n);
            *size += (size_t)n;
            (*buf)[*size] = '\0';
            HLOG("read: +%d bytes (total %lu)\n", n, (unsigned long)*size);
            if (!ui_progress(progress_done, progress_total, display_name)) {
                HLOG("read: cancelled by progress hook\n");
                *aborted_out = 1;
                return 0;
            }
            return 1;
        } else if (n == 0) {
            HLOG("read: clean shutdown\n");
            return 0;
        } else {
            int err = SSL_get_error(ssl, n);
            if (err == SSL_ERROR_WANT_READ) continue; /* WaitSelect again */
            HLOG("read: SSL_read error %d\n", err);
            return 0;
        }
    }
}

/* Original pre-keep-alive framing: read until the peer closes the
 * connection, or a genuine timeout/error/abort occurs -- used whenever a
 * response has neither Content-Length nor chunked Transfer-Encoding
 * (nothing else tells us where the body ends). Always returns 1 (best-
 * effort partial body on timeout/error, matching the old ssl_read_all()'s
 * behaviour exactly) except on a user-abort, which the caller must still
 * treat as "stop, don't reuse" -- see *aborted_out. The connection is
 * NEVER reusable after this path regardless of outcome (the close is
 * literally how we knew we were done). */
static int read_body_until_close(SSL *ssl, AC_SOCKET sock, char **buf, size_t *size,
                                 size_t header_len, long total_body,
                                 const char *display_name, int *aborted_out)
{
    for (;;) {
        long body_done = (long)(*size - header_len);
        if (!read_more(ssl, sock, buf, size, body_done, total_body, display_name, aborted_out))
            return 1; /* clean shutdown, timeout, or error -- all end the read here */
    }
}

/* Reads until exactly header_len+content_len bytes are in *buf (i.e.
 * content_len bytes of body beyond the already-buffered header block).
 * Returns 1 on success (connection may be reused afterward), 0 on
 * timeout/short-close/error/abort (caller must not reuse the connection
 * -- our position in the byte stream is no longer at a response
 * boundary). */
static int read_body_exact(SSL *ssl, AC_SOCKET sock, char **buf, size_t *size,
                           size_t header_len, long content_len,
                           const char *display_name, int *aborted_out)
{
    size_t want_total = header_len + (content_len > 0 ? (size_t)content_len : 0);
    while (*size < want_total) {
        long body_done = (long)(*size - header_len);
        if (!read_more(ssl, sock, buf, size, body_done, content_len, display_name, aborted_out))
            return 0;
    }
    return 1;
}

/* Decodes an HTTP/1.1 chunked body IN PLACE within *buf, starting right
 * after the header block (header_len) -- removes each chunk-size line
 * and its trailing CRLF as it goes, so *buf ends up holding a plain,
 * unchunked body exactly like a Content-Length response would (every
 * other body-extraction call site in this file stays chunking-unaware).
 * Returns 1 on success (0-length terminator chunk + trailer consumed;
 * connection may be reused afterward), 0 on malformed input, timeout,
 * I/O error, or abort (caller must not reuse the connection). */
static int read_body_chunked(SSL *ssl, AC_SOCKET sock, char **buf, size_t *size,
                             size_t header_len, const char *display_name,
                             int *aborted_out)
{
    size_t cursor = header_len; /* unparsed raw wire bytes start here */
    size_t write  = header_len; /* decoded body is compacted down to here */
    long   body_done = 0;

    for (;;) {
        char *eol;

        while ((eol = (char *)memchr(*buf + cursor, '\n', *size - cursor)) == NULL) {
            if (!read_more(ssl, sock, buf, size, body_done, -1, display_name, aborted_out))
                return 0;
        }

        {
            long  chunk_size;
            char *p;
            chunk_size = strtol(*buf + cursor, &p, 16); /* stops at ';ext' or '\r' on its own */
            if (p == *buf + cursor || chunk_size < 0) {
                HERR("amicloud: malformed chunk size\n");
                return 0;
            }
            cursor = (size_t)(eol - *buf) + 1; /* past the size line's terminating \n */

            if (chunk_size == 0) {
                /* Terminator chunk -- consume trailer headers (almost
                 * always none) up to and including the final blank line. */
                for (;;) {
                    if (cursor + 1 < *size && (*buf)[cursor] == '\r' && (*buf)[cursor + 1] == '\n') {
                        cursor += 2;
                        break;
                    }
                    {
                        const char *tail_end = find_body(*buf + cursor, *size - cursor);
                        if (tail_end) { cursor = (size_t)(tail_end - *buf); break; }
                    }
                    if (!read_more(ssl, sock, buf, size, body_done, -1, display_name, aborted_out)) {
                        /* A clean shutdown right after "0\r\n" (no final
                         * CRLF byte) is common enough server behaviour to
                         * tolerate rather than fail on -- but only a clean
                         * shutdown, never a timeout/error/abort. */
                        if (*aborted_out) return 0;
                        break;
                    }
                }
                *size = write;
                (*buf)[*size] = '\0';
                return 1;
            }

            while (*size < cursor + (size_t)chunk_size + 2) {
                if (!read_more(ssl, sock, buf, size, body_done, -1, display_name, aborted_out))
                    return 0;
            }

            if (write != cursor) memmove(*buf + write, *buf + cursor, (size_t)chunk_size);
            write     += (size_t)chunk_size;
            cursor    += (size_t)chunk_size + 2; /* data + its trailing \r\n */
            body_done += chunk_size;
        }
    }
}

/* Reads one full HTTP response (headers + framing-appropriate body) into
 * a malloc'd buffer, exactly like the old ssl_read_all() did -- callers
 * (do_request()) still just find_body()/parse_status()/extract_header()
 * against the result unchanged. *reusable_out tells do_request() whether
 * ssl/sock are still at a clean response boundary and safe to hand to
 * conn_cache_store() (see this file's header comment for the framing
 * rules that decide this). */
static char *ssl_read_response(SSL *ssl, AC_SOCKET sock, size_t *out_len,
                               const char *display_name, int *aborted_out,
                               int *reusable_out)
{
    char   *buf  = NULL;
    size_t  size = 0;
    size_t  header_len = 0;
    long    total_body = -1;
    int     chunked    = 0;
    int     conn_close = 0;
    int     ok;

    *aborted_out  = 0;
    *reusable_out = 0;
    HLOG("read: waiting for response...\n");

    /* Phase 1: headers. */
    for (;;) {
        const char *body_start;
        if (!read_more(ssl, sock, &buf, &size, 0, -1, display_name, aborted_out)) {
            *out_len = size;
            return buf; /* timeout/shutdown/error/abort before headers ever completed */
        }
        body_start = find_body(buf, size);
        if (body_start) {
            char clbuf[32], tebuf[64], connbuf[32];
            header_len = (size_t)(body_start - buf);
            extract_header(buf, header_len, "Content-Length:", clbuf, sizeof(clbuf));
            total_body = clbuf[0] ? atol(clbuf) : -1;
            extract_header(buf, header_len, "Transfer-Encoding:", tebuf, sizeof(tebuf));
            chunked = (tebuf[0] && strstr(tebuf, "chunked") != NULL);
            extract_header(buf, header_len, "Connection:", connbuf, sizeof(connbuf));
            conn_close = (connbuf[0] && ci_eqn(connbuf, "close", 5));
            HLOG("read: headers complete (%lu bytes), Content-Length=%ld, chunked=%d, conn_close=%d\n",
                 (unsigned long)header_len, total_body, chunked, conn_close);
            break;
        }
    }

    /* Phase 2: body, framed according to what phase 1 found. */
    if (chunked) {
        ok = read_body_chunked(ssl, sock, &buf, &size, header_len, display_name, aborted_out);
        *reusable_out = ok && !conn_close;
    } else if (total_body >= 0) {
        ok = read_body_exact(ssl, sock, &buf, &size, header_len, total_body, display_name, aborted_out);
        *reusable_out = ok && !conn_close;
    } else {
        read_body_until_close(ssl, sock, &buf, &size, header_len, total_body, display_name, aborted_out);
        *reusable_out = 0; /* the close is how we knew we were done -- nothing left to reuse */
    }

    HLOG("read: done, %lu bytes total, reusable=%d\n", (unsigned long)size, *reusable_out);
    *out_len = size;
    return buf;
}

/* ------------------------------------------------------------------ */
/* Parse HTTP status line; return numeric code or 0                     */
/* ------------------------------------------------------------------ */
static int parse_status(const char *resp)
{
    int code = 0;
    if (strncmp(resp, "HTTP/", 5) == 0) {
        const char *sp = strchr(resp, ' ');
        if (sp) code = atoi(sp + 1);
    }
    return code;
}

static const char *find_body(const char *resp, size_t len)
{
    size_t i;
    for (i = 0; i + 3 < len; i++) {
        if (resp[i]   == '\r' && resp[i+1] == '\n' &&
            resp[i+2] == '\r' && resp[i+3] == '\n') {
            return resp + i + 4;
        }
    }
    return NULL;
}

/* Case-insensitive n-char compare — strnicmp isn't uniformly available
 * across VBCC/MinGW/gcc. */
static int ci_eqn(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        int ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
    }
    return 1;
}

/* Pick "X-AmiCloud-MD5: <value>" out of the raw header block (headers_len
 * bytes starting at raw — everything before the \r\n\r\n body separator).
 * out[0] stays '\0' if the header isn't present. */
static void extract_header(const char *headers, size_t headers_len,
                           const char *name, char *out, size_t outsz)
{
    size_t namelen = strlen(name);
    const char *p   = headers;
    const char *end = headers + headers_len;

    out[0] = '\0';
    while (p < end) {
        const char *eol = p;
        while (eol < end && *eol != '\r' && *eol != '\n') eol++;
        if ((size_t)(eol - p) > namelen && ci_eqn(p, name, namelen)) {
            const char *v = p + namelen;
            size_t n = 0;
            while (v < eol && (*v == ' ' || *v == '\t')) v++;
            while (v < eol && n + 1 < outsz) out[n++] = *v++;
            out[n] = '\0';
            return;
        }
        p = eol;
        while (p < end && (*p == '\r' || *p == '\n')) p++;
    }
}

/* ------------------------------------------------------------------ */
/* Core request function                                                */
/* ------------------------------------------------------------------ */
static HttpResponse *do_request(const char *method, const char *url,
                                const char *auth, const char *extra_headers,
                                const char *upload_data, long upload_len,
                                FILE *upload_fp, long upload_fsize,
                                FILE *download_fp, Md5Ctx *md5ctx,
                                const char *display_name, int verify_ssl)
{
    char host[256], path[AC_MAX_URL];
    int  port;
    long upload_fp_start = upload_fp ? ftell(upload_fp) : 0;
    int  attempt;

    HLOG("http: %s %s\n", method, url);

    if (!parse_url(url, host, sizeof(host), &port, path, sizeof(path))) {
        HERR("amicloud: invalid URL: %s\n", url);
        return NULL;
    }
    HLOG("http: host=%s port=%d path=%s\n", host, port, path);

    /* At most 2 attempts: a connection taken from the cache (attempt 0)
     * might have been silently closed by the server in the meantime (an
     * idle keep-alive timeout) -- detected below as "wrote nothing" or
     * "got nothing back" -- in which case attempt 1 forces a guaranteed-
     * fresh tls_connect() and retries the whole request once. A fresh
     * connection failing is always a hard error, no retry (matches the
     * pre-keep-alive behaviour exactly). */
    for (attempt = 0; attempt < 2; attempt++) {
        AC_SOCKET sock = INVALID_SOCKET;
        SSL      *ssl  = NULL;
        char     *raw  = NULL;
        size_t    raw_len = 0;
        int       aborted     = 0;
        int       reusable    = 0;
        int       reused      = 0;
        int       write_failed = 0;
        HttpResponse *resp = NULL;

        if (attempt == 0 && conn_cache_take(host, port, &ssl, &sock)) {
            reused = 1;
        } else {
            ssl = tls_connect(host, port, verify_ssl, &sock);
            if (!ssl) return NULL;
        }

        if (upload_fp) fseek(upload_fp, upload_fp_start, SEEK_SET);

        {
            char *auth_val = make_basic_auth(auth);
            char req_head[2048];
            int  req_len;
            int  hdr_sent;

            const char *extra = (extra_headers && *extra_headers) ? extra_headers : "";
            /* Eigener Content-Type in extra_headers ersetzt den Default */
            int have_ctype = (strstr(extra, "Content-Type:") != NULL ||
                              strstr(extra, "content-type:") != NULL);

            if (!auth_val) { tls_close(ssl, sock); return NULL; }

            /* HTTP/1.1, no explicit Connection header -- persistent by
             * default (see this file's header comment); the server tells
             * us via its OWN "Connection: close" response header if it
             * intends to close anyway (checked in ssl_read_response()). */
            if (upload_data && upload_len > 0) {
                req_len = snprintf(req_head, sizeof(req_head),
                    "%s %s HTTP/1.1\r\n"
                    "Host: %s\r\n"
                    "Authorization: %s\r\n"
                    "%s"
                    "%s"
                    "Content-Length: %ld\r\n"
                    "\r\n",
                    method, path, host, auth_val,
                    have_ctype ? "" : "Content-Type: application/octet-stream\r\n",
                    extra, upload_len);
            } else if (upload_fp && upload_fsize > 0) {
                req_len = snprintf(req_head, sizeof(req_head),
                    "%s %s HTTP/1.1\r\n"
                    "Host: %s\r\n"
                    "Authorization: %s\r\n"
                    "%s"
                    "%s"
                    "Content-Length: %ld\r\n"
                    "\r\n",
                    method, path, host, auth_val,
                    have_ctype ? "" : "Content-Type: application/octet-stream\r\n",
                    extra, upload_fsize);
            } else {
                req_len = snprintf(req_head, sizeof(req_head),
                    "%s %s HTTP/1.1\r\n"
                    "Host: %s\r\n"
                    "Authorization: %s\r\n"
                    "%s"
                    "Content-Length: 0\r\n"
                    "\r\n",
                    method, path, host, auth_val, extra);
            }
            free(auth_val);
            if (req_len >= (int)sizeof(req_head)) {
                HERR("amicloud: request header too large\n");
                tls_close(ssl, sock);
                return NULL;
            }

            HLOG("http: sending %d header bytes\n", req_len);
            hdr_sent = SSL_write(ssl, req_head, req_len);
            if (hdr_sent <= 0) write_failed = 1;

            if (!write_failed && upload_data && upload_len > 0) {
                long sent = 0;
                HLOG("http: sending %ld body bytes\n", upload_len);
                while (sent < upload_len) {
                    int n = SSL_write(ssl, upload_data + sent,
                                      (int)(upload_len - sent < 16384 ? upload_len - sent : 16384));
                    if (n <= 0) { write_failed = 1; break; }
                    sent += n;
                }
                HLOG("http: body sent (%ld bytes)\n", upload_len);
            } else if (!write_failed && upload_fp && upload_fsize > 0) {
                /* Streamed from disk in fixed-size chunks instead of one big
                 * malloc'd buffer (GUI-Commander-plan section 5.3) — an 8 MB
                 * Amiga can't hold a large upload in RAM. */
                unsigned char chunk[16384];
                long sent = 0;
                HLOG("http: streaming %ld body bytes from disk\n", upload_fsize);
                while (sent < upload_fsize) {
                    size_t want = (size_t)((upload_fsize - sent) < (long)sizeof(chunk)
                                           ? (upload_fsize - sent) : (long)sizeof(chunk));
                    size_t got = fread(chunk, 1, want, upload_fp);
                    size_t off = 0;
                    if (got == 0) break;
                    while (off < got) {
                        int n = SSL_write(ssl, chunk + off, (int)(got - off));
                        if (n <= 0) { got = off; write_failed = 1; break; }
                        off += (size_t)n;
                    }
                    sent += (long)got;
                    if (!ui_progress(sent, upload_fsize, display_name)) {
                        HLOG("http: upload cancelled by progress hook\n");
                        aborted = 1;
                        break;
                    }
                    if (write_failed) break;
                }
                HLOG("http: body sent (%ld of %ld bytes)\n", sent, upload_fsize);
            }
        }

        if (aborted) {
            tls_close(ssl, sock);
            resp = calloc(1, sizeof(HttpResponse));
            if (resp) resp->aborted = 1;
            return resp;
        }

        if (write_failed) {
            tls_close(ssl, sock);
            if (reused && attempt == 0) {
                HLOG("http: reused connection was stale (write failed) -- retrying fresh\n");
                continue;
            }
            HERR("amicloud: failed to send request to %s\n", host);
            return NULL;
        }

        raw = ssl_read_response(ssl, sock, &raw_len, display_name, &aborted, &reusable);

        if (reused && attempt == 0 && !aborted && raw_len == 0) {
            HLOG("http: reused connection was stale (no data) -- retrying fresh\n");
            free(raw);
            tls_close(ssl, sock);
            continue;
        }

        if (aborted) {
            free(raw);
            tls_close(ssl, sock);
            resp = calloc(1, sizeof(HttpResponse));
            if (resp) resp->aborted = 1;
            return resp;
        }

        if (!raw) {
            HERR("amicloud: no response received from %s\n", host);
            tls_close(ssl, sock);
            return NULL;
        }

        resp = calloc(1, sizeof(HttpResponse));
        if (!resp) { free(raw); tls_close(ssl, sock); return NULL; }

        resp->status_code = parse_status(raw);
        HLOG("http: status %d, raw %lu bytes\n", resp->status_code, (unsigned long)raw_len);

        {
            const char *body_start = find_body(raw, raw_len);
            if (body_start) {
                size_t body_len = raw_len - (size_t)(body_start - raw);
                HLOG("http: body %lu bytes\n", (unsigned long)body_len);

                extract_header(raw, (size_t)(body_start - raw), "X-AmiCloud-MD5:",
                               resp->md5_header, sizeof(resp->md5_header));

                if (download_fp) {
                    fwrite(body_start, 1, body_len, download_fp);
                    if (md5ctx && body_len > 0)
                        md5_update(md5ctx, (const unsigned char *)body_start, body_len);
                } else {
                    resp->body = malloc(body_len + 1);
                    if (resp->body) {
                        memcpy(resp->body, body_start, body_len);
                        resp->body[body_len] = '\0';
                        resp->body_len = (long)body_len;
                    }
                }
            } else {
                HLOG("http: no body separator found in response\n");
            }
        }

        free(raw);

        if (reusable) conn_cache_store(host, port, ssl, sock);
        else           tls_close(ssl, sock);

        return resp;
    }

    return NULL; /* unreachable -- every path above returns or continues */
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */
HttpResponse *http_request(const char *method, const char *url,
                           const char *auth,
                           const char *upload_data, long upload_len,
                           int verify_ssl)
{
    return do_request(method, url, auth, NULL,
                      upload_data, upload_len, NULL, 0,
                      NULL, NULL, NULL, verify_ssl);
}

HttpResponse *http_request_hdr(const char *method, const char *url,
                               const char *auth, const char *extra_headers,
                               const char *upload_data, long upload_len,
                               int verify_ssl)
{
    return do_request(method, url, auth, extra_headers,
                      upload_data, upload_len, NULL, 0,
                      NULL, NULL, NULL, verify_ssl);
}

HttpResponse *http_download(const char *url, const char *auth,
                            FILE *fp, int verify_ssl, Md5Ctx *md5ctx,
                            const char *display_name)
{
    return do_request("GET", url, auth, NULL, NULL, 0, NULL, 0,
                      fp, md5ctx, display_name, verify_ssl);
}

HttpResponse *http_upload(const char *url, const char *auth,
                          FILE *fp, long fsize, int verify_ssl,
                          const char *display_name)
{
    return do_request("POST", url, auth, NULL, NULL, 0, fp, fsize,
                      NULL, NULL, display_name, verify_ssl);
}

HttpResponse *http_upload_hdr(const char *method, const char *url, const char *auth,
                              const char *extra_headers, FILE *fp, long fsize,
                              int verify_ssl, const char *display_name)
{
    return do_request(method, url, auth, extra_headers, NULL, 0, fp, fsize,
                      NULL, NULL, display_name, verify_ssl);
}

void http_response_free(HttpResponse *r)
{
    if (r) { free(r->body); free(r); }
}

/* ------------------------------------------------------------------ */
/* Init / cleanup                                                        */
/* ------------------------------------------------------------------ */
int http_init(void)
{
    HLOG("http_init: opening utility.library\n");
    UtilityBase = OpenLibrary("utility.library", 0);
    if (!UtilityBase) {
        HERR("amicloud: cannot open utility.library\n");
        return 0;
    }
    HLOG("http_init: utility.library OK\n");

    HLOG("http_init: opening bsdsocket.library\n");
    SocketBase = OpenLibrary("bsdsocket.library", 4L);
    if (!SocketBase) {
        HERR("amicloud: cannot open bsdsocket.library\n");
        CloseLibrary(UtilityBase); UtilityBase = NULL;
        return 0;
    }
    HLOG("http_init: bsdsocket.library OK\n");

    HLOG("http_init: opening amisslmaster.library\n");
    AmiSSLMasterBase = OpenLibrary("amisslmaster.library", AMISSLMASTER_MIN_VERSION);
    if (!AmiSSLMasterBase) {
        HERR("amicloud: cannot open amisslmaster.library (AmiSSL 5 required)\n");
        CloseLibrary(SocketBase);  SocketBase  = NULL;
        CloseLibrary(UtilityBase); UtilityBase = NULL;
        return 0;
    }
    HLOG("http_init: amisslmaster.library OK\n");

    HLOG("http_init: InitAmiSSLMaster\n");
    if (!InitAmiSSLMaster(AMISSL_CURRENT_VERSION, TRUE)) {
        HERR("amicloud: AmiSSL version too old\n");
        CloseLibrary(AmiSSLMasterBase); AmiSSLMasterBase = NULL;
        CloseLibrary(SocketBase);       SocketBase       = NULL;
        CloseLibrary(UtilityBase);      UtilityBase      = NULL;
        return 0;
    }
    HLOG("http_init: InitAmiSSLMaster OK\n");

    HLOG("http_init: OpenAmiSSL\n");
    AmiSSLBase = OpenAmiSSL();
    if (!AmiSSLBase) {
        HERR("amicloud: OpenAmiSSL failed\n");
        CloseLibrary(AmiSSLMasterBase); AmiSSLMasterBase = NULL;
        CloseLibrary(SocketBase);       SocketBase       = NULL;
        CloseLibrary(UtilityBase);      UtilityBase      = NULL;
        return 0;
    }
    HLOG("http_init: OpenAmiSSL OK\n");

    HLOG("http_init: InitAmiSSL\n");
    if (InitAmiSSL(AmiSSL_ErrNoPtr,   (ULONG)&errno,
                   AmiSSL_SocketBase, (ULONG)SocketBase,
                   TAG_DONE) != 0) {
        HERR("amicloud: InitAmiSSL failed\n");
        CloseAmiSSL();                  AmiSSLBase       = NULL;
        CloseLibrary(AmiSSLMasterBase); AmiSSLMasterBase = NULL;
        CloseLibrary(SocketBase);       SocketBase       = NULL;
        CloseLibrary(UtilityBase);      UtilityBase      = NULL;
        return 0;
    }
    AmiSSLInitialized = TRUE;
    HLOG("http_init: InitAmiSSL OK\n");

    HLOG("http_init: SSL_CTX_new\n");
    g_ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (!g_ssl_ctx) {
        HERR("amicloud: SSL_CTX_new failed\n");
        CleanupAmiSSLA(NULL);           AmiSSLInitialized = FALSE;
        CloseAmiSSL();                  AmiSSLBase        = NULL;
        CloseLibrary(AmiSSLMasterBase); AmiSSLMasterBase  = NULL;
        CloseLibrary(SocketBase);       SocketBase        = NULL;
        CloseLibrary(UtilityBase);      UtilityBase       = NULL;
        return 0;
    }

    /* Prevent OpenSSL from calling getenv("OPENSSL_CONF") -> GetVar() ->
     * dos.library DoPkt during SSL_connect() in packet_loop.
     * Must be called before any SSL context operations that might trigger
     * lazy config loading. */
    OPENSSL_no_config();

    /* Load CA certificates EAGERLY into the X509_STORE as X509 objects.
     * SSL_CTX_set_default_verify_paths() adds LOOKUP METHODS that call
     * getenv("SSL_CERT_FILE") / getenv("SSL_CERT_DIR") -> GetVar() ->
     * DoPkt on pr_MsgPort for EVERY SSL_connect() — even after warmup.
     * SSL_CTX_load_verify_locations() loads the certs as X509 objects now;
     * subsequent SSL_connect() finds them in the store without any DOS call. */
    {
        const char *cf = X509_get_default_cert_file();
        const char *cd = X509_get_default_cert_dir();
        HLOG("http_init: loading certs: file=%s dir=%s\n",
             cf ? cf : "(null)", cd ? cd : "(null)");
        if (SSL_CTX_load_verify_locations(g_ssl_ctx, cf, cd)) {
            HLOG("http_init: certs loaded OK (eager, no DOS in SSL_connect)\n");
        } else {
            /* Fallback: add lookup methods. These call getenv() on every
             * SSL_connect() — safe only if called before dn_Task is set.
             * With verify_ssl=0 this path is harmless (no cert check). */
            HLOG("http_init: cert load failed, using set_default_verify_paths\n");
            SSL_CTX_set_default_verify_paths(g_ssl_ctx);
        }
    }
    HLOG("http_init: done\n");
    return 1;
}

void http_cleanup(void)
{
    if (g_ssl_ctx)         { SSL_CTX_free(g_ssl_ctx);        g_ssl_ctx        = NULL; }
    if (AmiSSLInitialized) { CleanupAmiSSLA(NULL);           AmiSSLInitialized = FALSE; }
    if (AmiSSLBase)        { CloseAmiSSL();                  AmiSSLBase        = NULL; }
    if (AmiSSLMasterBase)  { CloseLibrary(AmiSSLMasterBase); AmiSSLMasterBase  = NULL; }
    if (SocketBase)        { CloseLibrary(SocketBase);       SocketBase        = NULL; }
    if (UtilityBase)       { CloseLibrary(UtilityBase);      UtilityBase       = NULL; }
}

/* Force DNS resolution + TLS cert loading before devnode->dn_Task is set.
 *
 * WHY: AmiSSL loads CA certificates lazily on the first SSL_connect() call.
 * That loading goes through dos.library (Open/Read CA cert files).  In the
 * DOSHandler, after dn_Task is set, dos.library's DoPkt() waits on our
 * pr_MsgPort for its own reply.  If Workbench simultaneously sends an
 * ACTION_DISK_INFO packet there, DoPkt gets the wrong message and calls
 * Alert(AN_DOSLib|4) -> Guru 0x87000004.
 *
 * The same applies to gethostbyname(): some TCP/IP stacks read ENVARC:sys/hosts
 * via dos.library on every call unless the result is already cached.
 *
 * Solution: call http_warmup() during backend_init(), BEFORE dn_Task is set.
 *   - tls_connect() calls gethostbyname() -> fills resolve_host's one-entry
 *     DNS cache (g_dns_cache_host / g_dns_cache_addr).
 *   - SSL_connect() triggers X509_verify_cert() which loads the CA cert store
 *     into g_ssl_ctx's X509_STORE; subsequent SSL_connect() calls reuse it.
 * After warmup, packet_loop's SSL calls need no DOS I/O. */
void http_warmup(const char *base_url, int verify_ssl)
{
    char host[256], path[8];
    int  port;
    AC_SOCKET sock = INVALID_SOCKET;
    SSL *ssl;

    if (!parse_url(base_url, host, sizeof(host), &port, path, sizeof(path))) {
        HLOG("warmup: bad URL, skipped\n");
        return;
    }
    HLOG("warmup: %s:%d (force DNS+cert cache)...\n", host, port);
    ssl = tls_connect(host, port, verify_ssl, &sock);
    if (ssl) {
        tls_close(ssl, sock);
        HLOG("warmup: OK — DNS and CA certs cached in memory\n");
    } else {
        HLOG("warmup: connect failed (non-fatal; first packet may still Guru)\n");
    }
}

#endif /* PLATFORM_AMIGA */
