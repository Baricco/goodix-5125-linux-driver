/* gxd-storage.h - fingerprints on disk: <state>/goodixfpd/<user>/<finger>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include "gxd-template.h"

gboolean     gxd_storage_valid_username (const char *username);

/* NULL + G_IO_ERROR_NOT_FOUND if the finger is not enrolled. */
GxdTemplate *gxd_storage_load (const char *username, int finger, GError **error);
gboolean     gxd_storage_save (const GxdTemplate *tpl, GError **error);
/* TRUE even if the file did not exist. */
gboolean     gxd_storage_delete (const char *username, int finger, GError **error);

/* Enrolled fingers (sorted), one user's fingerprints, everybody's fingerprints. */
GArray      *gxd_storage_fingers (const char *username);
GPtrArray   *gxd_storage_load_user (const char *username);
GPtrArray   *gxd_storage_load_all (void);
