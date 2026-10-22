/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#include "progress.h"
#include <stdio.h>
#include <string.h>

/* On by default: amicloud-shell and the CLI tools call progress_prefix()
 * expecting exactly this "(x/y pct%) " console output, unchanged. The GUI
 * commander apps (amicloud-commander-mui/-qt) route everything through
 * UiHooks instead and render their own progress widget — they turn this
 * off at startup (same pattern as ac_verbose/ac_errors_to_stderr, see
 * main_mui.c's configure_debug()) so this shared copy engine doesn't punch
 * a stray console/output window through the GUI. */
int ac_console_progress = 1;

void progress_reset(Progress *p)
{
    memset(p, 0, sizeof(*p));
}

void progress_prefix(Progress *p, long file_size)
{
    p->done++;
    p->bytes_done += file_size;
    p->cur_file_size = file_size;
    if (!ac_console_progress || p->total <= 1) return;
    if (p->bytes_total > 0) {
        long pct = (p->bytes_done * 100) / p->bytes_total;
        printf("(%2d/%2d %3ld%%) ", p->done, p->total, pct);
    } else {
        /* 0-byte edge case (only empty files): no percentage shown. */
        printf("(%2d/%2d) ", p->done, p->total);
    }
    fflush(stdout);
}

void ac_format_kb(long kb, char *buf, size_t n)
{
    /* One decimal place via integer maths only (no float formatting in
     * vbcc's printf needed): tenths = value * 10 / 1024, rounded. */
    const char *unit;
    long whole, tenths;
    if (kb < 0) kb = 0;
    if (kb >= 1024L * 1024L) {
        long t = (kb / 1024L * 10L + 512L) / 1024L;   /* GB in tenths */
        whole = t / 10; tenths = t % 10; unit = "GB";
    } else if (kb >= 1024L) {
        long t = (kb * 10L + 512L) / 1024L;           /* MB in tenths */
        whole = t / 10; tenths = t % 10; unit = "MB";
    } else {
        snprintf(buf, n, "%ld KB", kb);
        return;
    }
    snprintf(buf, n, "%ld,%ld %s", whole, tenths, unit);
}
