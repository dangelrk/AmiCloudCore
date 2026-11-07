/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#include "cloudtime.h"

#ifdef PLATFORM_AMIGA
#include <stdio.h>
#include <proto/dos.h>
#endif

#ifdef PLATFORM_AMIGA

void ac_civil_from_days(long z, int *y, int *m, int *d)
{
    long era, doe, yoe, doy, mp;
    z += 719468L;
    era = (z >= 0 ? z : z - 146096L) / 146097L;
    doe = z - era * 146097L;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp  = (5 * doy + 2) / 153;
    *d  = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m  = (int)(mp + (mp < 10 ? 3 : -9));
    *y  = (int)(yoe + era * 400 + (*m <= 2));
}

long ac_datestamp_to_epoch(const struct DateStamp *ds, int gmt_offset_minutes, int dst)
{
    long ds_seconds  = (long)ds->ds_Days * 86400L + (long)ds->ds_Minute * 60L
                      + (long)(ds->ds_Tick / 50);
    long offset_secs = (long)(gmt_offset_minutes + (dst ? 60 : 0)) * 60L;
    return ds_seconds + (AC_AMIGA_EPOCH_SHIFT_DAYS * 86400L) - offset_secs;
}

void ac_epoch_to_datestamp(long epoch, int gmt_offset_minutes, int dst, struct DateStamp *ds)
{
    long offset_secs = (long)(gmt_offset_minutes + (dst ? 60 : 0)) * 60L;
    long local_secs  = epoch - (AC_AMIGA_EPOCH_SHIFT_DAYS * 86400L) + offset_secs;

    if (local_secs < 0) local_secs = 0;
    ds->ds_Days   = local_secs / 86400L;
    ds->ds_Minute = (local_secs % 86400L) / 60L;
    ds->ds_Tick   = (local_secs % 60L) * 50L;
}

void ac_format_system_clock(char *buf, int bufsize)
{
    struct DateStamp ds;
    int y, m, d;

    DateStamp(&ds);
    ac_civil_from_days((long)ds.ds_Days + AC_AMIGA_EPOCH_SHIFT_DAYS, &y, &m, &d);
    snprintf(buf, (size_t)bufsize, "%02d.%02d.%04d %02ld:%02ld",
             d, m, y, (long)(ds.ds_Minute / 60), (long)(ds.ds_Minute % 60));
}

#endif /* PLATFORM_AMIGA */
