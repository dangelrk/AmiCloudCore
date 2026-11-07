/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 *
 * Version and build date of all programs of AmiCloud, AmiOwnCloud and
 * AmiVCS. Every program builds its "$VER: name " AC_VERSION " (" AC_DATE
 * ")" string from it, "make release" names the archive after AC_VERSION
 * and fills in both values in the installer scripts (@VERSION@, @DATE@).
 *
 * AC_DATE is the build date, never maintained by hand: the Makefiles pass
 * -DAC_BUILD_DATE=dd.mm.yyyy (today; "make release" rebuilds every Amiga
 * program, so a release carries the day it was built and tagged). Passed
 * without quotes -- vbcc's vc frontend runs its passes through a shell
 * that would strip them -- "31.10.2026" is a single preprocessing number
 * and is turned into a string here. Without the define (native Build-
 * Amiga script, other build systems) __DATE__ is used ("Oct 31 2026").
 *
 * The Amiga installers only replace a program when the archive's version
 * is not older than the installed one (Installer: getversion, shell
 * variant: Version ... FILE). Raise AC_VERSION with every release,
 * otherwise an update cannot be told apart from the installed build.
 *
 * Plain preprocessor definitions only: the handlers include this file
 * without any C library (-nostdlib).
 */
#ifndef CLOUDCORE_VERSION_H
#define CLOUDCORE_VERSION_H

#define AC_VERSION "0.8"         /* version.revision, both numbers */

#define AC_STR_(x) #x
#define AC_STR(x)  AC_STR_(x)
#ifdef AC_BUILD_DATE
#define AC_DATE    AC_STR(AC_BUILD_DATE)  /* build date, dd.mm.yyyy */
#else
#define AC_DATE    __DATE__
#endif

#endif
