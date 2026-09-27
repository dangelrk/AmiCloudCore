/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef COMMON_SEARCH_H
#define COMMON_SEARCH_H

/*
 * search.h — local-panel side of the Commander search feature (AmiCloud docs/design/
 * search-plan.md). The cloud side just calls cmd_search()
 * (libcloudhttp) directly; this covers PANEL_LOCAL, which has no server
 * to ask and instead walks the local filesystem itself, matching each
 * entry's basename with the shared pattern.h wildcard matcher ("*", "?",
 * AmigaDOS "#?" -- same syntax cmd_search()'s server side accepts).
 *
 * Deliberately mirrors AcSearchHit (libcloudcore/entry.h) field-for-field
 * so Commander can feed cloud and local hits into the same result list.
 */

#include <stddef.h>

typedef struct SearchHit {
    char path[1024]; /* full path, '/'-joined from root down */
    int  is_dir;
    long size;
    long mtime;
} SearchHit;

typedef void (*SearchHitFn)(const SearchHit *hit, void *ud);

/* Recursively walks root (localfs_opendir/readdir), calling cb for every
 * entry whose basename matches pattern (pattern.h: pattern_compile/
 * pattern_match). Returns 1 on success (even with zero hits), 0 if root
 * itself can't be opened or the pattern fails to compile. */
int search_local(const char *root, const char *pattern, SearchHitFn cb, void *ud);

#endif /* COMMON_SEARCH_H */
