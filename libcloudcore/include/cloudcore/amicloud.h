/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef AMICLOUD_H
#define AMICLOUD_H

/* Platform detection */
#if defined(__amigaos__) || defined(AMIGA) || defined(__AMIGA__)
#  define PLATFORM_AMIGA 1
#  include <exec/types.h>
   typedef long AC_SOCKET;
#  define INVALID_SOCKET (-1)
#elif defined(_WIN32) || defined(_WIN64)
#  define PLATFORM_WIN 1
#  include <winsock2.h>
#  include <ws2tcpip.h>
   typedef SOCKET AC_SOCKET;
#else
#  define PLATFORM_UNIX 1
#  include <sys/socket.h>
#  include <netdb.h>
#  include <arpa/inet.h>
   typedef int AC_SOCKET;
#  define INVALID_SOCKET (-1)
#endif

#include "parson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "version.h"   /* AC_VERSION, AC_DATE */
#ifndef AC_MAX_PATH
#define AC_MAX_PATH 1024
#endif
#define AC_MAX_URL  2048

typedef struct {
    char server[AC_MAX_URL];    /* https://host/path */
    char username[64];
    char password[128];
    char basepath[AC_MAX_PATH]; /* remote base path prefix */
    int  verify_ssl;            /* 1 = verify (default), 0 = skip (-k flag) */
    char debug[256];            /* log target, e.g. "T:amicloud.log" (empty = off) */
    /* Amiga-Systemzeit hat keinerlei Zeitzoneninfo (plan Abschnitt 19.2) —
     * nur vom Amiga-Build ausgewertet (localfs_amiga.c), PC-Builds parsen
     * die Keys mit, nutzen sie aber nicht (ihre libc-mtimes sind bereits
     * UTC-Epoche). Default 0/0 = Uhr laeuft auf UTC. */
    int  gmt_offset;             /* Minuten relativ zu GMT/UTC, z.B. MEZ=60 */
    int  dst;                    /* Sommerzeit aktiv? 1 = ja (+60 Min) */
    int  timeout;                /* Netz-Leerlauf-Timeout in Sekunden,
                                  * 0 = Vorgabe (AC_HTTP_DEFAULT_TIMEOUT,
                                  * http.h). Gilt fuer Stillstand, nie fuer
                                  * eine ganze Uebertragung. */
    int  cache_ttl;              /* Nur Amiga-Handler: Lebensdauer des
                                  * Verzeichnis-Caches in Sekunden, 0 =
                                  * Vorgabe des Handlers. */
} AcConfig;

#endif /* AMICLOUD_H */
