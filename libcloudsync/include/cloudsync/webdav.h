/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef CLOUDSYNC_WEBDAV_H
#define CLOUDSYNC_WEBDAV_H

/*
 * webdav.h — standard WebDAV transport (RFC 4918 PROPFIND/GET/PUT/MKCOL/
 * DELETE) for cloudsync_run_t()'s SyncTransport interface (transport.h).
 *
 * This is the ONLY backend this tool ships with: it talks to a plain
 * OwnCloud/Nextcloud server and nothing else -- there is no companion
 * AmiCloud/api.php backend here, and none is planned as part of this
 * work (see repo memory: AmiCloud and OwnCloud must not be mixed; the
 * AmiCloud server gets its own, separate sync commodity later).
 *
 * cfg->server accepts two shapes, both handled by
 * webdav_derive_base_url() below:
 *   - Bare host, e.g. "https://cloud.example.com" (the "nur
 *     Hauptserver" AmiCloudSyncPrefs editor, see repo plan) -- the
 *     per-user WebDAV collection root is derived as
 *     "<host>/remote.php/dav/files/<cfg->username>".
 *   - A full WebDAV collection URL, e.g.
 *     "https://cloud.example.com/remote.php/dav/files/USER" (Nextcloud/
 *     ownCloud's per-user files root, recognised by containing
 *     "/remote.php/") -- used verbatim, for backward compatibility with
 *     existing AmiCloudSync.prefs files and webdavsync's own config.
 * Neither shape is ever an api.php endpoint.
 * cfg->username/password are sent as HTTP Basic auth (Nextcloud: use an
 * app password, not the account login password). cfg->basepath is
 * unused by this backend (WebDAV has no separate base-path concept
 * distinct from the collection URL itself).
 */

#include "amicloud.h"
#include "transport.h"

#include <stddef.h>

/* Builds a WebDAV SyncTransport bound to cfg. Never fails outright (no
 * network round-trip happens here) -- returns NULL only on allocation
 * failure. Free with webdav_transport_close(). */
SyncTransport *webdav_transport_open(const AcConfig *cfg);

/* Convenience: transport->close(transport), guarding against NULL. */
void webdav_transport_close(SyncTransport *t);

/* Derives the effective WebDAV collection base URL from cfg->server (+
 * cfg->username if it's a bare host) -- see this header's own comment
 * above for the two accepted shapes. webdav_transport_open() uses this
 * internally; exposed here so independent test/verification code that
 * deliberately bypasses the SyncTransport (webdavsynctest's
 * build_test_url(), acting as a third party outside the sync engine)
 * can still compute the exact same collection URL a real transport
 * would, without duplicating the derivation POLICY itself. */
void webdav_derive_base_url(char *out, size_t outsz, const AcConfig *cfg);

#endif /* CLOUDSYNC_WEBDAV_H */
