/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * pattern_pc.c — Wildcard-Matching fuer macOS/Linux/Windows.
 * Kompiliert wenn PLATFORM_AMIGA NICHT gesetzt ist (Gegenstueck:
 * pattern_amiga.c).
 *
 * Unterstuetzt "*" (beliebig viele Zeichen) und "?" (genau ein Zeichen).
 * "#?" (AmigaDOS-Schreibweise fuer "beliebig viele Zeichen") wird vor dem
 * Kompilieren in "*" uebersetzt, damit dieselbe Musterzeichenkette auf
 * allen Plattformen funktioniert.
 */
#ifndef PLATFORM_AMIGA

#include "pattern.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

struct Pattern {
    char *text; /* vorverarbeitet: "#?" -> "*" */
};

int pattern_is_wildcard(const char *name)
{
    for (; *name; name++)
        if (*name == '*' || *name == '?' || *name == '#') return 1;
    return 0;
}

/* "#?" durch "*" ersetzen (kann die Zeichenkette verkuerzen, nie verlaengern) */
static void preprocess(const char *in, char *out)
{
    while (*in) {
        if (in[0] == '#' && in[1] == '?') { *out++ = '*'; in += 2; }
        else                              { *out++ = *in++; }
    }
    *out = '\0';
}

Pattern *pattern_compile(const char *pat)
{
    Pattern *p = (Pattern *)malloc(sizeof(Pattern));
    if (!p) return NULL;
    p->text = (char *)malloc(strlen(pat) + 1);
    if (!p->text) { free(p); return NULL; }
    preprocess(pat, p->text);
    return p;
}

/* Klassischer rekursiver Glob-Matcher ("*" und "?"), gross-/kleinschreibungs-
 * unabhaengig (wie AmigaDOS' MatchPatternNoCase — einheitliches Verhalten
 * auf allen Plattformen). */
static int ci_eq(char a, char b)
{
    return tolower((unsigned char)a) == tolower((unsigned char)b);
}

static int glob_match(const char *pat, const char *s)
{
    while (*pat) {
        if (*pat == '*') {
            while (*pat == '*') pat++;  /* aufeinanderfolgende "*" zusammenfassen */
            if (!*pat) return 1;        /* "*" am Ende passt auf den Rest */
            for (; *s; s++)
                if (glob_match(pat, s)) return 1;
            return 0;
        }
        if (!*s) return 0;
        if (*pat != '?' && !ci_eq(*pat, *s)) return 0;
        pat++; s++;
    }
    return !*s;
}

int pattern_match(Pattern *p, const char *name)
{
    return p ? glob_match(p->text, name) : 0;
}

void pattern_free(Pattern *p)
{
    if (!p) return;
    free(p->text);
    free(p);
}

#endif /* !PLATFORM_AMIGA */
