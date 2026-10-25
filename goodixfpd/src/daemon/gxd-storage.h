/* gxd-storage.h - impronte su disco: <state>/goodixfpd/<utente>/<dito>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include "gxd-template.h"

gboolean     gxd_storage_valid_username (const char *username);

/* NULL + G_IO_ERROR_NOT_FOUND se il dito non e' registrato. */
GxdTemplate *gxd_storage_load (const char *username, int finger, GError **error);
gboolean     gxd_storage_save (const GxdTemplate *tpl, GError **error);
/* TRUE anche se il file non esisteva. */
gboolean     gxd_storage_delete (const char *username, int finger, GError **error);

/* Dita registrate (ordinate), impronte di un utente, impronte di tutti. */
GArray      *gxd_storage_fingers (const char *username);
GPtrArray   *gxd_storage_load_user (const char *username);
GPtrArray   *gxd_storage_load_all (void);
