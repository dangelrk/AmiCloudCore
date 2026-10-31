/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 *
 * test_sync.c -- host unit tests for the sync kernel (cloudsync_run_ex)
 * against an in-memory FakeTransport (fake_transport.h) and a real
 * temporary local directory.
 *
 * Two kinds of tests:
 *
 *   ok     pins the current, intended behaviour.
 *   RISK   asserts the SAFE behaviour for one of the data risks listed in
 *          AmiCloud docs/design/desktop-sync-plan.md (section 2.1 and
 *          S1-S13). Today the kernel fails these, so they are expected to
 *          fail ("XFAIL") and prove that the risk is real. Once a fix
 *          lands, the test reports XPASS, which counts as a failure until
 *          its RISK mark is removed -- so a fix can't go unnoticed and a
 *          regression can't sneak back in.
 *
 * Run: make -C libcloudsync/tests  (or "make test" in the core root).
 * VERBOSE=1 in the environment prints the kernel's ui_notify() lines.
 */
#include "sync.h"
#include "syncstate.h"
#include "localfs.h"
#include "ui_hooks.h"
#include "progress.h"
#include "fake_transport.h"
#include "charset.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utime.h>
#include <fcntl.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* Mini test framework                                                 */
/* ------------------------------------------------------------------ */
static int g_failed;          /* checks failed in the current test */
static int g_quiet_checks;    /* RISK tests: don't print each failed check */
static int g_skipped;
static int g_verbose;

#define CHECK(cond) do { if (!(cond)) { g_failed++; \
    if (!g_quiet_checks || g_verbose) printf("    %s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long _a = (long)(a), _b = (long)(b); if (_a != _b) { g_failed++; \
    if (!g_quiet_checks || g_verbose) printf("    %s:%d: %s == %ld, erwartet %s == %ld\n", \
        __FILE__, __LINE__, #a, _a, #b, _b); } } while (0)
#define SKIP(why) do { printf("    (uebersprungen: %s)\n", why); g_skipped = 1; return; } while (0)

static void quiet_notify(int level, const char *msg)
{
    if (g_verbose) printf("      [%d] %s\n", level, msg);
}
static int quiet_progress(long d, long t, const char *n) { (void)d; (void)t; (void)n; return 1; }

/* ------------------------------------------------------------------ */
/* Test environment: one local temp dir + one fake remote               */
/* ------------------------------------------------------------------ */
#define T0 1700000000L   /* fixed base time for mtimes */

typedef struct {
    char base[512];
    char local[600];
    const char *remote;
    FakeTransport f;
    SyncSummary s;
} Env;

static char g_tmproot[512];
static int  g_name_max;   /* SyncOptions.name_max for the next runs, 0 = probe */
static int (*g_virus)(const char *, long, char *, size_t);   /* SyncOptions.virus_check */
static int  g_envno;

static void rm_rf(const char *path)
{
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "chmod -R u+rwx '%s' 2>/dev/null; rm -rf '%s'", path, path);
    if (system(cmd) != 0) { /* best effort */ }
}

static void env_init(Env *e, FakeKind kind)
{
    memset(e, 0, sizeof(*e));
    snprintf(e->base, sizeof(e->base), "%s/t%03d", g_tmproot, ++g_envno);
    snprintf(e->local, sizeof(e->local), "%s/local", e->base);
    localfs_mkdir_p(e->local);
    e->remote = "/r";
    fake_init(&e->f, kind);
}

static void env_free(Env *e)
{
    fake_free(&e->f);
    rm_rf(e->base);
}

static void lpath(Env *e, const char *rel, char *out, size_t sz)
{
    localfs_join(out, sz, e->local, rel);
}

static void set_mtime(const char *path, long mtime)
{
    struct utimbuf ut;
    ut.actime = ut.modtime = (time_t)mtime;
    utime(path, &ut);
}

/* writes a local file (parents created), sets its mtime */
static void lw(Env *e, const char *rel, const char *content, long mtime)
{
    char p[1024], dir[1024];
    FILE *fp;
    char *slash;
    lpath(e, rel, p, sizeof(p));
    strcpy(dir, p);
    slash = strrchr(dir, '/');
    if (slash) { *slash = '\0'; localfs_mkdir_p(dir); }
    fp = fopen(p, "wb");
    if (!fp) { printf("    lw: %s nicht schreibbar\n", p); g_failed++; return; }
    fputs(content, fp);
    fclose(fp);
    set_mtime(p, mtime);
}

static void lmkdir(Env *e, const char *rel)
{
    char p[1024];
    lpath(e, rel, p, sizeof(p));
    localfs_mkdir_p(p);
}

/* 1 if the local file exists with exactly this content */
static int lhas(Env *e, const char *rel, const char *content)
{
    char p[1024], buf[4096];
    FILE *fp;
    size_t n;
    lpath(e, rel, p, sizeof(p));
    fp = fopen(p, "rb");
    if (!fp) return 0;
    n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    return strcmp(buf, content) == 0;
}

static int lexists(Env *e, const char *rel)
{
    char p[1024];
    lpath(e, rel, p, sizeof(p));
    return localfs_exists(p);
}

static long lmtime(Env *e, const char *rel)
{
    char p[1024];
    long size = 0, mtime = 0;
    lpath(e, rel, p, sizeof(p));
    if (!localfs_stat(p, &size, &mtime)) return -1;
    return mtime;
}

static void lrm(Env *e, const char *rel)
{
    char p[1024];
    lpath(e, rel, p, sizeof(p));
    rm_rf(p);
}

static char *rp(Env *e, const char *rel)
{
    static char buf[4][1024];
    static int k;
    k = (k + 1) & 3;
    snprintf(buf[k], sizeof(buf[k]), "%s/%s", e->remote, rel);
    return buf[k];
}

static void run_root(Env *e, const char *remote_root, int dry)
{
    Progress pr;
    int saved = -1, devnull;
    memset(&pr, 0, sizeof(pr));
    fake_reset_calls(&e->f);
    /* progress_prefix() prints straight to stdout -- hide it */
    fflush(stdout);
    if (!g_verbose && (devnull = open("/dev/null", O_WRONLY)) >= 0) {
        saved = dup(1);
        dup2(devnull, 1);
        close(devnull);
    }
    {
        SyncOptions o;
        sync_options_init(&o);
        o.dry_run = dry;
        o.verbose = 1;
        o.name_max = g_name_max;
        o.virus_check = g_virus;
        cloudsync_run_ex(&e->f.t, e->local, remote_root, &o, &pr, &e->s);
    }
    fflush(stdout);
    if (saved >= 0) { dup2(saved, 1); close(saved); }
}

static void run(Env *e)     { run_root(e, e->remote, 0); }
static void run_dry(Env *e) { run_root(e, e->remote, 1); }

/* a sync that has nothing to do at all */
static void check_idle(Env *e)
{
    run(e);
    CHECK_EQ(e->s.uploaded, 0);
    CHECK_EQ(e->s.downloaded, 0);
    CHECK_EQ(e->s.deleted, 0);
    CHECK_EQ(e->s.conflicts, 0);
    CHECK_EQ(e->s.errors, 0);
}

static int baseline_has(Env *e, const char *rel)
{
    SyncState st;
    int found;
    syncstate_load(e->local, &st);
    found = syncstate_find(&st, rel) != NULL;
    syncstate_free(&st);
    return found;
}

/* ------------------------------------------------------------------ */
/* Basic decision matrix (both backends where it matters)              */
/* ------------------------------------------------------------------ */

static void t_first_upload(FakeKind k)
{
    Env e; env_init(&e, k);
    lw(&e, "a.txt", "alpha", T0);
    lw(&e, "sub/b.txt", "beta", T0 + 10);
    run(&e);
    CHECK_EQ(e.s.uploaded, 2);
    CHECK_EQ(e.s.errors, 0);
    CHECK(fake_has(&e.f, rp(&e, "a.txt"), "alpha"));
    CHECK(fake_has(&e.f, rp(&e, "sub/b.txt"), "beta"));
    CHECK(fake_find(&e.f, rp(&e, "sub/b.txt"))->mtime == T0 + 10);
    CHECK(baseline_has(&e, "sub/b.txt"));
    check_idle(&e);
    check_idle(&e);   /* WebDAV: the second run after upload fills the token */
    env_free(&e);
}
static void t_first_upload_ac(void) { t_first_upload(FAKE_AMICLOUD); }
static void t_first_upload_dav(void) { t_first_upload(FAKE_WEBDAV); }

static void t_first_download(FakeKind k)
{
    Env e; env_init(&e, k);
    fake_put_data(&e.f, rp(&e, "a.txt"), "alpha", T0);
    fake_put_data(&e.f, rp(&e, "d/e/c.txt"), "gamma", T0 + 20);
    run(&e);
    CHECK_EQ(e.s.downloaded, 2);
    CHECK_EQ(e.s.errors, 0);
    CHECK(lhas(&e, "a.txt", "alpha"));
    CHECK(lhas(&e, "d/e/c.txt", "gamma"));
    CHECK_EQ(lmtime(&e, "d/e/c.txt"), T0 + 20);
    CHECK(!lexists(&e, SYNC_PART_FILE));
    check_idle(&e);
    env_free(&e);
}
static void t_first_download_ac(void) { t_first_download(FAKE_AMICLOUD); }
static void t_first_download_dav(void) { t_first_download(FAKE_WEBDAV); }

/* both sides already have the file, no baseline yet */
static void t_no_baseline_same_content_ac(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    lw(&e, "a.txt", "same", T0);
    fake_put_data(&e.f, rp(&e, "a.txt"), "same", T0 + 100);
    run(&e);
    CHECK_EQ(e.s.unchanged, 1);
    CHECK_EQ(e.s.conflicts, 0);
    CHECK_EQ(e.s.uploaded + e.s.downloaded, 0);
    /* mtimes aligned to the older one */
    CHECK_EQ(fake_find(&e.f, rp(&e, "a.txt"))->mtime, T0);
    CHECK_EQ(lmtime(&e, "a.txt"), T0);
    check_idle(&e);
    env_free(&e);
}

/* Today: WebDAV has no content hash, so identical files without a
 * baseline are a conflict (plan section 4 changes this to a .part
 * download and a local compare). */
static void t_no_baseline_same_content_dav(void)
{
    Env e; env_init(&e, FAKE_WEBDAV);
    lw(&e, "a.txt", "same", T0);
    fake_put_data(&e.f, rp(&e, "a.txt"), "same", T0);
    run(&e);
    CHECK_EQ(e.s.conflicts, 1);
    CHECK_EQ(e.s.uploaded + e.s.downloaded, 0);
    run(&e);
    CHECK_EQ(e.s.conflicts, 1);   /* stays a conflict */
    env_free(&e);
}

static void t_no_baseline_different_content(FakeKind k)
{
    Env e; env_init(&e, k);
    lw(&e, "a.txt", "local", T0);
    fake_put_data(&e.f, rp(&e, "a.txt"), "remote", T0);
    run(&e);
    CHECK_EQ(e.s.conflicts, 1);
    CHECK_EQ(e.s.uploaded + e.s.downloaded, 0);
    CHECK(lhas(&e, "a.txt", "local"));
    CHECK(fake_has(&e.f, rp(&e, "a.txt"), "remote"));
    env_free(&e);
}
static void t_no_baseline_diff_ac(void) { t_no_baseline_different_content(FAKE_AMICLOUD); }
static void t_no_baseline_diff_dav(void) { t_no_baseline_different_content(FAKE_WEBDAV); }

static void t_local_change_uploads(FakeKind k)
{
    Env e; env_init(&e, k);
    lw(&e, "a.txt", "v1", T0);
    run(&e); run(&e);
    lw(&e, "a.txt", "v2 local", T0 + 100);
    run(&e);
    CHECK_EQ(e.s.uploaded, 1);
    CHECK_EQ(e.s.conflicts, 0);
    CHECK(fake_has(&e.f, rp(&e, "a.txt"), "v2 local"));
    check_idle(&e);
    check_idle(&e);
    env_free(&e);
}
static void t_local_change_ac(void) { t_local_change_uploads(FAKE_AMICLOUD); }
static void t_local_change_dav(void) { t_local_change_uploads(FAKE_WEBDAV); }

static void t_remote_change_downloads(FakeKind k)
{
    Env e; env_init(&e, k);
    lw(&e, "a.txt", "v1", T0);
    run(&e); run(&e);
    fake_put_data(&e.f, rp(&e, "a.txt"), "v2 remote", T0 + 100);
    run(&e);
    CHECK_EQ(e.s.downloaded, 1);
    CHECK_EQ(e.s.conflicts, 0);
    CHECK(lhas(&e, "a.txt", "v2 remote"));
    CHECK_EQ(lmtime(&e, "a.txt"), T0 + 100);
    check_idle(&e);
    env_free(&e);
}
static void t_remote_change_ac(void) { t_remote_change_downloads(FAKE_AMICLOUD); }
static void t_remote_change_dav(void) { t_remote_change_downloads(FAKE_WEBDAV); }

/* Today: changed differently on both sides = conflict, both sides kept
 * unchanged, reported again on every run (plan section 4: keep both). */
static void t_both_changed_conflict(FakeKind k)
{
    Env e; env_init(&e, k);
    SyncState st;
    lw(&e, "a.txt", "v1", T0);
    run(&e); run(&e);
    lw(&e, "a.txt", "v2 local", T0 + 100);
    fake_put_data(&e.f, rp(&e, "a.txt"), "v2 remote", T0 + 200);
    run(&e);
    CHECK_EQ(e.s.conflicts, 1);
    CHECK_EQ(e.s.uploaded + e.s.downloaded, 0);
    CHECK(lhas(&e, "a.txt", "v2 local"));
    CHECK(fake_has(&e.f, rp(&e, "a.txt"), "v2 remote"));
    syncstate_load(e.local, &st);
    CHECK(st.had_conflicts);
    syncstate_free(&st);
    run(&e);
    CHECK_EQ(e.s.conflicts, 1);
    CHECK(lhas(&e, "a.txt", "v2 local"));
    CHECK(fake_has(&e.f, rp(&e, "a.txt"), "v2 remote"));
    env_free(&e);
}
static void t_both_changed_ac(void) { t_both_changed_conflict(FAKE_AMICLOUD); }
static void t_both_changed_dav(void) { t_both_changed_conflict(FAKE_WEBDAV); }

/* AmiCloud only: both changed to the same content -> converge silently */
static void t_both_changed_same_ac(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    lw(&e, "a.txt", "v1", T0);
    run(&e);
    lw(&e, "a.txt", "v2", T0 + 100);
    fake_put_data(&e.f, rp(&e, "a.txt"), "v2", T0 + 100);
    run(&e);
    CHECK_EQ(e.s.conflicts, 0);
    CHECK_EQ(e.s.unchanged, 1);
    CHECK_EQ(e.s.uploaded + e.s.downloaded, 0);
    check_idle(&e);
    env_free(&e);
}

static void t_local_delete_propagates(FakeKind k)
{
    Env e; env_init(&e, k);
    lw(&e, "a.txt", "alpha", T0);
    lw(&e, "keep.txt", "keep", T0);
    run(&e); run(&e);
    lrm(&e, "a.txt");
    run(&e);
    CHECK_EQ(e.s.deleted, 1);
    CHECK(fake_find(&e.f, rp(&e, "a.txt")) == NULL);
    CHECK(fake_has(&e.f, rp(&e, "keep.txt"), "keep"));
    CHECK(!baseline_has(&e, "a.txt"));
    check_idle(&e);
    env_free(&e);
}
static void t_local_delete_ac(void) { t_local_delete_propagates(FAKE_AMICLOUD); }
static void t_local_delete_dav(void) { t_local_delete_propagates(FAKE_WEBDAV); }

static void t_remote_delete_propagates(FakeKind k)
{
    Env e; env_init(&e, k);
    lw(&e, "a.txt", "alpha", T0);
    run(&e); run(&e);
    fake_remove(&e.f, rp(&e, "a.txt"));
    run(&e);
    CHECK_EQ(e.s.deleted, 1);
    CHECK(!lexists(&e, "a.txt"));
    check_idle(&e);
    env_free(&e);
}
static void t_remote_delete_ac(void) { t_remote_delete_propagates(FAKE_AMICLOUD); }
static void t_remote_delete_dav(void) { t_remote_delete_propagates(FAKE_WEBDAV); }

/* Today: deleted remotely but changed locally = conflict, local file
 * kept, nothing uploaded (decision 2 of 31.10.2026: upload it again). */
static void t_remote_del_local_change_ac(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    lw(&e, "a.txt", "v1", T0);
    run(&e);
    fake_remove(&e.f, rp(&e, "a.txt"));
    lw(&e, "a.txt", "v2 local", T0 + 100);
    run(&e);
    CHECK_EQ(e.s.conflicts, 1);
    CHECK_EQ(e.s.uploaded, 0);
    CHECK(lhas(&e, "a.txt", "v2 local"));
    CHECK(fake_find(&e.f, rp(&e, "a.txt")) == NULL);
    env_free(&e);
}

/* Today: deleted locally but changed remotely = conflict, remote kept,
 * nothing downloaded (plan section 4: download it again). */
static void t_local_del_remote_change_ac(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    lw(&e, "a.txt", "v1", T0);
    run(&e);
    lrm(&e, "a.txt");
    fake_put_data(&e.f, rp(&e, "a.txt"), "v2 remote", T0 + 100);
    run(&e);
    CHECK_EQ(e.s.conflicts, 1);
    CHECK_EQ(e.s.downloaded, 0);
    CHECK(!lexists(&e, "a.txt"));
    CHECK(fake_has(&e.f, rp(&e, "a.txt"), "v2 remote"));
    env_free(&e);
}

/* Today: file vs. directory = conflict, both untouched */
static void t_file_vs_dir_ac(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    lw(&e, "x", "a file", T0);
    fake_mkdir(&e.f, rp(&e, "x"), T0);
    fake_put_data(&e.f, rp(&e, "x/in.txt"), "inside", T0);
    run(&e);
    CHECK(e.s.conflicts >= 1);
    CHECK(lhas(&e, "x", "a file"));
    CHECK(fake_has(&e.f, rp(&e, "x/in.txt"), "inside"));
    env_free(&e);
}

static void t_dirs_created_both_ways(FakeKind k)
{
    Env e; env_init(&e, k);
    lmkdir(&e, "emptylocal");
    fake_mkdir(&e.f, rp(&e, "emptyremote"), T0);
    run(&e);
    CHECK_EQ(e.s.errors, 0);
    CHECK(fake_find(&e.f, rp(&e, "emptylocal")) && fake_find(&e.f, rp(&e, "emptylocal"))->is_dir);
    CHECK(localfs_is_dir(e.local) && lexists(&e, "emptyremote"));
    check_idle(&e);
    env_free(&e);
}
static void t_dirs_both_ac(void) { t_dirs_created_both_ways(FAKE_AMICLOUD); }
static void t_dirs_both_dav(void) { t_dirs_created_both_ways(FAKE_WEBDAV); }

/* a locally deleted tree is deleted remotely leaf-first */
static void t_local_tree_delete(FakeKind k)
{
    Env e; env_init(&e, k);
    lw(&e, "d/a.txt", "a", T0);
    lw(&e, "d/e/b.txt", "b", T0);
    run(&e); run(&e);
    lrm(&e, "d");
    run(&e);
    CHECK_EQ(e.s.errors, 0);
    CHECK_EQ(e.s.deleted, 4);
    CHECK(fake_find(&e.f, rp(&e, "d")) == NULL);
    check_idle(&e);
    env_free(&e);
}
static void t_tree_delete_ac(void) { t_local_tree_delete(FAKE_AMICLOUD); }
static void t_tree_delete_dav(void) { t_local_tree_delete(FAKE_WEBDAV); }

static void t_remote_tree_delete(FakeKind k)
{
    Env e; env_init(&e, k);
    lw(&e, "d/a.txt", "a", T0);
    lw(&e, "d/e/b.txt", "b", T0);
    run(&e); run(&e);
    fake_remove(&e.f, rp(&e, "d"));
    run(&e);
    CHECK_EQ(e.s.errors, 0);
    CHECK_EQ(e.s.deleted, 4);
    CHECK(!lexists(&e, "d"));
    env_free(&e);
}
static void t_rtree_delete_ac(void) { t_remote_tree_delete(FAKE_AMICLOUD); }
static void t_rtree_delete_dav(void) { t_remote_tree_delete(FAKE_WEBDAV); }

/* same content, different mtime after a sync: aligned, no transfer */
static void t_mtime_align_ac(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    char p[1024];
    lw(&e, "a.txt", "alpha", T0 + 500);
    run(&e);
    fake_find(&e.f, rp(&e, "a.txt"))->mtime = T0;  /* touch remotely, no content change */
    e.f.global++;
    run(&e);
    CHECK_EQ(e.s.uploaded + e.s.downloaded, 0);
    lpath(&e, "a.txt", p, sizeof(p));
    CHECK_EQ(lmtime(&e, "a.txt"), T0);
    env_free(&e);
}

static void t_dry_run_changes_nothing(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    lw(&e, "a.txt", "alpha", T0);
    fake_put_data(&e.f, rp(&e, "b.txt"), "beta", T0);
    run_dry(&e);
    CHECK_EQ(e.s.uploaded, 1);
    CHECK_EQ(e.s.downloaded, 1);
    CHECK(fake_find(&e.f, rp(&e, "a.txt")) == NULL);
    CHECK(!lexists(&e, "b.txt"));
    CHECK(!lexists(&e, SYNCSTATE_FILE));
    CHECK_EQ(e.f.calls[FOP_PUT] + e.f.calls[FOP_GET] + e.f.calls[FOP_DEL] + e.f.calls[FOP_MKDIR], 0);
    env_free(&e);
}

/* the kernel's own files are never synced */
static void t_own_files_excluded(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    lw(&e, "a.txt", "alpha", T0);
    lw(&e, SYNC_PART_FILE, "half", T0);
    lw(&e, "sub/" SYNC_PART_FILE, "half", T0);
    run(&e);
    CHECK(fake_find(&e.f, rp(&e, SYNCSTATE_FILE)) == NULL);
    CHECK(fake_find(&e.f, rp(&e, SYNC_PART_FILE)) == NULL);
    CHECK(fake_find(&e.f, rp(&e, "sub/" SYNC_PART_FILE)) == NULL);
    CHECK_EQ(e.s.uploaded, 1);
    env_free(&e);
}

/* a failed upload rolls its baseline entry back and is retried */
static void t_failed_upload_retried(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    lw(&e, "a.txt", "alpha", T0);
    fake_fail(&e.f, FOP_PUT, rp(&e, "a.txt"));
    run(&e);
    CHECK_EQ(e.s.errors, 1);
    CHECK(!baseline_has(&e, "a.txt"));
    fake_fail(&e.f, FOP_PUT, NULL);
    run(&e);
    CHECK_EQ(e.s.uploaded, 1);
    CHECK(fake_has(&e.f, rp(&e, "a.txt"), "alpha"));
    env_free(&e);
}

/* a failed download leaves the local file and no .part behind */
static void t_failed_download_safe(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    lw(&e, "a.txt", "v1", T0);
    run(&e);
    fake_put_data(&e.f, rp(&e, "a.txt"), "v2 remote", T0 + 100);
    fake_fail(&e.f, FOP_GET, rp(&e, "a.txt"));
    run(&e);
    CHECK_EQ(e.s.errors, 1);
    CHECK(lhas(&e, "a.txt", "v1"));
    CHECK(!lexists(&e, SYNC_PART_FILE));
    env_free(&e);
}

/* AmiCloud: unchanged global_revision -> no remote listing at all */
static void t_delta_shortcut_ac(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    lw(&e, "a.txt", "alpha", T0);
    lw(&e, "d/b.txt", "beta", T0);
    run(&e);
    run(&e);   /* S9: own uploads moved the counter -> one full scan */
    CHECK(e.f.calls[FOP_LIST] > 0);
    run(&e);
    CHECK_EQ(e.f.calls[FOP_LIST], 0);
    lw(&e, "c.txt", "new local", T0);   /* a local change still uploads */
    run(&e);
    CHECK_EQ(e.f.calls[FOP_LIST], 0);
    CHECK_EQ(e.s.uploaded, 1);
    fake_put_data(&e.f, rp(&e, "a.txt"), "remote change", T0 + 50);
    run(&e);
    CHECK(e.f.calls[FOP_LIST] > 0);
    CHECK(lhas(&e, "a.txt", "remote change"));
    env_free(&e);
}

/* an unresolved conflict disables the delta shortcut */
static void t_conflict_disables_delta_ac(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    lw(&e, "a.txt", "v1", T0);
    run(&e);
    lw(&e, "a.txt", "v2 local", T0 + 100);
    fake_put_data(&e.f, rp(&e, "a.txt"), "v2 remote", T0 + 100);
    run(&e);
    CHECK_EQ(e.s.conflicts, 1);
    run(&e);
    CHECK(e.f.calls[FOP_LIST] > 0);
    CHECK_EQ(e.s.conflicts, 1);
    env_free(&e);
}

/* WebDAV: a download stores the real local hash in the baseline, so the
 * next run does not upload the same content back */
static void t_dav_no_flipflop(void)
{
    Env e; env_init(&e, FAKE_WEBDAV);
    fake_put_data(&e.f, rp(&e, "a.txt"), "alpha", T0);
    run(&e);
    CHECK_EQ(e.s.downloaded, 1);
    check_idle(&e);
    lw(&e, "b.txt", "beta", T0);
    run(&e);
    CHECK_EQ(e.s.uploaded, 1);
    check_idle(&e);
    check_idle(&e);
    CHECK_EQ(e.f.calls[FOP_PUT] + e.f.calls[FOP_GET], 0);
    env_free(&e);
}

/* Today (S4 changes this): a missing local root is created and filled */
static void t_missing_root_created(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    fake_put_data(&e.f, rp(&e, "a.txt"), "alpha", T0);
    rm_rf(e.local);
    run(&e);
    CHECK(localfs_is_dir(e.local));
    CHECK(lhas(&e, "a.txt", "alpha"));
    env_free(&e);
}

static void t_syncstate_roundtrip(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    SyncState st, st2;
    SyncStateEntry *x;
    memset(&st, 0, sizeof(st));
    st.global_revision = 4711;
    st.had_conflicts = 1;
    syncstate_upsert(&st, "a/b c/\xC3\xA4.txt", 0, 12, T0, "0123456789abcdef0123456789abcdef",
                     "\"etag\"", 3, 99, T0 + 1);
    syncstate_upsert(&st, "dir", 1, 0, T0, "", "", 1, 0, T0);
    CHECK(syncstate_save(e.local, &st));
    CHECK(syncstate_load(e.local, &st2));
    CHECK_EQ(st2.n, 2);
    CHECK_EQ(st2.global_revision, 4711);
    CHECK_EQ(st2.had_conflicts, 1);
    x = syncstate_find(&st2, "a/b c/\xC3\xA4.txt");
    CHECK(x != NULL);
    if (x) {
        CHECK_EQ(x->size, 12);
        CHECK_EQ(x->revision, 3);
        CHECK_EQ(x->server_id, 99);
        CHECK(strcmp(x->token, "\"etag\"") == 0);
    }
    syncstate_remove(&st2, "dir");
    CHECK(syncstate_find(&st2, "dir") == NULL);
    syncstate_free(&st);
    syncstate_free(&st2);
    env_free(&e);
}

/* a corrupt baseline counts as "no baseline" (AmiCloud: harmless, the
 * hashes match again) */
static void t_corrupt_baseline_ac(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    lw(&e, "a.txt", "alpha", T0);
    run(&e);
    lw(&e, SYNCSTATE_FILE, "{ kaputt", T0);
    run(&e);
    CHECK_EQ(e.s.conflicts, 0);
    CHECK_EQ(e.s.deleted, 0);
    CHECK(lhas(&e, "a.txt", "alpha"));
    CHECK(fake_has(&e.f, rp(&e, "a.txt"), "alpha"));
    env_free(&e);
}

/* ------------------------------------------------------------------ */
/* RISK tests: safe behaviour that the kernel does NOT deliver today    */
/* ------------------------------------------------------------------ */

/* S1: a failing remote list() of a subdirectory makes that subtree look
 * deleted remotely -> its local files are deleted. */
static void r_list_error_deletes_local(void)
{
    Env e; env_init(&e, FAKE_WEBDAV);
    lw(&e, "sub/f.txt", "precious", T0);
    lw(&e, "top.txt", "top", T0);
    run(&e); run(&e);
    fake_fail(&e.f, FOP_LIST, rp(&e, "sub"));
    run(&e);
    CHECK(lhas(&e, "sub/f.txt", "precious"));
    CHECK(e.s.errors > 0);
    env_free(&e);
}

/* S1, AmiCloud: same once global_revision moved (any remote change) */
static void r_list_error_deletes_local_ac(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    lw(&e, "sub/f.txt", "precious", T0);
    run(&e);
    fake_put_data(&e.f, rp(&e, "other.txt"), "x", T0);
    fake_fail(&e.f, FOP_LIST, rp(&e, "sub"));
    run(&e);
    CHECK(lhas(&e, "sub/f.txt", "precious"));
    env_free(&e);
}

/* S1: an unreadable local directory makes its subtree look deleted
 * locally -> deleted on the server. */
static void r_opendir_error_deletes_remote(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    char p[1024];
    if (geteuid() == 0) { env_free(&e); SKIP("laeuft als root"); }
    lw(&e, "sub/f.txt", "precious", T0);
    run(&e);
    lpath(&e, "sub", p, sizeof(p));
    chmod(p, 0);
    run(&e);
    chmod(p, 0755);
    CHECK(fake_has(&e.f, rp(&e, "sub/f.txt"), "precious"));
    env_free(&e);
}

/* S5: WebDAV DELETE of a locally deleted directory also removes a file
 * that someone else added to it on the server meanwhile. */
static void r_dav_dir_delete_takes_new_file(void)
{
    Env e; env_init(&e, FAKE_WEBDAV);
    lw(&e, "d/a.txt", "a", T0);
    run(&e); run(&e);
    lrm(&e, "d");
    fake_put_data(&e.f, rp(&e, "d/new.txt"), "added by someone else", T0 + 100);
    run(&e);
    CHECK(fake_has(&e.f, rp(&e, "d/new.txt"), "added by someone else") ||
          lhas(&e, "d/new.txt", "added by someone else"));
    env_free(&e);
}

/* S5, other direction: a directory deleted on the server while a new
 * file was added to it locally -- the directory comes back on the
 * server and the new file is uploaded, nothing is lost */
static void t_local_new_file_in_remotely_deleted_dir(FakeKind k)
{
    Env e; env_init(&e, k);
    lw(&e, "d/a.txt", "a", T0);
    run(&e); run(&e);
    fake_remove(&e.f, rp(&e, "d"));
    lw(&e, "d/new.txt", "added locally", T0 + 100);
    run(&e);
    CHECK_EQ(e.s.errors, 0);
    CHECK(lhas(&e, "d/new.txt", "added locally"));
    CHECK(fake_has(&e.f, rp(&e, "d/new.txt"), "added locally"));
    CHECK(!lexists(&e, "d/a.txt"));
    check_idle(&e);
    env_free(&e);
}
static void t_s5_local_ac(void) { t_local_new_file_in_remotely_deleted_dir(FAKE_AMICLOUD); }
static void t_s5_local_dav(void) { t_local_new_file_in_remotely_deleted_dir(FAKE_WEBDAV); }

/* S13: fixed exclusion list, both sides */
static void t_exclusions(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    lw(&e, ".DS_Store", "x", T0);
    lw(&e, "sub/._a.txt", "x", T0);
    lw(&e, "~$brief.doc", "x", T0);
    lw(&e, "film.part", "x", T0);
    lw(&e, "Thumbs.db", "x", T0);
    lw(&e, "back\\slash.txt", "x", T0);
    lw(&e, "ok.txt", "ok", T0);
    fake_put_data(&e.f, rp(&e, "desktop.ini"), "x", T0);
    fake_put_data(&e.f, rp(&e, "remote.part"), "x", T0);
    run(&e);
    CHECK_EQ(e.s.uploaded, 1);
    CHECK_EQ(e.s.downloaded, 0);
    CHECK(fake_has(&e.f, rp(&e, "ok.txt"), "ok"));
    CHECK(!lexists(&e, "desktop.ini"));
    CHECK(fake_find(&e.f, rp(&e, ".DS_Store")) == NULL);
    CHECK(sync_name_excluded(".amicloudsync.tmp"));
    CHECK(!sync_name_excluded("Disk.info"));
    CHECK(!sync_name_excluded("partitur.txt"));
    env_free(&e);
}

/* same scenario on AmiCloud: the server refuses the non-empty delete,
 * nothing is lost (pins today's behaviour, not a RISK test) */
static void t_ac_dir_delete_keeps_new_file(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    lw(&e, "d/a.txt", "a", T0);
    run(&e);
    lrm(&e, "d");
    fake_put_data(&e.f, rp(&e, "d/new.txt"), "added by someone else", T0 + 100);
    run(&e);
    CHECK(fake_has(&e.f, rp(&e, "d/new.txt"), "added by someone else"));
    env_free(&e);
}

/* S3: the baseline does not know which server/folder it belongs to.
 * Pointing the same local folder at another (empty) remote folder makes
 * every local file look "deleted remotely". */
static void r_identity_change_deletes_local(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    lw(&e, "a.txt", "precious", T0);
    run(&e);
    run_root(&e, "/other", 0);
    CHECK(lhas(&e, "a.txt", "precious"));
    env_free(&e);
}

/* S3: a baseline in another format (or the old one without a format
 * number) is discarded: first sync, nothing deleted */
static void t_foreign_format(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    lw(&e, "a.txt", "alpha", T0);
    lw(&e, SYNCSTATE_FILE, "{ \"global_revision\": 1, \"entries\": [ { \"path\": \"gone.txt\", "
       "\"is_dir\": false, \"md5\": \"x\" } ] }", T0);
    fake_put_data(&e.f, rp(&e, "gone.txt"), "remote only", T0);
    run(&e);
    CHECK_EQ(e.s.deleted, 0);
    CHECK(lhas(&e, "gone.txt", "remote only"));
    CHECK(fake_has(&e.f, rp(&e, "a.txt"), "alpha"));
    env_free(&e);
}

/* S4: with a baseline, a vanished remote root is not recreated empty
 * (which would delete everything locally) -- the run stops */
static void t_remote_root_gone(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    lw(&e, "a.txt", "precious", T0);
    run(&e);
    fake_remove(&e.f, e.remote);
    run(&e);
    CHECK(e.s.aborted);
    CHECK(lhas(&e, "a.txt", "precious"));
    CHECK(fake_find(&e.f, e.remote) == NULL);
    env_free(&e);
}

/* a dry run of a new pair whose remote folder does not exist yet */
static void t_dry_run_new_pair(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    lw(&e, "a.txt", "alpha", T0);
    run_dry(&e);
    CHECK(!e.s.aborted);
    CHECK_EQ(e.s.uploaded, 1);
    CHECK(fake_find(&e.f, e.remote) == NULL);
    env_free(&e);
}

typedef struct Race { FakeOp op; int fired; Env *e; const char *rel; const char *content; int local; } Race;
static void race_hook(FakeTransport *f, FakeOp op, const char *path, void *ud);

/* S2: Ctrl-C / Abbrechen is never checked -- a pending break still runs
 * every transfer. */
static void r_break_ignored(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    lw(&e, "a.txt", "alpha", T0);
    lw(&e, "b.txt", "beta", T0);
    ui_break_set();
    run(&e);
    ui_break_clear();
    CHECK_EQ(e.f.calls[FOP_PUT], 0);
    env_free(&e);
}

/* S6: two remote names differing only in case on a case-insensitive
 * local file system: one overwrites the other locally, and the next run
 * uploads the wrong content and deletes the other remote file. */
static void r_case_collision(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    char p1[1024], p2[1024];
    lw(&e, "CaseProbe", "x", T0);
    lpath(&e, "caseprobe", p1, sizeof(p1));
    lpath(&e, "CaseProbe", p2, sizeof(p2));
    if (!localfs_exists(p1)) { env_free(&e); SKIP("Dateisystem unterscheidet Gross/Klein"); }
    remove(p2);
    fake_put_data(&e.f, rp(&e, "A.txt"), "upper", T0);
    fake_put_data(&e.f, rp(&e, "a.txt"), "lower", T0);
    run(&e);
    run(&e);
    run(&e);
    CHECK(fake_has(&e.f, rp(&e, "A.txt"), "upper"));
    CHECK(fake_has(&e.f, rp(&e, "a.txt"), "lower"));
    CHECK_EQ(e.s.conflicts, 1);
    CHECK_EQ(e.f.calls[FOP_GET] + e.f.calls[FOP_PUT] + e.f.calls[FOP_DEL], 0);
    env_free(&e);
}

static int case_insensitive_fs(Env *e)
{
    char p1[1024], p2[1024];
    int r;
    lw(e, "CaseProbe", "x", T0);
    lpath(e, "caseprobe", p1, sizeof(p1));
    lpath(e, "CaseProbe", p2, sizeof(p2));
    r = localfs_exists(p1);
    remove(p2);
    return r;
}

/* S6 with directories: "Dir/x" and "dir/y" would land in one local
 * directory; the next run would move y on the server */
static void t_case_collision_dirs(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    if (!case_insensitive_fs(&e)) { env_free(&e); SKIP("Dateisystem unterscheidet Gross/Klein"); }
    fake_put_data(&e.f, rp(&e, "Dir/x.txt"), "x", T0);
    fake_put_data(&e.f, rp(&e, "dir/y.txt"), "y", T0);
    fake_put_data(&e.f, rp(&e, "fine.txt"), "fine", T0);
    run(&e);
    run(&e);
    CHECK(fake_has(&e.f, rp(&e, "Dir/x.txt"), "x"));
    CHECK(fake_has(&e.f, rp(&e, "dir/y.txt"), "y"));
    CHECK(fake_find(&e.f, rp(&e, "Dir/y.txt")) == NULL);
    CHECK(lhas(&e, "fine.txt", "fine"));
    CHECK(!lexists(&e, "Dir"));
    env_free(&e);
}

/* S7: a file edited after the scan is not deleted */
static void t_delete_keeps_fresh_edit(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    Race r;
    lw(&e, "a.txt", "v1", T0);
    lw(&e, "b.txt", "b", T0);
    run(&e);
    fake_remove(&e.f, rp(&e, "a.txt"));
    fake_put_data(&e.f, rp(&e, "b.txt"), "b2", T0 + 50);
    memset(&r, 0, sizeof(r));
    r.op = FOP_GET; r.e = &e; r.rel = "a.txt"; r.content = "fresh local edit"; r.local = 1;
    e.f.hook = race_hook; e.f.hook_ud = &r;
    run(&e);   /* downloads b.txt (hook edits a.txt), then would delete a.txt */
    e.f.hook = NULL;
    CHECK(r.fired);
    CHECK(lhas(&e, "a.txt", "fresh local edit"));
    env_free(&e);
}

/* S14: a failed download rolls back its baseline entry, but the run
 * still stores the new global_revision. The next run takes the delta
 * shortcut ("nothing changed remotely") and never retries; a later
 * local edit then overwrites the newer server version. */
static void r_failed_download_never_retried(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    lw(&e, "a.txt", "v1", T0);
    run(&e);
    fake_put_data(&e.f, rp(&e, "a.txt"), "v2 remote", T0 + 100);
    fake_fail(&e.f, FOP_GET, rp(&e, "a.txt"));
    run(&e);
    fake_fail(&e.f, FOP_GET, NULL);
    run(&e);
    CHECK(lhas(&e, "a.txt", "v2 remote"));
    env_free(&e);
}

/* S2: a break during the transfers stops the run: no deletions, the
 * baseline keeps what was done, the next run completes the rest */
static void break_hook(FakeTransport *f, FakeOp op, const char *path, void *ud)
{
    (void)f; (void)path; (void)ud;
    if (op == FOP_PUT) ui_break_set();
}

static void t_break_mid_run(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    lw(&e, "old.txt", "to be deleted", T0);
    run(&e);
    lrm(&e, "old.txt");
    lw(&e, "a.txt", "alpha", T0);
    lw(&e, "b.txt", "beta", T0);
    e.f.hook = break_hook;
    run(&e);
    e.f.hook = NULL;
    ui_break_clear();
    CHECK(e.s.aborted);
    CHECK_EQ(e.f.calls[FOP_PUT], 1);
    CHECK_EQ(e.f.calls[FOP_DEL], 0);
    CHECK(fake_has(&e.f, rp(&e, "old.txt"), "to be deleted"));
    run(&e);
    CHECK(!e.s.aborted);
    CHECK_EQ(e.s.errors, 0);
    CHECK(fake_has(&e.f, rp(&e, "a.txt"), "alpha"));
    CHECK(fake_has(&e.f, rp(&e, "b.txt"), "beta"));
    CHECK(fake_find(&e.f, rp(&e, "old.txt")) == NULL);
    env_free(&e);
}

/* hook helpers for the race tests: fire once on the first op of a kind */

static void race_hook(FakeTransport *f, FakeOp op, const char *path, void *ud)
{
    Race *r = (Race *)ud;
    (void)f; (void)path;
    if (r->fired || op != r->op) return;
    r->fired = 1;
    if (r->local) lw(r->e, r->rel, r->content, T0 + 999);
    else fake_put_data(&r->e->f, rp(r->e, r->rel), r->content, T0 + 999);
}

/* S9: global_revision is read at the END of a run. A change by another
 * client during the run is then covered by the stored counter, and the
 * delta shortcut never sees it. */
static void r_global_rev_at_end(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    Race r;
    lw(&e, "a.txt", "v1", T0);
    run(&e);
    lw(&e, "b.txt", "local new", T0);
    memset(&r, 0, sizeof(r));
    r.op = FOP_PUT; r.e = &e; r.rel = "a.txt"; r.content = "changed by other client"; r.local = 0;
    e.f.hook = race_hook; e.f.hook_ud = &r;
    run(&e);
    e.f.hook = NULL;
    run(&e);
    CHECK(lhas(&e, "a.txt", "changed by other client"));
    env_free(&e);
}

/* S7: the local file is edited after the scan, before the download
 * replaces it -> the edit is overwritten. */
static void r_download_overwrites_fresh_edit(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    Race r;
    lw(&e, "a.txt", "v1", T0);
    run(&e);
    fake_put_data(&e.f, rp(&e, "a.txt"), "v2 remote", T0 + 100);
    memset(&r, 0, sizeof(r));
    r.op = FOP_GET; r.e = &e; r.rel = "a.txt"; r.content = "fresh local edit"; r.local = 1;
    e.f.hook = race_hook; e.f.hook_ud = &r;
    run(&e);
    e.f.hook = NULL;
    run(&e);
    CHECK(lhas(&e, "a.txt", "fresh local edit") ||
          fake_has(&e.f, rp(&e, "a.txt"), "fresh local edit"));
    env_free(&e);
}

/* S8: the upload has no revision condition -> a remote change made after
 * the scan is overwritten. */
static void r_upload_overwrites_remote_change(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    Race r;
    lw(&e, "a.txt", "v1", T0);
    run(&e);
    lw(&e, "a.txt", "v2 local", T0 + 100);
    memset(&r, 0, sizeof(r));
    r.op = FOP_PUT; r.e = &e; r.rel = "a.txt"; r.content = "v2 other client"; r.local = 0;
    e.f.hook = race_hook; e.f.hook_ud = &r;
    run(&e);
    e.f.hook = NULL;
    CHECK(fake_has(&e.f, rp(&e, "a.txt"), "v2 other client") ||
          lhas(&e, "a.txt", "v2 other client"));
    env_free(&e);
}

/* S8: a new local file must not overwrite a file that someone created on
 * the server after the scan */
static void t_create_only(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    Race r;
    lw(&e, "a.txt", "mine", T0);
    memset(&r, 0, sizeof(r));
    r.op = FOP_PUT; r.e = &e; r.rel = "a.txt"; r.content = "theirs"; r.local = 0;
    e.f.hook = race_hook; e.f.hook_ud = &r;
    run(&e);
    e.f.hook = NULL;
    CHECK(fake_has(&e.f, rp(&e, "a.txt"), "theirs"));
    CHECK(lhas(&e, "a.txt", "mine"));
    CHECK_EQ(e.s.conflicts, 1);
    env_free(&e);
}

static void t_dav_if_match(void)
{
    Env e; env_init(&e, FAKE_WEBDAV);
    Race r;
    lw(&e, "a.txt", "v1", T0);
    run(&e); run(&e);
    lw(&e, "a.txt", "v2 local", T0 + 100);
    memset(&r, 0, sizeof(r));
    r.op = FOP_PUT; r.e = &e; r.rel = "a.txt"; r.content = "v2 other client"; r.local = 0;
    e.f.hook = race_hook; e.f.hook_ud = &r;
    run(&e);
    e.f.hook = NULL;
    CHECK(fake_has(&e.f, rp(&e, "a.txt"), "v2 other client"));
    CHECK(lhas(&e, "a.txt", "v2 local"));
    env_free(&e);
}

/* S10: after an upload the baseline holds the server's revision, so the
 * next full scan (S9) sees no remote change and transfers nothing */
static void t_put_result(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    SyncState st;
    SyncStateEntry *x;
    lw(&e, "a.txt", "v1", T0);
    run(&e);
    fake_put_data(&e.f, rp(&e, "other.txt"), "x", T0);   /* revisions now differ from a guess */
    lw(&e, "a.txt", "v2", T0 + 10);
    run(&e);
    lw(&e, "a.txt", "v3", T0 + 20);
    run(&e);
    syncstate_load(e.local, &st);
    x = syncstate_find(&st, "a.txt");
    CHECK(x && x->revision == fake_find(&e.f, rp(&e, "a.txt"))->revision);
    CHECK(x && x->server_id == fake_find(&e.f, rp(&e, "a.txt"))->id);
    syncstate_free(&st);
    check_idle(&e);
    env_free(&e);
}

/* S12: the baseline stays sorted through inserts and removes in any order */
static void t_syncstate_sorted(void)
{
    SyncState st;
    char p[32];
    int i, ok = 1;
    memset(&st, 0, sizeof(st));
    for (i = 0; i < 500; i++) {
        snprintf(p, sizeof(p), "d%03d/f%03d", (i * 7919) % 500, i % 13);
        syncstate_upsert(&st, p, 0, i, T0, "", "", 1, 0, T0);
    }
    for (i = 0; i < 500; i += 3) {
        snprintf(p, sizeof(p), "d%03d/f%03d", (i * 7919) % 500, i % 13);
        syncstate_remove(&st, p);
    }
    for (i = 1; i < st.n; i++)
        if (strcmp(st.items[i - 1].path, st.items[i].path) >= 0) ok = 0;
    CHECK(ok);
    for (i = 0; i < 500; i++) {
        snprintf(p, sizeof(p), "d%03d/f%03d", (i * 7919) % 500, i % 13);
        CHECK((syncstate_find(&st, p) != NULL) == (i % 3 != 0));
    }
    syncstate_free(&st);
}

/* S11: a checkpoint during a long run holds exactly what is done --
 * read back from disk at the 70th upload, as after a crash */
typedef struct { Env *e; int puts; int entries; int has_first; int has_last; long grev; } Cp;

static void cp_hook(FakeTransport *f, FakeOp op, const char *path, void *ud)
{
    Cp *c = (Cp *)ud;
    SyncState st;
    (void)f; (void)path;
    if (op != FOP_PUT || ++c->puts != 70) return;
    syncstate_load(c->e->local, &st);
    c->entries = st.n;
    c->has_first = syncstate_find(&st, "f000.txt") != NULL;
    c->has_last = syncstate_find(&st, "f119.txt") != NULL;
    c->grev = st.global_revision;
    syncstate_free(&st);
}

static void t_checkpoint(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    Cp c;
    char rel[32];
    int i;
    lw(&e, "keep.txt", "k", T0);
    run(&e);
    for (i = 0; i < 120; i++) {
        snprintf(rel, sizeof(rel), "f%03d.txt", i);
        lw(&e, rel, rel, T0);
    }
    memset(&c, 0, sizeof(c));
    c.e = &e;
    e.f.hook = cp_hook; e.f.hook_ud = &c;
    run(&e);
    e.f.hook = NULL;
    CHECK_EQ(c.entries, 1 + 50);          /* keep.txt + the first 50 uploads */
    CHECK(c.has_first);
    CHECK(!c.has_last);
    CHECK(c.grev != 0);                    /* the OLD counter of the first run */
    CHECK_EQ(e.s.uploaded, 120);
    check_idle(&e);
    env_free(&e);
}

/* S15: on a file system with 30-character names a longer server name is
 * skipped with everything below it; nothing is uploaded under a short
 * name, nothing deleted on the server */
static void t_long_names(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    g_name_max = 30;
    fake_put_data(&e.f, rp(&e, "A23456789012345678901234567890"), "thirty", T0);
    fake_put_data(&e.f, rp(&e, "B234567890123456789012345678901"), "thirty-one", T0);
    fake_put_data(&e.f, rp(&e, "LongDirectoryName_123456789012345/in.txt"), "inside", T0);
    run(&e);
    CHECK_EQ(e.s.downloaded, 1);
    CHECK_EQ(e.s.skipped, 2);
    CHECK_EQ(e.s.conflicts, 0);
    CHECK(lhas(&e, "A23456789012345678901234567890", "thirty"));
    run(&e);
    run(&e);
    CHECK_EQ(e.f.calls[FOP_PUT] + e.f.calls[FOP_DEL], 0);
    CHECK(fake_has(&e.f, rp(&e, "B234567890123456789012345678901"), "thirty-one"));
    CHECK(fake_has(&e.f, rp(&e, "LongDirectoryName_123456789012345/in.txt"), "inside"));
    g_name_max = 0;
    env_free(&e);
}

/* a larger tree: first sync, then an idle run (was O(n^2) before S12) */
static void t_many_files(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    char rel[64];
    int i;
    for (i = 0; i < 2000; i++) {
        snprintf(rel, sizeof(rel), "dir%02d/file%04d.txt", i % 40, i);
        fake_put_data(&e.f, rp(&e, rel), "x", T0);
    }
    run(&e);
    CHECK_EQ(e.s.downloaded, 2000);
    run(&e);
    CHECK_EQ(e.s.downloaded + e.s.uploaded + e.s.deleted, 0);
    env_free(&e);
}

/* ===== From the independent review (31.10.2026) ===== */

/* server: a.txt deleted, b.txt renamed to a.txt -- the revision stays 1,
 * the file ID changes; must be seen as a remote change */
static void t_rename_same_revision(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    FakeNode *n;
    lw(&e, "a.txt", "AAAA-old", T0);
    lw(&e, "b.txt", "BBBB-valuable", T0);
    run(&e); run(&e);
    fake_remove(&e.f, rp(&e, "a.txt"));
    n = fake_find(&e.f, rp(&e, "b.txt"));
    free(n->path); n->path = strdup("/r/a.txt"); e.f.global++;
    run(&e);
    CHECK(lhas(&e, "a.txt", "BBBB-valuable"));
    lw(&e, "a.txt", "AAAA-local-edit", T0 + 500);
    run(&e);
    CHECK(fake_has(&e.f, rp(&e, "a.txt"), "AAAA-local-edit") || fake_has(&e.f, rp(&e, "a.txt"), "BBBB-valuable"));
    env_free(&e);
}

/* macOS: directory deleted on the server, a .DS_Store in it locally */
static void t_dsstore_dir_delete(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    lw(&e, "d/a.txt", "a", T0);
    run(&e); run(&e);
    lw(&e, "d/.DS_Store", "finder", T0);
    fake_remove(&e.f, rp(&e, "d"));
    run(&e);
    CHECK_EQ(e.s.errors, 0);
    CHECK(!lexists(&e, "d"));
    check_idle(&e);
    env_free(&e);
}

/* server directory holds never-synced litter, deleted locally */
static void t_excluded_remote_in_deleted_dir(FakeKind k)
{
    Env e; env_init(&e, k);
    lw(&e, "d/a.txt", "a", T0);
    run(&e); run(&e);
    fake_put_data(&e.f, rp(&e, "d/Thumbs.db"), "win", T0);
    lrm(&e, "d");
    run(&e);
    CHECK_EQ(e.s.errors, 0);
    CHECK(fake_find(&e.f, rp(&e, "d")) == NULL);   /* litter goes with its directory */
    env_free(&e);
}
static void t_excl_ac(void)  { t_excluded_remote_in_deleted_dir(FAKE_AMICLOUD); }
static void t_excl_dav(void) { t_excluded_remote_in_deleted_dir(FAKE_WEBDAV); }

/* S5 nested: whole tree deleted locally, server has d/s/new.txt */
static void t_s5_nested(FakeKind k)
{
    Env e; env_init(&e, k);
    lw(&e, "d/a.txt", "a", T0);
    lw(&e, "d/s/b.txt", "b", T0);
    lw(&e, "d/t/c.txt", "c", T0);
    run(&e); run(&e);
    lrm(&e, "d");
    fake_put_data(&e.f, rp(&e, "d/s/new.txt"), "new remote", T0 + 50);
    run(&e);
    CHECK_EQ(e.s.errors, 0);
    CHECK(fake_has(&e.f, rp(&e, "d/s/new.txt"), "new remote"));
    CHECK(lhas(&e, "d/s/new.txt", "new remote"));
    CHECK(fake_find(&e.f, rp(&e, "d/a.txt")) == NULL);
    CHECK(fake_find(&e.f, rp(&e, "d/s/b.txt")) == NULL);
    CHECK(fake_find(&e.f, rp(&e, "d/t")) == NULL);
    check_idle(&e);
    env_free(&e);
}
static void t_s5n_ac(void)  { t_s5_nested(FAKE_AMICLOUD); }
static void t_s5n_dav(void) { t_s5_nested(FAKE_WEBDAV); }

/* S5 with a conflicting child (WebDAV): the changed file survives */
static void t_s5_conflict_child(void)
{
    Env e; env_init(&e, FAKE_WEBDAV);
    lw(&e, "d/a.txt", "a", T0);
    lw(&e, "d/b.txt", "b", T0);
    run(&e); run(&e);
    lrm(&e, "d");
    fake_put_data(&e.f, rp(&e, "d/a.txt"), "a changed remote", T0 + 50);
    run(&e);
    CHECK(fake_has(&e.f, rp(&e, "d/a.txt"), "a changed remote"));
    CHECK(fake_find(&e.f, rp(&e, "d/b.txt")) == NULL);
    run(&e);
    CHECK(fake_has(&e.f, rp(&e, "d/a.txt"), "a changed remote"));
    env_free(&e);
}

/* a file deleted locally is changed on the server during the run: the
 * delete must not take the foreign change along */
static void delrace_hook(FakeTransport *f, FakeOp op, const char *path, void *ud)
{
    (void)ud;
    if (op == FOP_PUT && strstr(path, "a_upload.txt"))
        fake_put_data(f, "/r/z_del.txt", "important foreign change", T0 + 900);
}
static void t_del_remote_race(FakeKind k)
{
    Env e; env_init(&e, k);
    lw(&e, "z_del.txt", "old", T0);
    run(&e); run(&e);
    lrm(&e, "z_del.txt");
    lw(&e, "a_upload.txt", "new", T0 + 10);
    e.f.hook = delrace_hook;
    run(&e);
    e.f.hook = NULL;
    CHECK(fake_has(&e.f, "/r/z_del.txt", "important foreign change"));
    run(&e);
    CHECK(lhas(&e, "z_del.txt", "important foreign change") ||
          fake_has(&e.f, "/r/z_del.txt", "important foreign change"));
    env_free(&e);
}
static void t_delrace_ac(void)  { t_del_remote_race(FAKE_AMICLOUD); }
static void t_delrace_dav(void) { t_del_remote_race(FAKE_WEBDAV); }

/* a new file appears in a directory that is about to be deleted on the
 * server (WebDAV DELETE is recursive) */
static void dirrace_hook(FakeTransport *f, FakeOp op, const char *path, void *ud)
{
    (void)ud;
    if (op == FOP_PUT && strstr(path, "a_upload.txt"))
        fake_put_data(f, "/r/d/late.txt", "added during the run", T0 + 900);
}
static void t_dir_delete_race_dav(void)
{
    Env e; env_init(&e, FAKE_WEBDAV);
    lw(&e, "d/x.txt", "x", T0);
    run(&e); run(&e);
    lrm(&e, "d");
    lw(&e, "a_upload.txt", "new", T0 + 10);
    e.f.hook = dirrace_hook;
    run(&e);
    e.f.hook = NULL;
    CHECK(fake_has(&e.f, "/r/d/late.txt", "added during the run"));
    env_free(&e);
}

/* a missing local root whose parent is gone too (unmounted drive) */
static void t_local_parent_missing(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    char deep[700];
    fake_put_data(&e.f, rp(&e, "a.txt"), "a", T0);
    snprintf(deep, sizeof(deep), "%s/notmounted/Sync", e.base);
    {
        Progress pr; SyncOptions o;
        memset(&pr, 0, sizeof(pr));
        sync_options_init(&o);
        cloudsync_run_ex(&e.f.t, deep, e.remote, &o, &pr, &e.s);
    }
    CHECK(e.s.aborted);
    CHECK(!localfs_exists(deep));
    env_free(&e);
}

/* S11 speed: 8000 files must not take quadratic time */
static void t_perf_8000(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    char rel[64]; int i; time_t t0;
    for (i = 0; i < 8000; i++) {
        snprintf(rel, sizeof(rel), "d%02d/f%05d.txt", i % 40, i);
        fake_put_data(&e.f, rp(&e, rel), "x", T0);
    }
    t0 = time(NULL);
    run(&e);
    CHECK_EQ(e.s.downloaded, 8000);
    CHECK(time(NULL) - t0 < 20);
    env_free(&e);
}

/* ===== charset.c (Phase 4) ===== */
static void t_cs_nfc(void)
{
    char o[64];
    CHECK(cs_nfc("a\xCC\x88", o, sizeof(o)) && strcmp(o, "\xC3\xA4") == 0);          /* a+U+0308 -> ä */
    CHECK(cs_nfc("Cafe\xCC\x81!", o, sizeof(o)) && strcmp(o, "Caf\xC3\xA9!") == 0);   /* é */
    CHECK(cs_nfc("U\xCC\x88\xCC\x84", o, sizeof(o)) && strcmp(o, "\xC7\x95") == 0);   /* Ü+macron -> U+01D5 */
    CHECK(cs_nfc("s\xCC\x81", o, sizeof(o)) && strcmp(o, "\xC5\x9B") == 0);            /* ś (Polish) */
    CHECK(cs_nfc("\xC3\xA4", o, sizeof(o)) && strcmp(o, "\xC3\xA4") == 0);             /* already NFC */
    CHECK(cs_nfc("x\xCC\x88", o, sizeof(o)) && strcmp(o, "x\xCC\x88") == 0);           /* no composition */
    CHECK(!cs_nfc("abcdef", o, 4));                                                     /* truncated */
}

static void t_cs_latin1(void)
{
    char o[64], u[64];
    cs_test_mode(CS_TEST_LATIN1);
    CHECK(strcmp(cs_name(), "ISO-8859-1") == 0);
    CHECK(cs_from_utf8("Gr\xC3\xBC\xC3\x9F" "e.txt", o, sizeof(o)) == 1);
    CHECK(strcmp(o, "Gr\xFC\xDF" "e.txt") == 0);
    CHECK(cs_to_utf8(o, u, sizeof(u)) && strcmp(u, "Gr\xC3\xBC\xC3\x9F" "e.txt") == 0);
    CHECK(cs_from_utf8("Preis\xE2\x82\xAC.txt", o, sizeof(o)) == 0);                    /* EUR: lossy */
    CHECK(strcmp(o, "Preis_.txt") == 0);
    CHECK(cs_from_utf8("a\xCC\x88", o, sizeof(o)) == 0 && strcmp(o, "\xE4") == 0);      /* NFD: not exact */
    CHECK(cs_from_utf8("bad\xFFname", o, sizeof(o)) == 0 && strcmp(o, "bad_name") == 0); /* invalid UTF-8 */
    CHECK(cs_from_utf8("plain", o, sizeof(o)) == 1 && strcmp(o, "plain") == 0);
    cs_test_mode(CS_TEST_DEFAULT);
}

static void t_cs_utf8_default(void)
{
    char o[64];
    CHECK(strcmp(cs_name(), "UTF-8") == 0);
    CHECK(cs_from_utf8("Gr\xC3\xBC\xC3\x9F" "e", o, sizeof(o)) == 1);
    CHECK(cs_from_utf8("a\xCC\x88", o, sizeof(o)) == 0 && strcmp(o, "\xC3\xA4") == 0);
    CHECK(cs_to_utf8("a\xCC\x88", o, sizeof(o)) && strcmp(o, "\xC3\xA4") == 0);          /* local NFD -> NFC */
}

/* ===== Phase 4 in the kernel, with an ASCII "local codeset" =====
 * (real Latin-1 bytes are not allowed in APFS names; ASCII makes every
 * non-ASCII character lossy, which is exactly what has to be tested) */
#define EUR "\xE2\x82\xAC"
#define UE  "\xC3\xBC"
#define SZ  "\xC3\x9F"

static void t_cs_lossy_sync(FakeKind k)
{
    Env e; env_init(&e, k);
    cs_test_mode(CS_TEST_ASCII);
    fake_put_data(&e.f, rp(&e, "Preis" EUR ".txt"), "euro", T0);
    fake_put_data(&e.f, rp(&e, "Gr" UE SZ "e/brief.txt"), "brief", T0);
    fake_put_data(&e.f, rp(&e, "plain.txt"), "plain", T0);
    run(&e);
    CHECK_EQ(e.s.downloaded, 3);
    CHECK(lhas(&e, "Preis_.txt", "euro"));
    CHECK(lhas(&e, "Gr__e/brief.txt", "brief"));
    check_idle(&e);
    check_idle(&e);
    CHECK(fake_find(&e.f, rp(&e, "Preis_.txt")) == NULL);
    lw(&e, "Preis_.txt", "euro neu", T0 + 100);       /* edit under the local form */
    run(&e);
    CHECK_EQ(e.s.uploaded, 1);
    CHECK(fake_has(&e.f, rp(&e, "Preis" EUR ".txt"), "euro neu"));
    CHECK(fake_find(&e.f, rp(&e, "Preis_.txt")) == NULL);
    fake_remove(&e.f, rp(&e, "Gr" UE SZ "e/brief.txt"));   /* deleted on the server */
    run(&e);
    CHECK(!lexists(&e, "Gr__e/brief.txt"));
    CHECK(fake_find(&e.f, rp(&e, "Gr__e")) == NULL);
    lw(&e, "Gr__e/neu.txt", "neu lokal", T0 + 200);  /* new file in a lossy directory */
    run(&e);
    CHECK(fake_has(&e.f, rp(&e, "Gr" UE SZ "e/neu.txt"), "neu lokal"));
    check_idle(&e);
    cs_test_mode(CS_TEST_DEFAULT);
    env_free(&e);
}
static void t_cs_lossy_ac(void)  { t_cs_lossy_sync(FAKE_AMICLOUD); }
static void t_cs_lossy_dav(void) { t_cs_lossy_sync(FAKE_WEBDAV); }

/* two server names with the same local form: both skipped, nothing lost */
static void t_cs_clash(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    cs_test_mode(CS_TEST_ASCII);
    fake_put_data(&e.f, rp(&e, "a" EUR ".txt"), "euro", T0);
    fake_put_data(&e.f, rp(&e, "a" UE ".txt"), "ue", T0);
    fake_put_data(&e.f, rp(&e, "b" EUR ".txt"), "b-euro", T0);
    fake_put_data(&e.f, rp(&e, "b_.txt"), "b-plain", T0);
    fake_put_data(&e.f, rp(&e, "ok.txt"), "ok", T0);
    run(&e);
    run(&e);
    CHECK(lhas(&e, "ok.txt", "ok"));
    CHECK(!lexists(&e, "a_.txt"));
    CHECK(!lexists(&e, "b_.txt"));
    CHECK(e.s.conflicts > 0);
    CHECK_EQ(e.f.calls[FOP_PUT] + e.f.calls[FOP_DEL], 0);
    CHECK(fake_has(&e.f, rp(&e, "a" EUR ".txt"), "euro"));
    CHECK(fake_has(&e.f, rp(&e, "a" UE ".txt"), "ue"));
    CHECK(fake_has(&e.f, rp(&e, "b" EUR ".txt"), "b-euro"));
    CHECK(fake_has(&e.f, rp(&e, "b_.txt"), "b-plain"));
    cs_test_mode(CS_TEST_DEFAULT);
    env_free(&e);
}

/* a decomposed (NFD) server name from a Mac: stored composed locally,
 * recognised again, not uploaded a second time */
static void t_cs_nfd_name(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    fake_put_data(&e.f, rp(&e, "Mu\xCC\x88sli.txt"), "nfd", T0);
    run(&e);
    CHECK_EQ(e.s.downloaded, 1);
    check_idle(&e);
    CHECK(fake_find(&e.f, rp(&e, "M\xC3\xBCsli.txt")) == NULL);
    env_free(&e);
}

/* the codeset is part of the identity: switching it starts over */
static void t_cs_identity(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    lw(&e, "a.txt", "a", T0);
    run(&e);
    cs_test_mode(CS_TEST_ASCII);
    run(&e);
    CHECK_EQ(e.s.deleted, 0);
    CHECK(lhas(&e, "a.txt", "a"));
    cs_test_mode(CS_TEST_DEFAULT);
    env_free(&e);
}

/* ===== Virus check (xvs.library on the Amiga; a stub here) ===== */
static int fake_virus(const char *path, long max, char *name, size_t nsz)
{
    char buf[256];
    size_t n;
    FILE *fp = fopen(path, "rb");
    (void)max;
    if (!fp) return -1;
    n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    if (!strstr(buf, "EICAR-VIRUS")) return 0;
    snprintf(name, nsz, "Test-Virus");
    return 1;
}

static void t_virus(void)
{
    Env e; env_init(&e, FAKE_AMICLOUD);
    g_virus = fake_virus;
    fake_put_data(&e.f, rp(&e, "sauber.txt"), "clean", T0);
    fake_put_data(&e.f, rp(&e, "boese.exe"), "xx EICAR-VIRUS xx", T0);
    lw(&e, "lokal-boese.exe", "EICAR-VIRUS", T0);
    lw(&e, "lokal-sauber.txt", "ok", T0);
    run(&e);
    CHECK_EQ(e.s.viruses, 2);
    CHECK(lhas(&e, "sauber.txt", "clean"));
    CHECK(!lexists(&e, "boese.exe"));
    CHECK(!lexists(&e, SYNC_PART_FILE));
    CHECK(fake_find(&e.f, rp(&e, "lokal-boese.exe")) == NULL);
    CHECK(fake_has(&e.f, rp(&e, "lokal-sauber.txt"), "ok"));
    CHECK(lhas(&e, "lokal-boese.exe", "EICAR-VIRUS"));        /* never deleted locally */
    CHECK(fake_has(&e.f, rp(&e, "boese.exe"), "xx EICAR-VIRUS xx"));   /* nor remotely */
    run(&e);                                                  /* stays like that */
    CHECK_EQ(e.s.viruses, 2);
    CHECK_EQ(e.s.deleted, 0);
    g_virus = NULL;
    env_free(&e);
}

/* ------------------------------------------------------------------ */
typedef struct { const char *name; void (*fn)(void); const char *risk; } Test;

static const Test tests[] = {
    { "erster Abgleich laedt hoch (AmiCloud)",       t_first_upload_ac, NULL },
    { "erster Abgleich laedt hoch (WebDAV)",         t_first_upload_dav, NULL },
    { "erster Abgleich laedt herunter (AmiCloud)",   t_first_download_ac, NULL },
    { "erster Abgleich laedt herunter (WebDAV)",     t_first_download_dav, NULL },
    { "ohne Stand, gleicher Inhalt (AmiCloud)",      t_no_baseline_same_content_ac, NULL },
    { "ohne Stand, gleicher Inhalt (WebDAV): Konflikt", t_no_baseline_same_content_dav, NULL },
    { "ohne Stand, verschieden (AmiCloud)",          t_no_baseline_diff_ac, NULL },
    { "ohne Stand, verschieden (WebDAV)",            t_no_baseline_diff_dav, NULL },
    { "lokal geaendert -> hoch (AmiCloud)",          t_local_change_ac, NULL },
    { "lokal geaendert -> hoch (WebDAV)",            t_local_change_dav, NULL },
    { "entfernt geaendert -> runter (AmiCloud)",     t_remote_change_ac, NULL },
    { "entfernt geaendert -> runter (WebDAV)",       t_remote_change_dav, NULL },
    { "beidseitig geaendert: Konflikt (AmiCloud)",   t_both_changed_ac, NULL },
    { "beidseitig geaendert: Konflikt (WebDAV)",     t_both_changed_dav, NULL },
    { "beidseitig gleich geaendert (AmiCloud)",      t_both_changed_same_ac, NULL },
    { "lokal geloescht (AmiCloud)",                  t_local_delete_ac, NULL },
    { "lokal geloescht (WebDAV)",                    t_local_delete_dav, NULL },
    { "entfernt geloescht (AmiCloud)",               t_remote_delete_ac, NULL },
    { "entfernt geloescht (WebDAV)",                 t_remote_delete_dav, NULL },
    { "entfernt geloescht + lokal geaendert: Konflikt", t_remote_del_local_change_ac, NULL },
    { "lokal geloescht + entfernt geaendert: Konflikt", t_local_del_remote_change_ac, NULL },
    { "Datei gegen Verzeichnis: Konflikt",           t_file_vs_dir_ac, NULL },
    { "leere Ordner beidseitig (AmiCloud)",          t_dirs_both_ac, NULL },
    { "leere Ordner beidseitig (WebDAV)",            t_dirs_both_dav, NULL },
    { "lokaler Baum geloescht (AmiCloud)",           t_tree_delete_ac, NULL },
    { "lokaler Baum geloescht (WebDAV)",             t_tree_delete_dav, NULL },
    { "entfernter Baum geloescht (AmiCloud)",        t_rtree_delete_ac, NULL },
    { "entfernter Baum geloescht (WebDAV)",          t_rtree_delete_dav, NULL },
    { "Zeitstempel angeglichen",                     t_mtime_align_ac, NULL },
    { "Probelauf aendert nichts",                    t_dry_run_changes_nothing, NULL },
    { "eigene Dateien ausgeschlossen",               t_own_files_excluded, NULL },
    { "Upload-Fehler: Stand zurueck, neuer Versuch", t_failed_upload_retried, NULL },
    { "Download-Fehler: lokal unberuehrt",           t_failed_download_safe, NULL },
    { "global_revision-Abkuerzung",                  t_delta_shortcut_ac, NULL },
    { "Konflikt schaltet Abkuerzung ab",             t_conflict_disables_delta_ac, NULL },
    { "WebDAV: kein Hin und Her",                    t_dav_no_flipflop, NULL },
    { "fehlende Wurzel wird angelegt (heute)",       t_missing_root_created, NULL },
    { "Stand speichern/laden",                       t_syncstate_roundtrip, NULL },
    { "S12: Stand bleibt sortiert",                  t_syncstate_sorted, NULL },
    { "S12: 2000 Dateien",                           t_many_files, NULL },
    { "S11: Zwischenstand bei langem Lauf",          t_checkpoint, NULL },
    { "S15: zu lange Namen uebersprungen",           t_long_names, NULL },
    { "Pruefung: Umbenennen bei gleicher Revision",  t_rename_same_revision, NULL },
    { "Pruefung: .DS_Store im geloeschten Ordner",   t_dsstore_dir_delete, NULL },
    { "Pruefung: Ausgeschlossenes im Ordner (AmiCloud)", t_excl_ac, NULL },
    { "Pruefung: Ausgeschlossenes im Ordner (WebDAV)",   t_excl_dav, NULL },
    { "Pruefung: S5 verschachtelt (AmiCloud)",       t_s5n_ac, NULL },
    { "Pruefung: S5 verschachtelt (WebDAV)",         t_s5n_dav, NULL },
    { "Pruefung: S5 mit Konfliktdatei",              t_s5_conflict_child, NULL },
    { "Pruefung: Loeschen gegen fremde Aenderung (AmiCloud)", t_delrace_ac, NULL },
    { "Pruefung: Loeschen gegen fremde Aenderung (WebDAV)",   t_delrace_dav, NULL },
    { "Pruefung: WebDAV-Ordner bekommt waehrend des Laufs Neues", t_dir_delete_race_dav, NULL },
    { "Pruefung: lokales Laufwerk fehlt",            t_local_parent_missing, NULL },
    { "Pruefung: 8000 Dateien",                      t_perf_8000, NULL },
    { "Zeichensatz: NFC",                            t_cs_nfc, NULL },
    { "Zeichensatz: Latin-1",                        t_cs_latin1, NULL },
    { "Zeichensatz: UTF-8 (PC)",                     t_cs_utf8_default, NULL },
    { "Zeichensatz: verlustbehaftete Namen (AmiCloud)", t_cs_lossy_ac, NULL },
    { "Zeichensatz: verlustbehaftete Namen (WebDAV)",   t_cs_lossy_dav, NULL },
    { "Zeichensatz: gleiche lokale Form",            t_cs_clash, NULL },
    { "Zeichensatz: NFD-Name vom Mac",               t_cs_nfd_name, NULL },
    { "Zeichensatz: Wechsel = neuer Stand",          t_cs_identity, NULL },
    { "Virenpruefung: nicht hoch-, nicht heruntergeladen", t_virus, NULL },
    { "kaputter Stand (AmiCloud)",                   t_corrupt_baseline_ac, NULL },
    { "AmiCloud: Ordner-Loeschen behaelt neue Datei", t_ac_dir_delete_keeps_new_file, NULL },
    { "S2: Abbruch mitten im Lauf",                  t_break_mid_run, NULL },
    { "S5: neue lokale Datei in entfernt geloeschtem Ordner (AmiCloud)", t_s5_local_ac, NULL },
    { "S5: neue lokale Datei in entfernt geloeschtem Ordner (WebDAV)",   t_s5_local_dav, NULL },
    { "S13: feste Ausschlussliste",                  t_exclusions, NULL },

    { "S1: list-Fehler bricht ab (WebDAV)",   r_list_error_deletes_local,     NULL },
    { "S1: list-Fehler bricht ab (AmiCloud)", r_list_error_deletes_local_ac,  NULL },
    { "S1: unlesbarer lokaler Ordner bricht ab",    r_opendir_error_deletes_remote, NULL },
    { "S5: WebDAV-Ordner-Loeschen laesst neue Datei",   r_dav_dir_delete_takes_new_file, NULL },
    { "S3: anderer Server-Ordner loescht nichts",       r_identity_change_deletes_local, NULL },
    { "S3: fremdes Stand-Format wird verworfen",        t_foreign_format, NULL },
    { "S4: verschwundene Server-Wurzel bricht ab",      t_remote_root_gone, NULL },
    { "Probelauf eines neuen Paars ohne Server-Ordner", t_dry_run_new_pair, NULL },
    { "S2: Abbruch vor dem Lauf",                          r_break_ignored,                NULL },
    { "S6: Gross/Klein-Kollision (Dateien)",            r_case_collision,               NULL },
    { "S6: Gross/Klein-Kollision (Ordner)",             t_case_collision_dirs,          NULL },
    { "S9: fremde Aenderung waehrend des Laufs",        r_global_rev_at_end,            NULL },
    { "S14: fehlgeschlagener Download wiederholt",       r_failed_download_never_retried, NULL },
    { "S7: Download laesst frische Aenderung",          r_download_overwrites_fresh_edit, NULL },
    { "S7: Loeschen laesst frische Aenderung",          t_delete_keeps_fresh_edit,       NULL },
    { "S8: Upload laesst fremde Aenderung",             r_upload_overwrites_remote_change, NULL },
    { "S8: neue Datei laesst fremde Neuanlage",         t_create_only, NULL },
    { "S8: WebDAV If-Match",                            t_dav_if_match, NULL },
    { "S10: Stand traegt Server-Revision nach Upload",  t_put_result, NULL },
};

int main(int argc, char **argv)
{
    UiHooks hooks;
    const char *tmp = getenv("TMPDIR");
    int i, n = (int)(sizeof(tests) / sizeof(tests[0]));
    int pass = 0, fail = 0, xfail = 0, xpass = 0, skip = 0;

    g_verbose = getenv("VERBOSE") && getenv("VERBOSE")[0] == '1';
    memset(&hooks, 0, sizeof(hooks));
    hooks.notify = quiet_notify;
    hooks.progress = quiet_progress;
    ui_set_hooks(&hooks);

    snprintf(g_tmproot, sizeof(g_tmproot), "%s/acsynctest-XXXXXX", tmp && tmp[0] ? tmp : "/tmp");
    if (!mkdtemp(g_tmproot)) { perror("mkdtemp"); return 2; }

    for (i = 0; i < n; i++) {
        if (argc > 1 && !strstr(tests[i].name, argv[1])) continue;
        g_failed = 0; g_skipped = 0;
        g_quiet_checks = tests[i].risk != NULL;
        printf("%-6s %s%s%s\n", tests[i].risk ? "RISK" : "ok?", tests[i].name,
               tests[i].risk ? "  " : "", tests[i].risk ? tests[i].risk : "");
        tests[i].fn();
        if (g_skipped) { skip++; printf("  -> SKIP\n"); }
        else if (!tests[i].risk) {
            if (g_failed) { fail++; printf("  -> FEHLER\n"); }
            else { pass++; printf("  -> ok\n"); }
        } else {
            if (g_failed) { xfail++; printf("  -> XFAIL (Risiko besteht)\n"); }
            else { xpass++; printf("  -> XPASS: behoben? RISK-Markierung entfernen\n"); }
        }
    }
    rm_rf(g_tmproot);
    printf("\n%d ok, %d Fehler, %d XFAIL (bekannte Risiken), %d XPASS, %d uebersprungen\n",
           pass, fail, xfail, xpass, skip);
    return (fail || xpass) ? 1 : 0;
}
