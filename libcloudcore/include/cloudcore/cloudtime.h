/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef CLOUDCORE_CLOUDTIME_H
#define CLOUDCORE_CLOUDTIME_H

#include "amicloud.h"

/*
 * cloudtime.h — the ONE place DateStamp<->UTC-epoch conversion lives
 * (module-encapsulation rule: this was previously duplicated verbatim in
 * client/localfs_amiga.c's datestamp_to_epoch()/epoch_to_datestamp() and
 * cloudhandler/worker.c's copy of the same two functions). Both now call
 * here instead. PC/Win builds never need this — their libc mtimes are
 * already UTC epoch — so it only compiles under PLATFORM_AMIGA.
 */

#define AC_AMIGA_EPOCH_SHIFT_DAYS 2922L /* days between 1970-01-01 and 1978-01-01 */

#ifdef PLATFORM_AMIGA
#include <dos/dos.h> /* struct DateStamp */

/* Civil-calendar-from-days-since-1970-01-01 (Howard Hinnant's algorithm).
 * Shared by anything that needs to format a DateStamp for display. */
void ac_civil_from_days(long z, int *y, int *m, int *d);

/* ds is Amiga LOCAL time (the system clock has no timezone info at all);
 * gmt_offset_minutes/dst come from AcConfig, loaded once from the config
 * file. Returns UTC epoch seconds. */
long ac_datestamp_to_epoch(const struct DateStamp *ds, int gmt_offset_minutes, int dst);

/* Inverse of ac_datestamp_to_epoch(). Clamped at 0 for epochs before the
 * Amiga epoch (1978-01-01) — not a realistic case for real files. */
void ac_epoch_to_datestamp(long epoch, int gmt_offset_minutes, int dst, struct DateStamp *ds);

/* Current system clock as "DD.MM.YYYY HH:MM" (local, as the Amiga sees
 * it). For error messages that hinge on the clock — e.g. a TLS
 * certificate "not yet valid" because the RTC battery is flat. */
void ac_format_system_clock(char *buf, int bufsize);

#endif /* PLATFORM_AMIGA */

#endif /* CLOUDCORE_CLOUDTIME_H */
