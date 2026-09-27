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
