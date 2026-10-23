/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef HTTP_H
#define HTTP_H

#include <stdio.h>
#include "amicloud.h"
#include "md5.h"

/*
 * Result of an HTTP request.
 * body is malloc'd; call http_response_free() when done.
 * body is NULL when the response was streamed directly to a file.
 */
typedef struct {
    char *body;
    long  body_len;
    int   status_code;
    /* Value of the X-AmiCloud-MD5 response header (lowercase hex), ""
     * if absent (older server, or a partial/offset download — see plan
     * section 18.2). Deliberately a single dedicated field, not a
     * generic header store — this is the only header the client needs. */
    char  md5_header[40];
    /* 1 if the transfer was cancelled via the progress hook returning 0
     * (GUI-Commander-plan section 5.2) or by Ctrl-C (Amiga) — distinct
     * from a genuine network failure so callers can clean up (delete the
     * partial file) without changing behaviour for real errors. */
    int   aborted;
} HttpResponse;

/*
 * Send an HTTP/HTTPS request and return the response body in memory.
 *
 * method      : "GET", "POST", "DELETE"
 * url         : full HTTPS URL
 * auth        : "username:password" (encoded to Basic Auth internally)
 * upload_data : body to send for POST (NULL for GET/DELETE)
 * upload_len  : length of upload_data
 * verify_ssl  : 1 = verify certificate (default), 0 = skip (dev only)
 *
 * Returns allocated HttpResponse; caller must call http_response_free().
 * Returns NULL on fatal network error. status_code 0 in a returned
 * response means the transfer broke off part-way (the body is
 * incomplete and must not be used) -- also a network error.
 */
HttpResponse *http_request(const char    *method,
                           const char    *url,
                           const char    *auth,
                           const char    *upload_data,
                           long           upload_len,
                           int            verify_ssl);

/*
 * Wie http_request, aber mit beliebiger Methode (PUT, PROPFIND, MKCOL,
 * MOVE, ...) und optionalen Zusatz-Headern fuer WebDAV.
 *
 * extra_headers : "Depth: 1\r\nDestination: https://...\r\n" oder NULL.
 *                 Enthaelt es einen Content-Type, ersetzt der den Default
 *                 (application/octet-stream) fuer den Upload-Body.
 */
HttpResponse *http_request_hdr(const char *method,
                               const char *url,
                               const char *auth,
                               const char *extra_headers,
                               const char *upload_data,
                               long        upload_len,
                               int         verify_ssl);

/*
 * Download URL directly into fp (no in-memory buffer).
 * Returns an HttpResponse with body==NULL and the HTTP status code.
 *
 * md5ctx       : optional (NULL to skip) — if non-NULL, every chunk written
 *                to fp is also fed into md5_update(md5ctx, ...) as it
 *                arrives, so the caller gets a streaming digest of the
 *                downloaded bytes without a second read pass (plan
 *                section 18.3). Caller owns md5ctx (already md5_init()'d)
 *                and calls md5_final() after.
 * display_name : passed through to the ui_progress() hook (GUI-Commander-
 *                plan section 5.2) so a GUI can show what's transferring;
 *                NULL is fine, the hook just gets a NULL name. Purely
 *                informational, never sent over the wire.
 *
 * Calls ui_progress(bytes_so_far, total_or_minus_1, display_name)
 * periodically; if the hook returns 0, the transfer is aborted and the
 * returned HttpResponse has aborted=1 (see above). The default console
 * hook always continues, so this is a no-op for the plain CLI/shell.
 */
HttpResponse *http_download(const char *url,
                            const char *auth,
                            FILE       *fp,
                            int         verify_ssl,
                            Md5Ctx     *md5ctx,
                            const char *display_name);

/*
 * Upload fp's contents (fsize bytes, read from the current position) as
 * the POST body — streamed in fixed-size chunks rather than loaded
 * whole into RAM first, unlike http_request()'s upload_data path (GUI-
 * Commander-plan section 5.3: an 8 MB Amiga can't malloc a large upload).
 * Same progress/cancel behaviour as http_download(); response body is
 * still buffered in memory as usual (it's just the small JSON ack).
 */
HttpResponse *http_upload(const char *url,
                          const char *auth,
                          FILE       *fp,
                          long        fsize,
                          int         verify_ssl,
                          const char *display_name);

/*
 * Wie http_upload(), aber mit beliebiger Methode (PUT statt des
 * hartcodierten POST) und optionalen Zusatz-Headern -- fuers WebDAV-
 * Sync-Backend (libcloudsync/webdav.c), das eine Datei per PUT samt
 * X-OC-Mtime-Header hochladen muss, ohne den Streaming-Upload (kein
 * Ganze-Datei-malloc, siehe http_upload()'s eigener Kommentar) dafuer
 * aufzugeben. http_upload() bleibt ein duenner Wrapper hierauf mit
 * method="POST"/extra_headers=NULL -- unveraendertes Verhalten fuer
 * jeden bestehenden Aufrufer.
 */
HttpResponse *http_upload_hdr(const char *method,
                              const char *url,
                              const char *auth,
                              const char *extra_headers,
                              FILE       *fp,
                              long        fsize,
                              int         verify_ssl,
                              const char *display_name);

void http_response_free(HttpResponse *r);

/* Network idle timeout in seconds: a connect, TLS handshake, send or
 * receive that makes no progress at all for this long fails. It is
 * never a limit on a whole transfer -- a large file over a modem may
 * take as long as it needs as long as data keeps moving. Set from the
 * config's timeout= key (cmd.c/webdavcmd.c do that per request);
 * secs <= 0 restores the default. */
#define AC_HTTP_DEFAULT_TIMEOUT 120
void http_set_timeout(int secs);

/* Platform init/cleanup (no-op on Unix; WSAStartup/WSACleanup on Windows) */
int  http_init(void);
void http_cleanup(void);

/* Text of the most recent error reported by the HTTP layer (e.g. why
 * http_init() failed: "cannot open bsdsocket.library"), without prefix
 * and trailing newline; "" if none. Recorded regardless of
 * ac_errors_to_stderr, so GUI programs -- which suppress stderr output --
 * can still show the reason in a requester or message box. */
const char *http_last_error(void);

#ifdef PLATFORM_AMIGA
#include <dos/dos.h>      /* BPTR ist in dos/dos.h definiert (NICHT exec/types.h)! */
/* http_log_bptr: Sentinel-Flag (!=0 = HTTP-Log aktiv), kein offenes Handle.
 * http_log_path: Pfad zur Log-Datei (z.B. "RAM:httplog.log").
 * Beide werden vom Handler in main() gesetzt; der Client laesst sie auf 0/"". */
extern BPTR http_log_bptr;
extern char http_log_path[256];
/* g_log_buf: Handler-Log-Puffer (kein DOS im Packet-Loop).
 * Gesetzt/freigegeben in handler.c; http_write_log schreibt hierher. */
extern char  *g_log_buf;
extern ULONG  g_log_pos;
extern ULONG  g_log_size;
/* Force DNS resolution + CA cert loading BEFORE devnode->dn_Task is set.
 * Call once from backend_init() to prevent DOS calls in packet_loop. */
void http_warmup(const char *base_url, int verify_ssl);

/* Same debug-log sink as http_amiga.c's internal HLOG macro (hlog_fn) --
 * exposed here so portable callers like cmd.c's cmd_get_buf()/cmd_put_buf()
 * (which have no access to that static function) can log crash-hunt
 * diagnostics through the same debug=... mechanism. No-op unless
 * http_log_bptr/http_log_path (handler mode) or ac_verbose (client mode)
 * is active, exactly like HLOG. */
void ac_log_printf(const char *fmt, ...);
#endif

#endif /* HTTP_H */
