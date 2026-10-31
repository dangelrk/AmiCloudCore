/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 *
 * Version and release date of all programs of AmiCloud, AmiOwnCloud and
 * AmiVCS -- the only place where they are maintained. Every program
 * builds its "$VER: name " AC_VERSION " (" AC_DATE ")" string from it,
 * "make release" names the archive after AC_VERSION and fills in both
 * values in the installer scripts (@VERSION@, @DATE@).
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

#define AC_VERSION "0.1"         /* version.revision, both numbers */
#define AC_DATE    "31.10.2026"  /* release date, dd.mm.yyyy */

#endif
