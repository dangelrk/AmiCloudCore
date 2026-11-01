/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * authguard.h -- no repeated logins with credentials the server has
 * already rejected.
 *
 * Private to libcloudhttp: included exactly once by each backend
 * (http_amiga.c, http_pc.c), so every program on every system gets it
 * without its own build file knowing about it.
 *
 * Once a server answers 401 (login rejected) or 429 (too many attempts)
 * for a user:password pair, every further request with the same pair to
 * the same server fails at once with the same status, without touching
 * the network, for AUTHGUARD_HOLDOFF seconds. Then one new attempt is
 * allowed. Before, a handler or sync run with a wrong password fired one
 * login per DOS action or file (four within one second against ownCloud),
 * and the server's brute-force protection locked the account -- and kept
 * extending the lock.
 *
 * Different credentials (the user fixed the password) are never held
 * back, and any answer other than 401/429 clears the entry. Requests
 * without credentials are not guarded (an anonymous 401 is "needs
 * login", not a failed guess). Only an MD5 of server+credentials is
 * kept, never the password.
 */
#ifndef AUTHGUARD_H
#define AUTHGUARD_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "http.h"

#define AUTHGUARD_HOLDOFF 900   /* s; the AmiCloud server's own lockout is 15 min */
#define AUTHGUARD_SLOTS   4

typedef struct {
    unsigned char key[16];
    long          until;   /* time() up to which the pair is held back */
    int           status;  /* 401 or 429 */
} AuthGuardSlot;

static AuthGuardSlot g_authguard[AUTHGUARD_SLOTS];

/* key = MD5("scheme://host[:port]" NUL "user:password"); 0 = not guarded */
static int authguard_key(const char *url, const char *auth, unsigned char key[16])
{
    Md5Ctx ctx;
    const char *p, *end;

    if (!url || !auth || !*auth) return 0;
    p = strstr(url, "://");
    p = p ? p + 3 : url;
    end = strchr(p, '/');
    if (!end) end = p + strlen(p);
    md5_init(&ctx);
    md5_update(&ctx, (const unsigned char *)url, (size_t)(end - url));
    md5_update(&ctx, (const unsigned char *)"", 1);
    md5_update(&ctx, (const unsigned char *)auth, strlen(auth));
    md5_final(&ctx, key);
    return 1;
}

static AuthGuardSlot *authguard_find(const unsigned char key[16])
{
    int i;
    for (i = 0; i < AUTHGUARD_SLOTS; i++)
        if (g_authguard[i].until && memcmp(g_authguard[i].key, key, 16) == 0)
            return &g_authguard[i];
    return NULL;
}

/* Before a request: NULL = go ahead; otherwise a ready answer with the
 * held status and a JSON message (the AmiCloud error format; WebDAV
 * callers only look at the status). */
static HttpResponse *authguard_check(const char *url, const char *auth)
{
    unsigned char key[16];
    AuthGuardSlot *s;
    HttpResponse *resp;
    long now, mins;
    char msg[200];

    if (!authguard_key(url, auth, key) || !(s = authguard_find(key))) return NULL;
    now = (long)time(NULL);
    if (now >= s->until) { s->until = 0; return NULL; }   /* one new attempt */
    mins = (s->until - now + 59) / 60;
    snprintf(msg, sizeof(msg), "{\"status\":\"error\",\"message\":\"%s - kein neuer Anmeldeversuch "
            "fuer %ld Min. (Benutzer/Passwort pruefen)\"}",
            s->status == 429 ? "Zu viele Fehlversuche" : "Anmeldung abgelehnt", mins);
    resp = calloc(1, sizeof(HttpResponse));
    if (!resp) return NULL;
    resp->status_code = s->status;
    resp->body = malloc(strlen(msg) + 1);
    if (resp->body) {
        strcpy(resp->body, msg);
        resp->body_len = (long)strlen(msg);
    }
    return resp;
}

/* After a request that got an answer from the server. */
static void authguard_note(const char *url, const char *auth, int status)
{
    unsigned char key[16];
    AuthGuardSlot *s;
    int i;

    if (status <= 0 || !authguard_key(url, auth, key)) return;
    s = authguard_find(key);
    if (status != 401 && status != 429) {
        if (s) s->until = 0;
        return;
    }
    if (!s) {
        s = &g_authguard[0];               /* free slot, else the oldest */
        for (i = 0; i < AUTHGUARD_SLOTS; i++) {
            if (!g_authguard[i].until) { s = &g_authguard[i]; break; }
            if (g_authguard[i].until < s->until) s = &g_authguard[i];
        }
        memcpy(s->key, key, 16);
    }
    s->status = status;
    s->until  = (long)time(NULL) + AUTHGUARD_HOLDOFF;
}

#endif /* AUTHGUARD_H */
