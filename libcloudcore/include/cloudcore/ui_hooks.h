/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef UI_HOOKS_H
#define UI_HOOKS_H

/*
 * ui_hooks.h — frontend-agnostic callback layer (GUI-Commander plan §5.1).
 *
 * cmd.c used to printf()/fprintf(stderr) directly and shell.c asked
 * yes/no questions straight off stdin. That's fine for a console client,
 * but a GUI frontend (Qt/MUI) needs to route the same information to a
 * status bar, a message box, and a progress dialog instead. This header
 * is the seam: cmd.c/shell.c call ui_notify()/ui_confirm()/ui_progress(),
 * which forward to whatever hook set is currently installed.
 *
 * Default hooks (installed automatically, restored by ui_set_hooks(NULL))
 * reproduce today's console behaviour for notify/confirm — the plain CLI
 * and amicloud-shell never call ui_set_hooks() at all. console_progress()
 * is the one deliberate exception: it now draws a live bytes/sec status
 * line (previously a total no-op), so console output during a transfer
 * is no longer byte-for-byte identical to before that was added.
 */

#include <stddef.h>

typedef enum { UI_INFO, UI_WARN, UI_ERROR } UiLevel;

/* Per-file/per-folder overwrite decision (GUI-Commander copy-flow rework).
 * OVERWRITE_ALL/SKIP_ALL are "sticky" — the caller applies them to every
 * remaining conflict of the current operation without asking again. */
typedef enum {
    UI_OW_OVERWRITE, UI_OW_SKIP,
    UI_OW_OVERWRITE_ALL, UI_OW_SKIP_ALL, UI_OW_CANCEL
} UiOverwrite;

typedef struct {
    /* Pre-formatted single-line message (no trailing newline). */
    void (*notify)(int level, const char *msg);
    /* 1 = yes, 0 = no. */
    int  (*confirm)(const char *msg);
    /* Called periodically during a transfer. done/total in bytes; total
     * may be -1 if unknown (e.g. no Content-Length yet). Return 0 to
     * cancel the transfer, nonzero to continue. */
    int  (*progress)(long done, long total, const char *name);
    /* Asked once per conflicting file/folder when the destination already
     * exists. src_* describes the incoming object, dst_* the existing one
     * (dates are pre-formatted display strings, md5 "" if unknown/dir). */
    UiOverwrite (*overwrite)(int is_dir, const char *name,
                             long src_size, const char *src_date, const char *src_md5,
                             long dst_size, const char *dst_date, const char *dst_md5);
} UiHooks;

/* Installs h as the active hook set (shallow copy). Passing NULL restores
 * the console defaults. Any NULL member of *h keeps its current handler
 * rather than becoming a no-op, so a caller may override just one hook. */
void ui_set_hooks(const UiHooks *h);

/* Line input used by the console defaults of confirm() and overwrite().
 * The built-in reader prints the prompt and reads a line from stdin with
 * fgets(). A program whose terminal is in raw mode (amicloud-shell's line
 * editor) or that reads its commands from stdin installs its own reader,
 * so the answer is echoed properly and no command line is swallowed as
 * an answer. The reader returns the answer's length, or -1 if no answer
 * can be read -- the prompts then take the safe choice (confirm: no,
 * overwrite: cancel). NULL restores the built-in reader. */
typedef int (*UiReadLine)(const char *prompt, char *buf, size_t bufsz);
void ui_set_line_reader(UiReadLine fn);

/* printf-style helpers used throughout cmd.c/shell.c instead of calling
 * printf()/fprintf(stderr)/a local confirm() directly. */
void ui_notify(int level, const char *fmt, ...);
int  ui_confirm(const char *fmt, ...);

/* Straight passthrough, no formatting needed for a byte count. */
int  ui_progress(long done, long total, const char *name);

/* Ctrl-C (Amiga break signal), latched: ui_break_pending() takes the
 * signal and remembers it until ui_break_clear(), so a loop over many
 * files (get -r, sync) ends instead of starting the next transfer, and
 * the HTTP layer stops waiting (it calls ui_break_set() when its
 * WaitSelect() got the signal). Interactive programs call
 * ui_break_clear() before each new command (shell prompt, GUI action).
 * On PC Ctrl-C simply ends the program, so this stays 0 there. */
int  ui_break_pending(void);
void ui_break_set(void);
void ui_break_clear(void);

/* Straight passthrough — see UiHooks.overwrite above. */
UiOverwrite ui_overwrite(int is_dir, const char *name,
                         long src_size, const char *src_date, const char *src_md5,
                         long dst_size, const char *dst_date, const char *dst_md5);

/* ------------------------------------------------------------------ */
/* Rate meter — shared bytes/sec helper for every frontend (console,     */
/* Qt, Amiga MUI) so bps math/throttling lives in exactly one place.     */
/* Based on time(NULL) (1s resolution): plain C89, already portable to   */
/* vbcc/AmigaOS, MinGW and gcc/clang alike, no platform-specific timer.  */
/* ------------------------------------------------------------------ */
typedef struct {
    long start_time;
    long last_time;
    long last_bytes;
    long bps;
} UiRateMeter;

/* Call once at the start of a transfer/new file. */
void ui_rate_reset(UiRateMeter *m);

/* Feed one done-bytes sample (cumulative, not a delta). Recomputes m->bps
 * and returns 1 at most once per elapsed wall-clock second, so callers
 * redraw a bps figure at a sane rate instead of on every 4-16KB chunk.
 * A done value smaller than the last one (a new file/transfer started
 * without an explicit ui_rate_reset()) reseeds instead of computing a
 * bogus negative rate. */
int ui_rate_sample(UiRateMeter *m, long done);

/* "12.3 KB/s" -- one decimal, because at modem speed whole KB/s mostly
 * read "0" or "1". Integer math only (no float on a 68000). */
void ui_format_rate(char *out, size_t outsz, long bps);

#endif /* UI_HOOKS_H */
