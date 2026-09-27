/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * wbstartup.c -- see wbstartup.h.
 */
#ifdef PLATFORM_AMIGA

#include "wbstartup.h"

#include <workbench/startup.h>
#include <workbench/workbench.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/icon.h>

#include <string.h>

int ac_wb_started(int argc)
{
    return argc == 0;
}

int ac_wb_tooltype(int argc, char *argv[], const char *name, char *out, size_t outsz)
{
    struct WBStartup *wb;
    struct Library *IconBase;
    int found = 0;

    if (argc != 0) return 0;
    wb = (struct WBStartup *)argv;

    IconBase = OpenLibrary((CONST_STRPTR)"icon.library", 0);
    if (IconBase && wb->sm_NumArgs > 0) {
        BPTR old = CurrentDir(wb->sm_ArgList[0].wa_Lock);
        struct DiskObject *dob = GetDiskObject(wb->sm_ArgList[0].wa_Name);
        if (dob) {
            STRPTR tt = FindToolType((CONST_STRPTR *)dob->do_ToolTypes, (CONST_STRPTR)name);
            if (tt) {
                char *eq = strchr((char *)tt, '=');
                const char *val = eq ? eq + 1 : (char *)tt;
                if (val[0]) {
                    strncpy(out, val, outsz - 1);
                    out[outsz - 1] = '\0';
                    found = 1;
                }
            }
            FreeDiskObject(dob);
        }
        CurrentDir(old);
    }
    if (IconBase) CloseLibrary(IconBase);
    return found;
}

int ac_wb_tooltype_present(int argc, char *argv[], const char *name)
{
    struct WBStartup *wb;
    struct Library *IconBase;
    int found = 0;

    if (argc != 0) return 0;
    wb = (struct WBStartup *)argv;

    IconBase = OpenLibrary((CONST_STRPTR)"icon.library", 0);
    if (IconBase && wb->sm_NumArgs > 0) {
        BPTR old = CurrentDir(wb->sm_ArgList[0].wa_Lock);
        struct DiskObject *dob = GetDiskObject(wb->sm_ArgList[0].wa_Name);
        if (dob) {
            if (FindToolType((CONST_STRPTR *)dob->do_ToolTypes, (CONST_STRPTR)name))
                found = 1;
            FreeDiskObject(dob);
        }
        CurrentDir(old);
    }
    if (IconBase) CloseLibrary(IconBase);
    return found;
}

#endif /* PLATFORM_AMIGA */
