/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef CONFIG_H
#define CONFIG_H

#include "amicloud.h"

/*
 * Load config from file.  Returns 1 on success, 0 if file not found
 * (caller should fall back to command-line args).
 * Keys recognised: server, username/user, password/pass, basepath,
 *                  verify_ssl (0|1), debug (log target path),
 *                  gmt_offset, dst, timeout (network idle timeout, s),
 *                  cache_ttl (handler directory cache lifetime, s)
 */
int config_load(const char *filepath, AcConfig *cfg);

/* Merge command-line values into cfg (non-empty strings overwrite) */
void config_merge(AcConfig *cfg,
                  const char *server,
                  const char *username,
                  const char *password,
                  const char *basepath);

/*
 * config_save() -- the write-side counterpart of config_load(), for the
 * Prefs editors (amicloud-prefs/amivcs-prefs/amiowncloud-sync-prefs, see
 * repo plan). Always writes server=/user=/pass=/verify_ssl= (every tool
 * needs those); CONFIG_SAVE_* flags add the optional keys a given tool's
 * config file actually uses, so e.g. amivcs.cfg doesn't grow a stray
 * basepath= line no amivcs code ever reads.
 *
 * Atomic like pairs_save() (AmiOwnCloud's apps/amiowncloud-sync-cx/src/pairs.c):
 * writes "<filepath>.tmp" then rename()s it over filepath, so a crash or
 * a full ENVARC: mid-write never leaves a half-written config behind.
 * Returns 1 on success, 0 on any I/O failure (tmp file couldn't be
 * opened/closed/renamed).
 */
#define CONFIG_SAVE_BASEPATH   (1u << 0)
#define CONFIG_SAVE_DEBUG      (1u << 1)
#define CONFIG_SAVE_TZ         (1u << 2) /* gmt_offset= and dst= together */
#define CONFIG_SAVE_ALL        (CONFIG_SAVE_BASEPATH | CONFIG_SAVE_DEBUG | CONFIG_SAVE_TZ)

int config_save(const char *filepath, const AcConfig *cfg, unsigned flags);

/* Print a usage summary and exit */
void config_usage(const char *progname);

/*
 * Fills primary/fallback with this platform's conventional config-file
 * search path for a program config named `basename` (e.g. "amicloud",
 * "amiowncloud-sync") -- the exact ENV:/PROGDIR:/~/.<basename>rc cascade every
 * CLI and GUI app in this repo already needs, previously copied ad hoc
 * into each one's own main() as DEFAULT_CFG/FALLBACK_CFG macros:
 *   Amiga : primary "ENV:<basename>.cfg", fallback "<basename>.cfg"
 *   Windows: primary "<basename>.cfg", fallback "" (none)
 *   else  : primary "$HOME/.<basename>rc" (or "./.<basename>rc" if
 *           $HOME is unset), fallback "" (none)
 * Caller: config_load(primary, cfg); if that fails and fallback[0],
 * config_load(fallback, cfg) too. Nothing here touches the filesystem --
 * it only builds path strings, exactly like the macros it replaces did. */
void config_default_path(const char *basename,
                         char *primary,  size_t primary_sz,
                         char *fallback, size_t fallback_sz);

#endif /* CONFIG_H */
