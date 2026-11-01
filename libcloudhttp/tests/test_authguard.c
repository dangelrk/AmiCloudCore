/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/* Unit tests for src/authguard.h (no repeated logins with rejected
 * credentials). The clock is faked: time() inside the header is
 * fake_time(). */
#include <stdio.h>
#include <string.h>
#include <time.h>

static long g_now = 1000000;
static time_t fake_time(time_t *t) { if (t) *t = (time_t)g_now; return (time_t)g_now; }
#define time fake_time
#include "../src/authguard.h"
#undef time

static int g_ok, g_fail;
#define CHECK(c, what) do { if (c) g_ok++; else { g_fail++; printf("FEHLER: %s (Zeile %d)\n", what, __LINE__); } } while (0)

static const char *A = "https://cloud.example/remote.php/dav/files/test/";
static const char *B = "https://cloud.example/api.php?action=list";
static const char *OTHER = "https://other.example/api.php";

/* returns the held status, or 0 when the request may go out */
static int held(const char *url, const char *auth)
{
    HttpResponse *r = authguard_check(url, auth);
    int st = r ? r->status_code : 0;
    if (r) {
        CHECK(r->body && strstr(r->body, "\"status\":\"error\""), "Antwort hat JSON-Fehler");
        free(r->body);
        free(r);
    }
    return st;
}

int main(void)
{
    CHECK(held(A, "test:falsch") == 0, "erster Versuch geht raus");
    authguard_note(A, "test:falsch", 401);
    CHECK(held(A, "test:falsch") == 401, "zweiter Versuch sofort 401");
    CHECK(held(B, "test:falsch") == 401, "anderer Pfad, gleicher Server: auch gesperrt");
    CHECK(held(OTHER, "test:falsch") == 0, "anderer Server nicht gesperrt");
    CHECK(held(A, "test:richtig") == 0, "neues Passwort geht sofort");
    CHECK(held(A, "") == 0 && held(A, NULL) == 0, "ohne Zugangsdaten nicht bewacht");

    authguard_note(A, "test:richtig", 200);
    CHECK(held(A, "test:falsch") == 401, "Erfolg mit anderem Passwort hebt Sperre nicht auf");

    g_now += AUTHGUARD_HOLDOFF - 1;
    CHECK(held(A, "test:falsch") == 401, "kurz vor Ablauf noch gesperrt");
    g_now += 1;
    CHECK(held(A, "test:falsch") == 0, "nach Ablauf ein neuer Versuch");
    authguard_note(A, "test:falsch", 401);
    CHECK(held(A, "test:falsch") == 401, "wieder abgelehnt: wieder gesperrt");

    authguard_note(A, "u:p", 429);
    CHECK(held(A, "u:p") == 429, "429 sperrt ebenso");
    g_now += AUTHGUARD_HOLDOFF;
    CHECK(held(A, "u:p") == 0, "429-Sperre laeuft ab");
    authguard_note(A, "u:p", 200);
    CHECK(held(A, "u:p") == 0, "Erfolg loescht Eintrag");
    authguard_note(A, "u:p", 404);
    CHECK(held(A, "u:p") == 0, "404 sperrt nicht");
    authguard_note(A, "u:p", 0);
    CHECK(held(A, "u:p") == 0, "Netzfehler sperrt nicht");

    /* more rejected pairs than slots: the newest are always held */
    {
        char auth[32];
        int i;
        for (i = 0; i < AUTHGUARD_SLOTS + 3; i++) {
            g_now++;
            sprintf(auth, "u%d:x", i);
            authguard_note(A, auth, 401);
            CHECK(held(A, auth) == 401, "neuester Eintrag gesperrt");
        }
    }

    printf("authguard: %d ok, %d Fehler\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
