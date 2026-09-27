/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * ui_hooks.c — default (console) implementation + dispatch, see ui_hooks.h.
 */
#include "ui_hooks.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

void ui_rate_reset(UiRateMeter *m)
{
    long now = (long)time(NULL);
    m->start_time = now;
    m->last_time  = now;
    m->last_bytes = 0;
    m->bps        = 0;
}

int ui_rate_sample(UiRateMeter *m, long done)
{
    long now = (long)time(NULL);

    if (done < m->last_bytes) {
        /* New file/transfer started without an explicit reset. */
        ui_rate_reset(m);
        m->last_bytes = done;
        return 0;
    }
    if (now == m->last_time) return 0;
    m->bps = (done - m->last_bytes) / (now - m->last_time);
    m->last_bytes = done;
    m->last_time  = now;
    return 1;
}

/* Console progress line: a live \r-updated status line, throttled via
 * UiRateMeter to ~1 redraw/sec (see ui_hooks.h). g_console_line_open lets
 * console_notify()/console_confirm() start their own output on a clean
 * line instead of overwriting/mixing with an in-progress status line. */
static UiRateMeter g_console_rate;
static char        g_console_last_name[256] = "";
static int         g_console_line_open = 0;

static void console_progress_close_line(void)
{
    if (g_console_line_open) {
        printf("\n");
        g_console_line_open = 0;
    }
}

static void console_notify(int level, const char *msg)
{
    console_progress_close_line();
    if (level == UI_ERROR || level == UI_WARN) fprintf(stderr, "%s\n", msg);
    else printf("%s\n", msg);
}

static int console_confirm(const char *msg)
{
    char line[16];
    console_progress_close_line();
    printf("%s [y/N] ", msg);
    fflush(stdout);
    if (!fgets(line, sizeof(line), stdin)) return 0;
    return (line[0] == 'y' || line[0] == 'Y');
}

static int console_progress(long done, long total, const char *name)
{
    int redraw;
    long kbps;

    if (!name) name = "";
    if (strcmp(name, g_console_last_name) != 0 || done < g_console_rate.last_bytes) {
        ui_rate_reset(&g_console_rate);
        strncpy(g_console_last_name, name, sizeof(g_console_last_name) - 1);
        g_console_last_name[sizeof(g_console_last_name) - 1] = '\0';
    }

    /* Throttled like everywhere else, but always redraw the very last
     * sample of a known-size transfer so the line actually reaches 100%
     * instead of freezing on whatever the last throttled tick showed. */
    redraw = ui_rate_sample(&g_console_rate, done) || (total > 0 && done >= total);
    if (!redraw) return 1;

    kbps = g_console_rate.bps / 1024;
    if (total > 0) {
        long pct = (done * 100) / total;
        printf("\r%-24.24s %6ld%% %6ld KB/s   ", name, pct, kbps);
    } else {
        printf("\r%-24.24s %6ldK gelesen %6ld KB/s   ", name, done / 1024, kbps);
    }
    fflush(stdout);
    g_console_line_open = 1;
    return 1;
}

/* Console/shell recursive get/put has always overwritten silently (no
 * existence check at all) — the default keeps that behaviour exactly, so
 * shell.c's CopyCtx is built with ask=0 and this hook is never even
 * consulted there; it only matters if some future caller sets ask=1
 * without installing its own overwrite hook. */
static UiOverwrite console_overwrite(int is_dir, const char *name,
                                     long src_size, const char *src_date, const char *src_md5,
                                     long dst_size, const char *dst_date, const char *dst_md5)
{
    (void)is_dir; (void)name;
    (void)src_size; (void)src_date; (void)src_md5;
    (void)dst_size; (void)dst_date; (void)dst_md5;
    return UI_OW_OVERWRITE;
}

static UiHooks g_hooks = { console_notify, console_confirm, console_progress, console_overwrite };

void ui_set_hooks(const UiHooks *h)
{
    if (!h) {
        g_hooks.notify    = console_notify;
        g_hooks.confirm   = console_confirm;
        g_hooks.progress  = console_progress;
        g_hooks.overwrite = console_overwrite;
        return;
    }
    if (h->notify)    g_hooks.notify    = h->notify;
    if (h->confirm)   g_hooks.confirm   = h->confirm;
    if (h->progress)  g_hooks.progress  = h->progress;
    if (h->overwrite) g_hooks.overwrite = h->overwrite;
}

void ui_notify(int level, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_hooks.notify(level, buf);
}

int ui_confirm(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return g_hooks.confirm(buf);
}

int ui_progress(long done, long total, const char *name)
{
    return g_hooks.progress(done, total, name);
}

UiOverwrite ui_overwrite(int is_dir, const char *name,
                         long src_size, const char *src_date, const char *src_md5,
                         long dst_size, const char *dst_date, const char *dst_md5)
{
    return g_hooks.overwrite(is_dir, name, src_size, src_date, src_md5,
                             dst_size, dst_date, dst_md5);
}
