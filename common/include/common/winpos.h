/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef COMMON_WINPOS_H
#define COMMON_WINPOS_H

/*
 * winpos.h -- window position memory for the Intuition (non-MUI)
 * programs: the prefs editors and the sync commodity. MUI programs use
 * mui_remember_window() from mui_stubs.h instead.
 *
 * "left top" is kept in the global variable `var`, e.g.
 * "AmiCloud/Windows/AmiCloudPrefs", in ENV: and ENVARC:, so a window
 * reopens where it was left, also after a reboot.
 *
 * Header-only on purpose: two small functions, and every program that
 * opens a window can include this without its Makefiles (cross and
 * native Build-Amiga) growing another object file.
 */
#include <exec/types.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <dos/dos.h>
#include <dos/var.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>

/* Overwrites *left/*top with the saved position -- only if there is one
 * and a w x h window there still fits on `scr` (NULL: no check). */
static void ac_winpos_load(const char *var, struct Screen *scr, WORD w, WORD h,
                           WORD *left, WORD *top)
{
    char buf[32];
    long l, t;

    if (GetVar((STRPTR)var, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) <= 0)
        return;
    if (sscanf(buf, "%ld %ld", &l, &t) != 2 || l < 0 || t < 0)
        return;
    if (scr && (l + w > scr->Width || t + h > scr->Height))
        return;   /* screen got smaller: keep the default */
    *left = (WORD)l;
    *top = (WORD)t;
}

/* Stores win's current position; call right before CloseWindow().
 * SetVar() does not create directories, so the ones in `var` are made
 * in ENV: and ENVARC: first. */
static void ac_winpos_save(const char *var, struct Window *win)
{
    static const char *const roots[] = { "ENV:", "ENVARC:" };
    char path[160], buf[32];
    size_t i, n;
    int r;

    if (!win) return;
    for (r = 0; r < 2; r++) {
        strcpy(path, roots[r]);
        n = strlen(path);
        for (i = 0; var[i] && n + 1 < sizeof(path); i++) {
            if (var[i] == '/') {
                BPTR lock;
                path[n] = '\0';
                lock = CreateDir((STRPTR)path);
                if (lock) UnLock(lock);
            }
            path[n++] = var[i];
        }
    }
    sprintf(buf, "%d %d", (int)win->LeftEdge, (int)win->TopEdge);
    SetVar((STRPTR)var, (STRPTR)buf, -1, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
}

#endif /* COMMON_WINPOS_H */
