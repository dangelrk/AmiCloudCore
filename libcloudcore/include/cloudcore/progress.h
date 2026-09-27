/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef CLOUDCORE_PROGRESS_H
#define CLOUDCORE_PROGRESS_H

/*
 * progress.h — file counter x/y + byte-percentage across a whole
 * multi-file operation (wildcard get/put, get -r/put -r, sync). Split out
 * of the old client/panel.{c,h} into libcloudcore because both the copy
 * engine (common/panel.c) and libcloudsync need it — a plain
 * counter with no dependency on http/fs/sync, so core is its natural
 * home (module-encapsulation rule: shared, dependency-free state belongs
 * in core, not duplicated into whichever caller needed it first).
 */
typedef struct {
    int  done, total;    /* files attempted / files expected in total */
    int  ok, err;
    long bytes_done, bytes_total;
    long cur_file_size;  /* size of the file currently in flight, set by
                          * progress_prefix() — lets a caller reconstruct
                          * "overall bytes so far" even when the HTTP
                          * layer's own total is unknown. */
} Progress;

void progress_reset(Progress *p);

/* Called once per file, right before attempting the transfer — prints
 * "( x/y  p%) " (no newline) to stdout. No-op prefix for a single-file
 * operation (total<=1) or when ac_console_progress is off. */
void progress_prefix(Progress *p, long file_size);

/* 1 (default) = progress_prefix() prints to stdout, matching amicloud-shell/
 * CLI behaviour. GUI apps (amicloud-commander-mui/-qt) set this to 0 at
 * startup — they render progress through their own UiHooks widget instead
 * and must never punch console output through a windowed app. */
extern int ac_console_progress;

#endif /* CLOUDCORE_PROGRESS_H */
