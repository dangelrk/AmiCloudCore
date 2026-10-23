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
 * once against a guaranteed-fresh connection -- see exchange().
 *
 * Built for slow links (a modem at 2-5 KB/s, a 68k doing the TLS math):
 * all timeouts are idle timeouts (http_set_timeout(), default 120 s),
 * never limits on a whole transfer; Ctrl-C cancels any wait; a body that
 * breaks off is an error, never a short "success", and a GET resumes
 * where it stopped (do_request()); TLS sessions are resumed and the
 * server's keep-alive window is honoured to save handshakes.
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
#include <libraries/bsdsocket.h> /* SBTM_SETVAL/SBTC_BREAKMASK */
#include <utility/tagitem.h>
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
#include <time.h>

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

static char g_last_error[256] = "";

const char *http_last_error(void)
{
    return g_last_error;
}

/* Keep a copy of every HERR message for http_last_error(), without the
 * "amicloudcore: " prefix and the trailing newline. */
static void remember_error(const char *fmt, va_list ap)
{
    const char *p = "amicloudcore: ";
    size_t len;
    char buf[sizeof(g_last_error)];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    if (strncmp(buf, p, strlen(p)) == 0)
        memmove(buf, buf + strlen(p), strlen(buf + strlen(p)) + 1);
    len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = '\0';
    strcpy(g_last_error, buf);
}

static void herr_fn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt); remember_error(fmt, ap); va_end(ap);
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
        if (!he) { HERR("amicloudcore: cannot resolve '%s'\n", host); return 0; }
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
/* Timeouts and socket waiting                                          */
/*                                                                      */
/* Every timeout here is an IDLE timeout: it only fires when nothing at */
/* all moved for g_timeout seconds, and it restarts with every chunk    */
/* that does move. There is deliberately no limit on a whole request -- */
/* a 2 MB file at modem speed (2-5 KB/s) legitimately takes 10+ minutes.*/
/* The socket stays non-blocking from connect() to close; every TLS     */
/* call that can't proceed waits here via WaitSelect().                 */
/*                                                                      */
/* Ctrl-C: http_init() switches off bsdsocket's own break handling     */
/* (SBTC_BREAKMASK 0) and wait_sock() passes SIGBREAKF_CTRL_C as a user */
/* signal instead, so WaitSelect() reliably returns with it in the mask */
/* -- whether the stack leaves the break signal set after an EINTR      */
/* differs (Amiberry's bsdsocket emulation clears it; a Break then      */
/* looked like a timeout and the download simply resumed). It is        */
/* latched via ui_break_set(), so the next request of a recursive       */
/* get/put aborts right away as well until the program clears it        */
/* (ui_break_clear(), e.g. the shell's prompt).                         */
/* ------------------------------------------------------------------ */
static int g_timeout = AC_HTTP_DEFAULT_TIMEOUT;

/* connect() needs one round trip only; waiting the full idle timeout
 * for a server that's down is pointless. */
#define CONNECT_TIMEOUT_MAX 60

void http_set_timeout(int secs)
{
    g_timeout = secs > 0 ? secs : AC_HTTP_DEFAULT_TIMEOUT;
}

#define IO_DONE     1
#define IO_TIMEOUT  0
#define IO_ABORT   -1   /* Ctrl-C or the progress hook said stop */
#define IO_EOF     -2   /* peer closed the connection */
#define IO_ERROR   -3   /* reset, TLS error, out of memory, disk full */

/* Waits until sock is readable (for_write 0) or writable (1). Returns
 * IO_DONE, IO_TIMEOUT or IO_ABORT. */
static int wait_sock(AC_SOCKET sock, int for_write, int secs)
{
    fd_set fds;
    struct timeval tv;
    ULONG sigs = SIGBREAKF_CTRL_C;
    int sel;

    if (ui_break_pending()) return IO_ABORT;
    FD_ZERO(&fds);
    FD_SET(sock, &fds);
    tv.tv_secs  = secs;
    tv.tv_micro = 0;
    /* (void*) cast: Roadshow-struct timeval vs exec/types.h — gleiche Binaerlayout auf 68k */
    sel = WaitSelect(sock + 1, for_write ? NULL : &fds, for_write ? &fds : NULL,
                     NULL, (void *)&tv, &sigs);
    if (sel > 0) return IO_DONE;
    if ((sel == 0 && (sigs & SIGBREAKF_CTRL_C)) || ui_break_pending()) {
        HLOG("io: break signal\n");
        ui_break_set(); /* latched, see above */
        return IO_ABORT;
    }
    return IO_TIMEOUT;
}

/* After SSL_read/SSL_write/SSL_connect returned ret <= 0: waits for
 * whatever OpenSSL needs next. IO_DONE means "call it again". */
static int ssl_wait(SSL *ssl, AC_SOCKET sock, int ret)
{
    int err = SSL_get_error(ssl, ret);

    switch (err) {
    case SSL_ERROR_WANT_READ:   return wait_sock(sock, 0, g_timeout);
    case SSL_ERROR_WANT_WRITE:  return wait_sock(sock, 1, g_timeout);
    case SSL_ERROR_ZERO_RETURN: return IO_EOF;
    case SSL_ERROR_SYSCALL:     if (ret == 0) return IO_EOF;
                                return ui_break_pending() ? IO_ABORT : IO_ERROR;
    default:
        HLOG("io: SSL error %d\n", err);
        return IO_ERROR;
    }
}

/* Writes all len bytes. Without SSL_MODE_ENABLE_PARTIAL_WRITE a
 * successful SSL_write() always takes everything; a WANT_WRITE retry
 * must repeat the call with the same buffer and length (SSL_write(3)),
 * which this loop does. */
static int ssl_write_all(SSL *ssl, AC_SOCKET sock, const char *buf, int len)
{
    while (len > 0) {
        int n, r;
        ERR_clear_error();
        n = SSL_write(ssl, buf, len);
        if (n > 0) { buf += n; len -= n; continue; }
        r = ssl_wait(ssl, sock, n);
        if (r != IO_DONE) return r;
    }
    return IO_DONE;
}

static const char *io_text(int io)
{
    switch (io) {
    case IO_TIMEOUT: return "timed out";
    case IO_ABORT:   return "cancelled";
    case IO_EOF:     return "connection closed by server";
    default:         return "connection error";
    }
}

/* ------------------------------------------------------------------ */
/* TLS session resumption                                               */
/*                                                                      */
/* A full handshake (certificate chain verification, key exchange) costs*/
/* 10-60 s of 68k CPU time; resuming a session skips the certificate    */
/* part. One remembered session is enough -- a process only ever talks  */
/* to the one server in its config. Keyed by the SNI host name.         */
/* ------------------------------------------------------------------ */
static SSL_SESSION *g_sess = NULL;
static char         g_sess_host[256] = "";

/* Called right before a connection is freed or parked: with TLS 1.3 the
 * session ticket only arrives after the handshake, so only now is
 * there something resumable to keep. */
static void sess_remember(SSL *ssl)
{
    const char  *host = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
    SSL_SESSION *s;

    if (!host) return;
    s = SSL_get1_session(ssl);
    if (!s) return;
    if (!SSL_SESSION_is_resumable(s)) { SSL_SESSION_free(s); return; }
    if (g_sess) SSL_SESSION_free(g_sess);
    g_sess = s;
    strncpy(g_sess_host, host, sizeof(g_sess_host) - 1);
    g_sess_host[sizeof(g_sess_host) - 1] = '\0';
}

static void sess_forget(void)
{
    if (g_sess) SSL_SESSION_free(g_sess);
    g_sess = NULL;
    g_sess_host[0] = '\0';
}

/* ------------------------------------------------------------------ */
/* Low-level: open TCP socket + TLS, return SSL* or NULL on error.      */
/* *io_out tells why on NULL (IO_ABORT for Ctrl-C, else a failure).    */
/* ------------------------------------------------------------------ */
static SSL *tls_connect(const char *host, int port, int verify,
                        AC_SOCKET *sock_out, int *io_out)
{
    struct sockaddr_in addr;
    AC_SOCKET sock;
    SSL      *ssl;
    int       r;

    *io_out = IO_ERROR;
    HLOG("tls: resolving '%s'...\n", host);
    if (!resolve_host(host, &addr)) {
        HERR("amicloudcore: cannot resolve '%s'\n", host);
        return NULL;
    }
    HLOG("tls: resolved OK\n");
    addr.sin_port = htons((unsigned short)port);

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        HERR("amicloudcore: socket() failed\n");
        return NULL;
    }
    HLOG("tls: socket fd=%ld\n", (long)sock);

    /* Non-blocking for good: connect, handshake and all data transfer
     * wait in WaitSelect() (see wait_sock()), never in the stack. */
    {
        LONG nb = 1;
        IoctlSocket(sock, FIONBIO, (char *)&nb);
    }

    HLOG("tls: connect() to %s:%d...\n", host, port);
    connect(sock, (struct sockaddr *)&addr, sizeof(addr)); /* EINPROGRESS expected */

    {
        int secs = g_timeout < CONNECT_TIMEOUT_MAX ? g_timeout : CONNECT_TIMEOUT_MAX;
        int soerr;
        LONG soerr_len;  /* LONG* — Roadshow getsockopt erwartet LONG-Laenge */

        r = wait_sock(sock, 1, secs);
        if (r != IO_DONE) {
            if (r == IO_TIMEOUT)
                HERR("amicloudcore: connect to %s:%d timed out (%d s)\n", host, port, secs);
            CloseSocket(sock);
            *io_out = r;
            return NULL;
        }
        soerr = 0; soerr_len = (LONG)sizeof(soerr);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&soerr, (void *)&soerr_len);
        if (soerr != 0) {
            HERR("amicloudcore: connect to %s:%d failed (err %d)\n", host, port, soerr);
            CloseSocket(sock);
            return NULL;
        }
        HLOG("tls: connect OK\n");
    }

    ssl = SSL_new(g_ssl_ctx);
    if (!ssl) {
        HERR("amicloudcore: SSL_new failed\n");
        CloseSocket(sock);
        return NULL;
    }

    SSL_set_verify(ssl, verify ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, NULL);
    SSL_set_fd(ssl, (int)sock);
    SSL_set_tlsext_host_name(ssl, host); /* SNI */
    if (g_sess && strcmp(g_sess_host, host) == 0 && SSL_SESSION_is_resumable(g_sess))
        SSL_set_session(ssl, g_sess);
    HLOG("tls: SSL_connect (verify=%d, SNI=%s, session=%s)...\n",
         verify, host, g_sess ? "offered" : "none");

    for (;;) {
        int n;
        ERR_clear_error();
        n = SSL_connect(ssl);
        if (n == 1) break;
        r = ssl_wait(ssl, sock, n);
        if (r != IO_DONE) {
            if (r == IO_TIMEOUT)
                HERR("amicloudcore: TLS handshake with %s timed out (no data for %d s)\n", host, g_timeout);
            else if (r != IO_ABORT)
                HERR("amicloudcore: TLS handshake with %s failed\n", host);
            /* A rejected or broken resumption must not poison the next try. */
            if (r != IO_ABORT) sess_forget();
            SSL_free(ssl);
            CloseSocket(sock);
            *io_out = r;
            return NULL;
        }
    }
    HLOG("tls: SSL_connect OK, cipher=%s, resumed=%d\n",
         SSL_get_cipher(ssl), (int)SSL_session_reused(ssl));

    *sock_out = sock;
    *io_out = IO_DONE;
    return ssl;
}

static void tls_close(SSL *ssl, AC_SOCKET sock)
{
    if (ssl) {
        /* Shutdown first: SSL_free() without it marks the session as not
         * resumable. Non-blocking, so this sends close_notify at most --
         * nobody waits for the server's answer. */
        SSL_shutdown(ssl);
        sess_remember(ssl);
        SSL_free(ssl);
    }
    if (sock != INVALID_SOCKET) CloseSocket(sock);
}

/* ------------------------------------------------------------------ */
/* Single-slot persistent-connection cache (see file header comment).   */
/* ------------------------------------------------------------------ */
static char      g_conn_host[256] = "";
static int       g_conn_port      = 0;
static AC_SOCKET g_conn_sock      = INVALID_SOCKET;
static SSL      *g_conn_ssl       = NULL;
/* time() after which the server will have dropped the idle connection
 * (from its "Keep-Alive: timeout=N" header), 0 = unknown. Reusing a
 * connection the server already closed costs a full request timeout in
 * the worst case, so a known-dead one is never tried. */
static long      g_conn_deadline  = 0;

/* A request needs a moment to reach the server over a slow link; don't
 * bet on the last seconds of its keep-alive window. */
#define KEEPALIVE_MARGIN 2

static void conn_cache_close(void)
{
    if (g_conn_ssl) {
        HLOG("conn: closing cached connection to %s:%d\n", g_conn_host, g_conn_port);
        tls_close(g_conn_ssl, g_conn_sock);
    }
    g_conn_ssl      = NULL;
    g_conn_sock     = INVALID_SOCKET;
    g_conn_host[0]  = '\0';
    g_conn_deadline = 0;
}

/* Discards whatever's currently cached (closing it if it's a different,
 * still-open connection) and stores ssl/sock as the new cached
 * connection for host:port. keepalive_secs is the server's announced
 * idle timeout (0 = unknown). Caller must only call this with a
 * connection it has just finished using and confirmed reusable (see
 * do_request()) -- this function itself does no framing validation. */
static void conn_cache_store(const char *host, int port, SSL *ssl, AC_SOCKET sock,
                             long keepalive_secs)
{
    conn_cache_close();
    if (keepalive_secs > 0 && keepalive_secs <= KEEPALIVE_MARGIN) {
        HLOG("conn: keep-alive window %lds too short, closing\n", keepalive_secs);
        tls_close(ssl, sock);
        return;
    }
    strncpy(g_conn_host, host, sizeof(g_conn_host) - 1);
    g_conn_host[sizeof(g_conn_host) - 1] = '\0';
    g_conn_port = port;
    g_conn_ssl  = ssl;
    g_conn_sock = sock;
    g_conn_deadline = keepalive_secs > 0
                    ? (long)time(NULL) + keepalive_secs - KEEPALIVE_MARGIN : 0;
    sess_remember(ssl);
    HLOG("conn: cached connection to %s:%d for reuse (keep-alive %lds)\n",
         host, port, keepalive_secs);
}

/* Removes and returns the cached connection if it matches host:port
 * and hasn't outlived the server's keep-alive window (cache slot is
 * empty afterward either way -- a caller that decides not to use a hit
 * must tls_close() it itself, never leave it half-taken). Returns 1 on
 * a hit (ssl/sock filled in), 0 on a miss. */
static int conn_cache_take(const char *host, int port, SSL **ssl, AC_SOCKET *sock)
{
    if (g_conn_ssl && g_conn_deadline && (long)time(NULL) >= g_conn_deadline) {
        HLOG("conn: cached connection past its keep-alive window\n");
        conn_cache_close();
    }
    if (g_conn_ssl && g_conn_port == port && strcmp(g_conn_host, host) == 0) {
        HLOG("conn: reusing cached connection to %s:%d\n", host, port);
        *ssl  = g_conn_ssl;
        *sock = g_conn_sock;
        g_conn_ssl      = NULL;
        g_conn_sock     = INVALID_SOCKET;
        g_conn_host[0]  = '\0';
        g_conn_deadline = 0;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Reading one HTTP response                                            */
/* ------------------------------------------------------------------ */
#define READ_CHUNK 4096

/* find_body()/extract_header()/ci_eqn()/parse_status() are defined
 * further down this file, but ssl_read_response() needs them early (as
 * soon as the header block is fully received) to learn Content-Length/
 * Transfer-Encoding/Connection before the body finishes. */
static const char *find_body(const char *resp, size_t len);
static void extract_header(const char *headers, size_t headers_len,
                           const char *name, char *out, size_t outsz);
static int ci_eqn(const char *a, const char *b, size_t n);
static int parse_status(const char *resp);

/* State of one response being read. buf holds the header block and,
 * unless the body is streamed to fp, the body behind it. */
typedef struct {
    SSL        *ssl;
    AC_SOCKET   sock;
    char       *buf;
    size_t      size;
    size_t      cap;            /* allocated bytes at buf (>= size + 1) */
    size_t      header_len;     /* 0 until the header block is complete */
    int         status;
    long        total_body;     /* Content-Length, -1 if none */
    int         chunked;
    int         conn_close;
    long        keepalive;      /* "Keep-Alive: timeout=N", 0 if absent */
    long        expected;       /* body length for progress: Content-Length,
                                 * else X-AmiCloud-Length (survives a proxy
                                 * that turns the body into chunks), -1 */
    size_t      dec_end;        /* chunked: end of the decoded part in buf */
    /* Body sink: a 2xx body goes straight to fp (and md5) as it arrives
     * instead of piling up in RAM -- an 8 MB Amiga can't hold a large
     * download twice. Other statuses (error JSON) stay in buf. */
    FILE       *fp;
    Md5Ctx     *md5;
    int         streaming;
    long        flushed;        /* body bytes already handed to fp */
    long        progress_base;  /* bytes delivered by earlier resumed parts */
    const char *display_name;
} ReadCtx;

/* Body bytes received so far, wherever they went. */
static long body_received(const ReadCtx *c)
{
    return c->flushed + (c->header_len ? (long)(c->size - c->header_len) : 0);
}

/* Hands buf[header_len..size) to the sink and drops it from buf. */
static int flush_body(ReadCtx *c)
{
    size_t n = c->size - c->header_len;
    if (!c->streaming || n == 0) return IO_DONE;
    if (fwrite(c->buf + c->header_len, 1, n, c->fp) != n) {
        HERR("amicloudcore: cannot write downloaded data (disk full?)\n");
        return IO_ERROR;
    }
    if (c->md5) md5_update(c->md5, (const unsigned char *)c->buf + c->header_len, n);
    c->flushed += (long)n;
    c->size = c->header_len;
    c->buf[c->size] = '\0';
    return IO_DONE;
}

/* Makes room for need bytes. Grows geometrically: a GET into memory
 * used to realloc() per 4 KB read, which for an 8 MB file meant some
 * 2000 reallocations copying gigabytes in total -- minutes on a 68030. */
static int reserve(ReadCtx *c, size_t need)
{
    size_t ncap;
    char *nb;

    if (need <= c->cap) return 1;
    ncap = c->cap ? c->cap : 16384;
    while (ncap < need) ncap *= 2;
    nb = realloc(c->buf, ncap);
    if (!nb) {
        /* Doubling may overshoot what's left; try the exact size. */
        ncap = need;
        nb = realloc(c->buf, ncap);
        if (!nb) { HERR("amicloudcore: out of memory\n"); return 0; }
    }
    c->buf = nb;
    c->cap = ncap;
    return 1;
}

/* One SSL_read() (waiting as long as needed, see wait_sock()) appended
 * to c->buf, then a progress report. Returns IO_DONE if >= 1 byte
 * arrived, otherwise the reason it didn't. */
static int read_more(ReadCtx *c)
{
    char tmp[READ_CHUNK];
    for (;;) {
        int n, r;
        ERR_clear_error();
        n = SSL_read(c->ssl, tmp, READ_CHUNK);
        if (n > 0) {
            long total;
            if (!reserve(c, c->size + (size_t)n + 1)) return IO_ERROR;
            memcpy(c->buf + c->size, tmp, (size_t)n);
            c->size += (size_t)n;
            c->buf[c->size] = '\0';
            HLOG("read: +%d bytes (total %lu)\n", n, (unsigned long)c->size);
            total = (c->header_len && c->expected >= 0) ? c->progress_base + c->expected : -1;
            if (!ui_progress(c->progress_base + body_received(c), total, c->display_name)) {
                HLOG("read: cancelled by progress hook\n");
                return IO_ABORT;
            }
            return IO_DONE;
        }
        r = ssl_wait(c->ssl, c->sock, n);
        if (r != IO_DONE) {
            HLOG("read: stopped after %lu bytes: %s\n", (unsigned long)c->size, io_text(r));
            return r;
        }
    }
}

/* No Content-Length, no chunking: the body ends when the server closes
 * the connection. Only a clean close counts as complete -- a timeout or
 * reset means the body is cut short. Never reusable afterward. */
static int read_body_until_close(ReadCtx *c)
{
    for (;;) {
        int r = flush_body(c);
        if (r != IO_DONE) return r;
        r = read_more(c);
        if (r == IO_EOF) return flush_body(c);
        if (r != IO_DONE) return r;
    }
}

/* Reads until exactly total_body bytes of body arrived. IO_DONE means
 * complete (connection may be reused afterward); anything else means
 * the body is incomplete and the connection is no longer at a response
 * boundary. */
static int read_body_exact(ReadCtx *c)
{
    for (;;) {
        int r = flush_body(c);
        if (r != IO_DONE) return r;
        if (body_received(c) >= c->total_body) return IO_DONE;
        r = read_more(c);
        if (r != IO_DONE) return r;
    }
}

/* Decodes an HTTP/1.1 chunked body IN PLACE within c->buf, starting
 * right after the header block -- removes each chunk-size line and its
 * trailing CRLF as it goes, so the buffer ends up holding a plain body
 * exactly like a Content-Length response would. When streaming, every
 * decoded chunk goes to the sink right away and the raw remainder moves
 * back down to the end of the header block. IO_DONE means the 0-length
 * terminator chunk and trailer were consumed (connection reusable). */
static int chunked_decode(ReadCtx *c)
{
    size_t cursor = c->header_len; /* unparsed raw wire bytes start here */

    c->dec_end = c->header_len;    /* decoded body is compacted down to here */

    for (;;) {
        char *eol;
        int   r;

        while ((eol = (char *)memchr(c->buf + cursor, '\n', c->size - cursor)) == NULL) {
            r = read_more(c);
            if (r != IO_DONE) return r;
        }

        {
            long  chunk_size;
            char *p;
            chunk_size = strtol(c->buf + cursor, &p, 16); /* stops at ';ext' or '\r' on its own */
            if (p == c->buf + cursor || chunk_size < 0) {
                HERR("amicloudcore: malformed chunk size\n");
                return IO_ERROR;
            }
            cursor = (size_t)(eol - c->buf) + 1; /* past the size line's terminating \n */

            if (chunk_size == 0) {
                /* Terminator chunk -- consume trailer headers (almost
                 * always none) up to and including the final blank line. */
                for (;;) {
                    if (cursor + 1 < c->size && c->buf[cursor] == '\r' && c->buf[cursor + 1] == '\n') {
                        cursor += 2;
                        break;
                    }
                    {
                        const char *tail_end = find_body(c->buf + cursor, c->size - cursor);
                        if (tail_end) { cursor = (size_t)(tail_end - c->buf); break; }
                    }
                    r = read_more(c);
                    /* A clean shutdown right after "0\r\n" (no final CRLF
                     * byte) is common enough server behaviour to tolerate
                     * -- the body itself is complete at this point. */
                    if (r == IO_EOF) break;
                    if (r != IO_DONE) return r;
                }
                c->size = c->dec_end;
                c->buf[c->size] = '\0';
                return flush_body(c);
            }

            while (c->size < cursor + (size_t)chunk_size + 2) {
                r = read_more(c);
                if (r != IO_DONE) return r;
            }

            if (c->dec_end != cursor) memmove(c->buf + c->dec_end, c->buf + cursor, (size_t)chunk_size);
            c->dec_end  += (size_t)chunk_size;
            cursor += (size_t)chunk_size + 2; /* data + its trailing \r\n */

            if (c->streaming) {
                /* Sink the decoded part, keep the undecoded rest. */
                size_t rest = c->size - cursor;
                c->size = c->dec_end;
                r = flush_body(c);
                if (r != IO_DONE) return r;
                memmove(c->buf + c->header_len, c->buf + cursor, rest);
                c->size = c->header_len + rest;
                cursor = c->header_len;
                c->dec_end  = c->header_len;
            }
        }
    }
}

/* On any failure, cut the buffer back to the decoded part: a broken-off
 * chunked body then counts exactly the bytes that are really there
 * (body_received()), so a GET can resume from that offset like a
 * Content-Length one -- many servers send chunked (PHP-FPM behind
 * Apache, Nextcloud). */
static int read_body_chunked(ReadCtx *c)
{
    int r = chunked_decode(c);
    if (r != IO_DONE && c->size > c->dec_end) {
        c->size = c->dec_end;
        c->buf[c->size] = '\0';
    }
    return r;
}

/* Reads one full HTTP response (headers + framing-appropriate body).
 * Returns IO_DONE when it arrived complete; otherwise c->header_len
 * tells whether at least the header block made it (0 = nothing usable).
 * *reusable_out tells do_request() whether ssl/sock are still at a clean
 * response boundary and safe to hand to conn_cache_store(). */
static int ssl_read_response(ReadCtx *c, int *reusable_out)
{
    int r;

    *reusable_out = 0;
    HLOG("read: waiting for response...\n");

    /* Phase 1: headers. */
    for (;;) {
        const char *body_start;
        r = read_more(c);
        if (r != IO_DONE) return r;
        body_start = find_body(c->buf, c->size);
        if (body_start) {
            char clbuf[32], tebuf[64], connbuf[32], kabuf[64];
            const char *t;
            c->header_len = (size_t)(body_start - c->buf);
            c->status = parse_status(c->buf);
            extract_header(c->buf, c->header_len, "Content-Length:", clbuf, sizeof(clbuf));
            c->total_body = clbuf[0] ? atol(clbuf) : -1;
            extract_header(c->buf, c->header_len, "Transfer-Encoding:", tebuf, sizeof(tebuf));
            c->chunked = (tebuf[0] && strstr(tebuf, "chunked") != NULL);
            extract_header(c->buf, c->header_len, "Connection:", connbuf, sizeof(connbuf));
            c->conn_close = (connbuf[0] && ci_eqn(connbuf, "close", 5));
            extract_header(c->buf, c->header_len, "Keep-Alive:", kabuf, sizeof(kabuf));
            t = strstr(kabuf, "timeout=");
            c->keepalive = t ? atol(t + 8) : 0;
            if (c->chunked) c->total_body = -1;
            c->expected = c->total_body;
            if (c->expected < 0) {
                char lenbuf[32];
                extract_header(c->buf, c->header_len, "X-AmiCloud-Length:", lenbuf, sizeof(lenbuf));
                if (lenbuf[0]) c->expected = atol(lenbuf);
            }
            /* No body by definition (RFC 9110 6.4.1) -- without this a
             * 204 to a DELETE would wait for the server to close. */
            if (c->status == 204 || c->status == 304 || (c->status >= 100 && c->status < 200)) {
                c->chunked = 0;
                c->total_body = 0;
                c->expected = 0;
            }
            c->streaming = (c->fp && c->status >= 200 && c->status < 300);
            /* Body kept in memory and its length known: one allocation. */
            if (!c->streaming && c->expected > 0
                && !reserve(c, c->header_len + (size_t)c->expected + 1))
                return IO_ERROR;
            HLOG("read: headers complete (%lu bytes), status=%d, Content-Length=%ld, chunked=%d, "
                 "conn_close=%d, keep-alive=%ld\n",
                 (unsigned long)c->header_len, c->status, c->total_body, c->chunked,
                 c->conn_close, c->keepalive);
            break;
        }
    }

    /* Phase 2: body, framed according to what phase 1 found. */
    if (c->chunked) {
        r = read_body_chunked(c);
        *reusable_out = (r == IO_DONE) && !c->conn_close;
    } else if (c->total_body >= 0) {
        r = read_body_exact(c);
        *reusable_out = (r == IO_DONE) && !c->conn_close;
    } else {
        r = read_body_until_close(c);
        *reusable_out = 0; /* the close is how we knew we were done -- nothing left to reuse */
    }

    HLOG("read: done, %ld body bytes, %s, reusable=%d\n",
         body_received(c), r == IO_DONE ? "complete" : io_text(r), *reusable_out);
    return r;
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

/* Pick "<name> <value>" out of the raw header block (headers_len bytes
 * starting at headers — everything before the \r\n\r\n body separator).
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
/* One request/response exchange                                        */
/* ------------------------------------------------------------------ */

/* Sends the request on a cached or fresh connection and reads the
 * response into *c (see ReadCtx). Returns the ssl_read_response()
 * result, or the reason no response could be read at all (then
 * c->header_len stays 0).
 *
 * At most 2 attempts: a connection taken from the cache (attempt 0)
 * might have been closed by the server in the meantime even though its
 * keep-alive window looked fine -- that shows as an immediate close or
 * reset while sending, or as a close before a single response byte
 * arrived. Then attempt 1 retries the whole request once on a fresh
 * connection. A *timeout* is never retried: it means the server or the
 * line is slow, not that the connection was stale, and the server may
 * well be executing the request already (a repeated MOVE/DELETE/upload
 * would then run twice). */
static int exchange(const char *method, const char *host, int port, const char *path,
                    const char *auth_val, const char *extra,
                    const char *upload_data, long upload_len,
                    FILE *upload_fp, long upload_fsize, long upload_fp_start,
                    long range_from, int verify_ssl, ReadCtx *c)
{
    int attempt;

    for (attempt = 0; attempt < 2; attempt++) {
        AC_SOCKET sock = INVALID_SOCKET;
        SSL      *ssl  = NULL;
        int       reused = 0;
        int       reusable = 0;
        int       r;
        char      req_head[AC_MAX_URL + 1024]; /* path up to AC_MAX_URL + headers */
        char      range[48];
        long      body_len;
        int       req_len;
        /* Eigener Content-Type in extra_headers ersetzt den Default */
        int have_ctype = (strstr(extra, "Content-Type:") != NULL ||
                          strstr(extra, "content-type:") != NULL);

        if (attempt == 0 && conn_cache_take(host, port, &ssl, &sock)) {
            reused = 1;
        } else {
            ssl = tls_connect(host, port, verify_ssl, &sock, &r);
            if (!ssl) return r;
        }

        if (upload_fp) fseek(upload_fp, upload_fp_start, SEEK_SET);

        body_len = (upload_data && upload_len > 0) ? upload_len
                 : (upload_fp && upload_fsize > 0) ? upload_fsize : 0;
        range[0] = '\0';
        if (range_from > 0) snprintf(range, sizeof(range), "Range: bytes=%ld-\r\n", range_from);

        /* HTTP/1.1, no explicit Connection header -- persistent by
         * default (see this file's header comment); the server tells
         * us via its OWN "Connection: close" response header if it
         * intends to close anyway (checked in ssl_read_response()). */
        req_len = snprintf(req_head, sizeof(req_head),
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Authorization: %s\r\n"
            "%s"
            "%s"
            "%s"
            "Content-Length: %ld\r\n"
            "\r\n",
            method, path, host, auth_val, range,
            (body_len > 0 && !have_ctype) ? "Content-Type: application/octet-stream\r\n" : "",
            extra, body_len);
        if (req_len >= (int)sizeof(req_head)) {
            HERR("amicloudcore: request header too large\n");
            tls_close(ssl, sock);
            return IO_ERROR;
        }

        HLOG("http: sending %d header bytes\n", req_len);
        r = ssl_write_all(ssl, sock, req_head, req_len);

        if (r == IO_DONE && upload_data && upload_len > 0) {
            long sent = 0;
            HLOG("http: sending %ld body bytes\n", upload_len);
            while (sent < upload_len) {
                int n = (int)(upload_len - sent < 16384 ? upload_len - sent : 16384);
                r = ssl_write_all(ssl, sock, upload_data + sent, n);
                if (r != IO_DONE) break;
                sent += n;
                /* Small bodies (JSON, a tiny file) aren't worth a progress line. */
                if (upload_len > 16384 && !ui_progress(sent, upload_len, c->display_name)) {
                    r = IO_ABORT;
                    break;
                }
            }
            HLOG("http: body sent (%ld of %ld bytes)\n", sent, upload_len);
        } else if (r == IO_DONE && upload_fp && upload_fsize > 0) {
            /* Streamed from disk in fixed-size chunks instead of one big
             * malloc'd buffer (GUI-Commander-plan section 5.3) — an 8 MB
             * Amiga can't hold a large upload in RAM. */
            static char chunk[16384]; /* static: keeps 16 KB off a 64 KB stack */
            long sent = 0;
            HLOG("http: streaming %ld body bytes from disk\n", upload_fsize);
            while (sent < upload_fsize) {
                size_t want = (size_t)((upload_fsize - sent) < (long)sizeof(chunk)
                                       ? (upload_fsize - sent) : (long)sizeof(chunk));
                size_t got = fread(chunk, 1, want, upload_fp);
                if (got == 0) {
                    HERR("amicloudcore: cannot read upload data\n");
                    r = IO_ERROR;
                    break;
                }
                r = ssl_write_all(ssl, sock, chunk, (int)got);
                if (r != IO_DONE) break;
                sent += (long)got;
                if (!ui_progress(sent, upload_fsize, c->display_name)) { r = IO_ABORT; break; }
            }
            HLOG("http: body sent (%ld of %ld bytes)\n", sent, upload_fsize);
        }

        if (r != IO_DONE) {
            tls_close(ssl, sock);
            if (reused && attempt == 0 && (r == IO_EOF || r == IO_ERROR)) {
                HLOG("http: reused connection was stale (send failed) -- retrying fresh\n");
                continue;
            }
            if (r == IO_TIMEOUT)
                HERR("amicloudcore: sending to %s timed out (no progress for %d s)\n", host, g_timeout);
            else if (r != IO_ABORT)
                HERR("amicloudcore: failed to send request to %s (%s)\n", host, io_text(r));
            return r;
        }

        c->ssl  = ssl;
        c->sock = sock;
        r = ssl_read_response(c, &reusable);

        if (c->header_len == 0) {
            int nothing = (c->size == 0);
            free(c->buf);
            c->buf  = NULL;
            c->size = 0;
            c->cap  = 0;
            tls_close(ssl, sock);
            if (reused && attempt == 0 && nothing && (r == IO_EOF || r == IO_ERROR)) {
                HLOG("http: reused connection was stale (closed before answering) -- retrying fresh\n");
                continue;
            }
            if (r == IO_TIMEOUT)
                HERR("amicloudcore: no response from %s within %d s\n", host, g_timeout);
            else if (r != IO_ABORT)
                HERR("amicloudcore: no response received from %s (%s)\n", host, io_text(r));
            return r == IO_DONE ? IO_ERROR : r;
        }

        if (reusable) conn_cache_store(host, port, ssl, sock, c->keepalive);
        else          tls_close(ssl, sock);
        return r;
    }

    return IO_ERROR; /* unreachable -- every path above returns or continues */
}

/* ------------------------------------------------------------------ */
/* Core request function                                                */
/*                                                                      */
/* A GET whose body breaks off (timeout, reset) is resumed with a Range */
/* request from where it stopped, up to MAX_RESUMES times as long as   */
/* each try made progress -- instead of failing a 95 % finished modem   */
/* download outright. Works for chunked bodies too (only fully decoded */
/* bytes count, see read_body_chunked()) and only if the server answers*/
/* 206; the AmiCloud server and every WebDAV server do. The MD5 header */
/* of the first response still covers the whole file, and the streamed*/
/* MD5 context simply continues, so callers verify exactly as before.  */
/*                                                                      */
/* Result: NULL if no response header arrived at all, otherwise an     */
/* HttpResponse. status_code 0 means the transfer failed part-way      */
/* (body incomplete) -- callers already treat 0 as a network error.    */
/* ------------------------------------------------------------------ */
#define MAX_RESUMES 5

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
    const char *extra = (extra_headers && *extra_headers) ? extra_headers : "";
    int  can_resume = (strcmp(method, "GET") == 0 && !upload_data && !upload_fp);
    char *auth_val;
    HttpResponse *resp;
    char *body = NULL;      /* in-memory body, collected across resumed parts */
    long  body_len = 0;
    long  delivered = 0;    /* body bytes delivered so far (memory or file) */
    int   resumes;

    HLOG("http: %s %s\n", method, url);

    if (!parse_url(url, host, sizeof(host), &port, path, sizeof(path))) {
        HERR("amicloudcore: invalid URL: %s\n", url);
        return NULL;
    }
    HLOG("http: host=%s port=%d path=%s\n", host, port, path);

    /* Ctrl-C from an earlier transfer still pending: a recursive get/put
     * stops here instead of starting the next file. */
    if (ui_break_pending()) {
        resp = calloc(1, sizeof(HttpResponse));
        if (resp) resp->aborted = 1;
        return resp;
    }

    auth_val = make_basic_auth(auth);
    if (!auth_val) return NULL;
    resp = calloc(1, sizeof(HttpResponse));
    if (!resp) { free(auth_val); return NULL; }

    for (resumes = 0; ; resumes++) {
        ReadCtx c;
        int r;
        long got;

        memset(&c, 0, sizeof(c));
        c.fp            = download_fp;
        c.md5           = md5ctx;
        c.display_name  = display_name;
        c.progress_base = delivered;

        r = exchange(method, host, port, path, auth_val, extra,
                     upload_data, upload_len, upload_fp, upload_fsize, upload_fp_start,
                     resumes ? delivered : 0, verify_ssl, &c);

        if (c.header_len == 0) {
            /* Nothing usable came back from this try. */
            if (r == IO_ABORT) resp->aborted = 1;
            else if (resumes == 0) { free(auth_val); free(resp); return NULL; }
            resp->status_code = 0;
            break;
        }

        if (resumes == 0) {
            resp->status_code = c.status;
            extract_header(c.buf, c.header_len, "X-AmiCloud-MD5:",
                           resp->md5_header, sizeof(resp->md5_header));
        } else if (c.status != 206) {
            HERR("amicloudcore: server answered HTTP %d to a resume request\n", c.status);
            free(c.buf);
            resp->status_code = 0;
            break;
        }

        got = body_received(&c);
        if (!c.streaming && c.size > c.header_len) {
            /* Kept in memory (no fp, or a non-2xx answer): append. */
            long n = (long)(c.size - c.header_len);
            if (!body) {
                /* First part: take over the read buffer instead of a
                 * second full-size copy. */
                memmove(c.buf, c.buf + c.header_len, (size_t)n);
                c.buf[n] = '\0';
                body = c.buf;
                body_len = n;
                c.buf = NULL;
            } else {
                char *nb = realloc(body, (size_t)(body_len + n + 1));
                if (!nb) {
                    HERR("amicloudcore: out of memory\n");
                    free(c.buf);
                    resp->status_code = 0;
                    break;
                }
                body = nb;
                memcpy(body + body_len, c.buf + c.header_len, (size_t)n);
                body_len += n;
                body[body_len] = '\0';
            }
        }
        delivered += got;
        free(c.buf);

        if (r == IO_DONE) break;
        if (r == IO_ABORT) { resp->aborted = 1; resp->status_code = 0; break; }

        if (can_resume && resp->status_code >= 200 && resp->status_code < 300
            && got > 0 && resumes < MAX_RESUMES) {
            HERR("amicloudcore: connection to %s lost (%s) after %ld bytes -- resuming\n",
                 host, io_text(r), delivered);
            continue;
        }
        HERR("amicloudcore: transfer from %s incomplete (%s) after %ld bytes\n",
             host, io_text(r), delivered);
        resp->status_code = 0;
        break;
    }
    free(auth_val);

    /* With download_fp a 2xx body went to the file; body only holds a
     * non-2xx answer then (error JSON), handy for the caller's message. */
    resp->body     = body;
    resp->body_len = body_len;
    HLOG("http: status %d, %ld body bytes\n", resp->status_code, delivered);
    return resp;
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
    g_last_error[0] = '\0';
    HLOG("http_init: opening utility.library\n");
    UtilityBase = OpenLibrary("utility.library", 0);
    if (!UtilityBase) {
        HERR("amicloudcore: cannot open utility.library\n");
        return 0;
    }
    HLOG("http_init: utility.library OK\n");

    HLOG("http_init: opening bsdsocket.library\n");
    SocketBase = OpenLibrary("bsdsocket.library", 4L);
    if (!SocketBase) {
        HERR("amicloudcore: cannot open bsdsocket.library\n");
        CloseLibrary(UtilityBase); UtilityBase = NULL;
        return 0;
    }
    HLOG("http_init: bsdsocket.library OK\n");
    {
        /* Ctrl-C is handled by wait_sock() itself, see there. */
        struct TagItem tags[2];
        tags[0].ti_Tag  = SBTM_SETVAL(SBTC_BREAKMASK);
        tags[0].ti_Data = 0;
        tags[1].ti_Tag  = TAG_DONE;
        tags[1].ti_Data = 0;
        SocketBaseTagList(tags);
    }

    HLOG("http_init: opening amisslmaster.library\n");
    AmiSSLMasterBase = OpenLibrary("amisslmaster.library", AMISSLMASTER_MIN_VERSION);
    if (!AmiSSLMasterBase) {
        HERR("amicloudcore: cannot open amisslmaster.library (AmiSSL 5 required)\n");
        CloseLibrary(SocketBase);  SocketBase  = NULL;
        CloseLibrary(UtilityBase); UtilityBase = NULL;
        return 0;
    }
    HLOG("http_init: amisslmaster.library OK\n");

    HLOG("http_init: InitAmiSSLMaster\n");
    if (!InitAmiSSLMaster(AMISSL_CURRENT_VERSION, TRUE)) {
        HERR("amicloudcore: AmiSSL version too old\n");
        CloseLibrary(AmiSSLMasterBase); AmiSSLMasterBase = NULL;
        CloseLibrary(SocketBase);       SocketBase       = NULL;
        CloseLibrary(UtilityBase);      UtilityBase      = NULL;
        return 0;
    }
    HLOG("http_init: InitAmiSSLMaster OK\n");

    HLOG("http_init: OpenAmiSSL\n");
    AmiSSLBase = OpenAmiSSL();
    if (!AmiSSLBase) {
        HERR("amicloudcore: OpenAmiSSL failed\n");
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
        HERR("amicloudcore: InitAmiSSL failed\n");
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
        HERR("amicloudcore: SSL_CTX_new failed\n");
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
    conn_cache_close();
    sess_forget();
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
    int  io;

    if (!parse_url(base_url, host, sizeof(host), &port, path, sizeof(path))) {
        HLOG("warmup: bad URL, skipped\n");
        return;
    }
    HLOG("warmup: %s:%d (force DNS+cert cache)...\n", host, port);
    ssl = tls_connect(host, port, verify_ssl, &sock, &io);
    if (ssl) {
        /* Kept for the first real request instead of thrown away: a
         * full 68k handshake is far too expensive to waste. If the
         * server closes it before then, that request notices and
         * reconnects -- resuming this session, which is still cheaper. */
        conn_cache_store(host, port, ssl, sock, 0);
        HLOG("warmup: OK — DNS and CA certs cached in memory\n");
    } else {
        HLOG("warmup: connect failed (non-fatal; first packet may still Guru)\n");
    }
}

#endif /* PLATFORM_AMIGA */
