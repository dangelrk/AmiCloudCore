/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef CLOUDCORE_WBSTARTUP_H
#define CLOUDCORE_WBSTARTUP_H

/*
 * wbstartup.h -- Workbench-launch helpers (icon.library tooltypes),
 * factored out of amicloud-commander-mui/amivcs-gui-mui/amiowncloud-sync-cx,
 * which each had their own copy of this exact dance (OpenLibrary("icon.
 * library"), CurrentDir/GetDiskObject/FindToolType/FreeDiskObject).
 *
 * PLATFORM_AMIGA-only AND deliberately its own file, never folded into
 * config.c: a CLI program has no business linking icon.library/
 * workbench headers at all. Only Workbench-launchable GUI apps
 * (amicloud-commander-mui, amivcs-gui-mui, amiowncloud-sync-cx) add
 * wbstartup.c to their Makefile's CORE_SRCS -- a plain CLI build simply
 * never compiles this file in, so it stays exactly as WB-independent as
 * it always was.
 */
#ifdef PLATFORM_AMIGA

#include <stddef.h>

/* True if launched from Workbench (argc==0, the AmigaOS convention)
 * rather than a CLI/Shell. */
int ac_wb_started(int argc);

/* Only meaningful when ac_wb_started(argc). Reads the launched icon's
 * ToolTypes for a "NAME=value" entry and copies value into out. Returns
 * 1 if found (out filled), 0 otherwise (out left untouched, including
 * the non-Workbench case). */
int ac_wb_tooltype(int argc, char *argv[], const char *name, char *out, size_t outsz);

/* Same, for a flag-only tooltype (e.g. "DEBUG", no "=value"). Returns 1
 * if present. */
int ac_wb_tooltype_present(int argc, char *argv[], const char *name);

#endif /* PLATFORM_AMIGA */
#endif /* CLOUDCORE_WBSTARTUP_H */
