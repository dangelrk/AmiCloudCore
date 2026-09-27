/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * pattern_amiga.c — Wildcard-Matching fuer AmigaOS via native dos.library-
 * Musterfunktionen. Kompiliert nur wenn PLATFORM_AMIGA gesetzt ist
 * (Gegenstueck: pattern_pc.c).
 *
 * Nutzt ParsePatternNoCase()/MatchPatternNoCase() — das bringt neben "?"
 * und "#?" (beliebig viele Zeichen) automatisch die volle AmigaDOS-
 * Mustersprache mit ([abc], (a|b), ~ usw.), ohne dass wir sie selbst
 * nachbauen muessten.
 *
 * Ein blosses "*" (PC-Konvention) wird vor dem Parsen in "#?" uebersetzt,
 * damit dieselbe Musterzeichenkette auf allen Plattformen funktioniert —
 * AmigaDOS wuerde ein einzelnes "*" sonst als literales Zeichen behandeln.
 */
#ifdef PLATFORM_AMIGA

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "pattern.h"
#include <string.h>
#include <stdlib.h>

struct Pattern {
    UBYTE *buf; /* AllocVec — von ParsePatternNoCase gefuellt */
};

int pattern_is_wildcard(const char *name)
{
    for (; *name; name++)
        if (*name == '*' || *name == '?' || *name == '#' ||
            *name == '[' || *name == '(') return 1;
    return 0;
}

/* "*" durch "#?" ersetzen (kann die Zeichenkette verlaengern: 1 -> 2 Byte) */
static char *preprocess(const char *in)
{
    size_t len = strlen(in);
    char  *out = (char *)malloc(len * 2 + 1);
    char  *o;
    if (!out) return NULL;
    o = out;
    for (; *in; in++) {
        if (*in == '*') { *o++ = '#'; *o++ = '?'; }
        else            { *o++ = *in; }
    }
    *o = '\0';
    return out;
}

Pattern *pattern_compile(const char *pat)
{
    Pattern *p;
    char    *pre = preprocess(pat);
    LONG     buflen, rc;

    if (!pre) return NULL;

    p = (Pattern *)malloc(sizeof(Pattern));
    if (!p) { free(pre); return NULL; }

    /* Vom Autodoc empfohlene Puffergroesse: 2 * strlen(Muster) + 2 */
    buflen  = (LONG)strlen(pre) * 2 + 2;
    p->buf  = (UBYTE *)AllocVec((ULONG)buflen, MEMF_PUBLIC);
    if (!p->buf) { free(pre); free(p); return NULL; }

    rc = ParsePatternNoCase((CONST_STRPTR)pre, p->buf, buflen);
    free(pre);
    if (rc < 0) { FreeVec(p->buf); free(p); return NULL; }

    return p;
}

int pattern_match(Pattern *p, const char *name)
{
    return p ? (int)MatchPatternNoCase(p->buf, (CONST_STRPTR)name) : 0;
}

void pattern_free(Pattern *p)
{
    if (!p) return;
    FreeVec(p->buf);
    free(p);
}

#endif /* PLATFORM_AMIGA */
