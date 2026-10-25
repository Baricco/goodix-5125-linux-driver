/* gxd-manager.h - /net/reactivated/Fprint/Manager e l'ObjectManager.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include "gxd-device.h"

typedef struct _GxdManager GxdManager;

GxdManager *gxd_manager_new (GDBusConnection *connection, GxdDevice *device);
void        gxd_manager_free (GxdManager *manager);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GxdManager, gxd_manager_free)
