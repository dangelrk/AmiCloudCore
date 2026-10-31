/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef CLOUDSYNC_AMICLOUD_TRANSPORT_H
#define CLOUDSYNC_AMICLOUD_TRANSPORT_H

/*
 * amicloud_transport.h -- the AmiCloud server (api.php) behind the sync
 * kernel's SyncTransport interface (transport.h). Counterpart of
 * webdav.h; used by AmiCloud's amicloud-sync.
 */

#include "transport.h"

/* malloc'd transport bound to cfg (kept by reference, must outlive it);
 * NULL on out of memory. Free with amicloud_transport_close(). */
SyncTransport *amicloud_transport_open(const AcConfig *cfg);
void amicloud_transport_close(SyncTransport *t);

#endif /* CLOUDSYNC_AMICLOUD_TRANSPORT_H */
