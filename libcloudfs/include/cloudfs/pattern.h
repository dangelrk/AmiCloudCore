/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef PATTERN_H
#define PATTERN_H

/*
 * pattern.h — Wildcard-Muster fuer `get`/`put` in amicloud-shell.
 *
 * Beide Schreibweisen werden auf ALLEN Plattformen gleichermassen
 * akzeptiert: "*"/"?" (PC-Konvention) und "#?"/"?" (AmigaDOS-Konvention).
 * Das Matching passiert immer in unserem eigenen Code (die Zeilen werden
 * von shell.c's eigenem Tokenizer gelesen, nicht von bash/cmd.exe) — eine
 * Plattformbeschraenkung der Syntax haette keinen Vorteil.
 *
 * Zwei Implementierungen, wie localfs_pc.c/localfs_amiga.c:
 *   pattern_pc.c    — eigener kleiner "*"/"?"-Matcher (macOS/Linux/Windows)
 *   pattern_amiga.c — native dos.library-Musterei (ParsePatternNoCase/
 *                     MatchPatternNoCase) — bringt zusaetzlich die volle
 *                     AmigaDOS-Mustersprache ([abc], (a|b) usw.) mit
 */

typedef struct Pattern Pattern;

/* Enthaelt der (letzte Pfadsegment-)String Wildcard-Zeichen?
 * Reine Zeichenpruefung, keine Kompilierung, plattformunabhaengig. */
int pattern_is_wildcard(const char *name);

/* Kompiliert ein Muster — NUR das letzte Pfadsegment, kein Verzeichnis-
 * anteil. NULL bei Fehler (z.B. Speicherknappheit). */
Pattern *pattern_compile(const char *pat);

int pattern_match(Pattern *p, const char *name);

void pattern_free(Pattern *p);

#endif /* PATTERN_H */
