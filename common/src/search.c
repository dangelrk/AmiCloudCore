/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#include "search.h"
#include "localfs.h"
#include "pattern.h"
#include <string.h>

/* Recursion mirrors count_local_tree() (panel.c) -- open, walk entries,
 * recurse into subdirectories -- except it matches each basename against
 * the compiled pattern instead of only summing sizes. */
static void walk(const char *dir, Pattern *pat, SearchHitFn cb, void *ud)
{
    LocalDir *d;
    LocalDirEntry e;

    d = localfs_opendir(dir);
    if (!d) return;

    while (localfs_readdir(d, &e)) {
        SearchHit hit;
        char child[1024];

        localfs_join(child, sizeof(child), dir, e.name);

        if (pattern_match(pat, e.name)) {
            strncpy(hit.path, child, sizeof(hit.path) - 1);
            hit.path[sizeof(hit.path) - 1] = '\0';
            hit.is_dir = e.is_dir;
            hit.size   = e.size;
            hit.mtime  = e.mtime;
            cb(&hit, ud);
        }
        if (e.is_dir) {
            walk(child, pat, cb, ud);
        }
    }
    localfs_closedir(d);
}

int search_local(const char *root, const char *pattern, SearchHitFn cb, void *ud)
{
    Pattern *pat;

    if (!root || !pattern || !cb) return 0;

    pat = pattern_compile(pattern);
    if (!pat) return 0;

    walk(root, pat, cb, ud);

    pattern_free(pat);
    return 1;
}
