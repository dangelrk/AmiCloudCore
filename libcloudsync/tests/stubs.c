/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 *
 * stubs.c -- link-time stand-ins for the libcloudhttp functions that
 * sync.c's AmiCloud adapter (cloudsync_run) references. The unit tests
 * only ever call cloudsync_run_t() with a FakeTransport, so none of
 * these is reached; they exist so the tests link without libcurl.
 */
#include "cmd.h"
#include <stdlib.h>

int cmd_list_raw(const AcConfig *cfg, const char *remote_path, AcEntry **out, int *out_n)
{ (void)cfg; (void)remote_path; *out = NULL; *out_n = 0; abort(); }
void cmd_entries_free(AcEntry *entries) { (void)entries; abort(); }
int cmd_get_named(const AcConfig *cfg, const char *remote_path, const char *local_path,
                  const char *display_name)
{ (void)cfg; (void)remote_path; (void)local_path; (void)display_name; abort(); }
int cmd_put(const AcConfig *cfg, const char *local_path, const char *remote_path)
{ (void)cfg; (void)local_path; (void)remote_path; abort(); }
int cmd_setdate_mtime(const AcConfig *cfg, const char *remote_path, long mtime_epoch)
{ (void)cfg; (void)remote_path; (void)mtime_epoch; abort(); }
int cmd_del(const AcConfig *cfg, const char *remote_path)
{ (void)cfg; (void)remote_path; abort(); }
int cmd_mkdir_quiet(const AcConfig *cfg, const char *remote_path)
{ (void)cfg; (void)remote_path; abort(); }
int cmd_status(const AcConfig *cfg, char *version_out, size_t version_sz, long *global_revision_out)
{ (void)cfg; (void)version_out; (void)version_sz; (void)global_revision_out; abort(); }
