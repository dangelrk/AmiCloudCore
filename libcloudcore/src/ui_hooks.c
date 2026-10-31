/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * ui_hooks.c — default (console) implementation + dispatch, see ui_hooks.h.
 */
#include "ui_hooks.h"
#include "amicloud.h" /* PLATFORM_AMIGA */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#ifdef PLATFORM_AMIGA
#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#endif

/* Latched Ctrl-C, see ui_hooks.h. The signal itself is consumed: left
 * pending, vbcc's stdio (__chkabort) would end the program on its next
 * printf -- the shell would quit instead of returning to its prompt and
 * get would leave its partial file behind. */
static int g_break = 0;

int ui_break_pending(void)
{
#ifdef PLATFORM_AMIGA
    if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) g_break = 1;
#endif
    return g_break;
}

void ui_break_set(void)
{
    g_break = 1;
}

void ui_break_clear(void)
{
    g_break = 0;
#ifdef PLATFORM_AMIGA
    SetSignal(0, SIGBREAKF_CTRL_C);
#endif
}

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

void ui_format_rate(char *out, size_t outsz, long bps)
{
    if (bps < 0) bps = 0;
    snprintf(out, outsz, "%ld.%ld KB/s", bps / 1024, (bps % 1024) * 10 / 1024);
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

/* Default line reader for the console prompts below: prompt + fgets(). */
static int stdio_read_line(const char *prompt, char *buf, size_t bufsz)
{
    size_t n;
    fputs(prompt, stdout);
    fflush(stdout);
    if (!fgets(buf, (int)bufsz, stdin)) return -1;
    n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
    return (int)n;
}

static UiReadLine g_read_line = stdio_read_line;

void ui_set_line_reader(UiReadLine fn)
{
    g_read_line = fn ? fn : stdio_read_line;
}

static int console_confirm(const char *msg)
{
    char prompt[1100], line[16];
    console_progress_close_line();
    snprintf(prompt, sizeof(prompt), "%s [j/N] ", msg);
    if (g_read_line(prompt, line, sizeof(line)) < 0) return 0;
    return (line[0] == 'y' || line[0] == 'Y' || line[0] == 'j' || line[0] == 'J');
}

static int console_progress(long done, long total, const char *name)
{
    int redraw;
    char rate[24];

    if (ui_break_pending()) return 0; /* Ctrl-C: cancel this transfer */
    /* Unnamed = a small API request (list, info, mkdir ...): no status
     * line for those, over a slow line they'd print "0K gelesen" each. */
    if (!name || !*name) return 1;
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

    ui_format_rate(rate, sizeof(rate), g_console_rate.bps);
    if (total > 0) {
        /* done*100 overflows a 32-bit long beyond ~21 MB */
        long pct = total > 2000000L ? done / (total / 100) : (done * 100) / total;
        printf("\r%-24.24s %6ld%% %12s   ", name, pct, rate);
    } else {
        printf("\r%-24.24s %6ldK gelesen %12s   ", name, done / 1024, rate);
    }
    fflush(stdout);
    g_console_line_open = 1;
    return 1;
}

/* Console overwrite prompt (amicloud-shell's get -r/put -r and wildcard
 * copies without -f; the GUIs install their own hook). The default is
 * the safe choice: an empty answer skips the file, no answer at all
 * (EOF / line reader returns -1) cancels the whole operation. */
static UiOverwrite console_overwrite(int is_dir, const char *name,
                                     long src_size, const char *src_date, const char *src_md5,
                                     long dst_size, const char *dst_date, const char *dst_md5)
{
    char line[16];

    console_progress_close_line();
    if (is_dir) {
        printf("Directory '%s' already exists.\n", name);
    } else {
        printf("'%s' already exists.\n", name);
        printf("  existing: %10ld bytes  %s\n", dst_size, (dst_date && *dst_date) ? dst_date : "-");
        printf("  new:      %10ld bytes  %s\n", src_size, (src_date && *src_date) ? src_date : "-");
        if (src_md5 && dst_md5 && *src_md5 && strcmp(src_md5, dst_md5) == 0)
            printf("  (identical content, MD5 %s)\n", src_md5);
    }
    if (g_read_line(is_dir
                    ? "Copy into it? [y]es [n]o [a]ll [s]kip all [c]ancel (default: n) "
                    : "Overwrite? [y]es [n]o [a]ll [s]kip all [c]ancel (default: n) ",
                    line, sizeof(line)) < 0)
        return UI_OW_CANCEL;
    switch (line[0]) {
    case 'y': case 'Y': return UI_OW_OVERWRITE;
    case 'a': case 'A': return UI_OW_OVERWRITE_ALL;
    case 's': case 'S': return UI_OW_SKIP_ALL;
    case 'c': case 'C': case 'q': case 'Q': return UI_OW_CANCEL;
    default:            return UI_OW_SKIP;
    }
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
