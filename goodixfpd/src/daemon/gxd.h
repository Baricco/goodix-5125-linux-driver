/* gxd.h - goodixfpd: a service compatible with fprintd's D-Bus API for the
 * Goodix 27c6:5125/5135 reader.
 *
 * pam_fprintd, the fprintd-* commands, GNOME/KDE and the project's GUI talk to
 * net.reactivated.Fprint: this service implements the same contract (methods,
 * signals, errors, polkit checks) on top of the sensor core, without
 * modifying libfprint or fprintd.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <gio/gio.h>

#include "fprintd-dbus.h"

#define GXD_SERVICE_NAME  "net.reactivated.Fprint"
#define GXD_SERVICE_PATH  "/net/reactivated/Fprint"
#define GXD_DEVICE_PATH   GXD_SERVICE_PATH "/Device/0"
/* Name the device had with libfprint: pam_fprintd uses it in its messages
 * ("Place your finger on ..."). */
#define GXD_DEVICE_NAME   "Goodix fingerprint (27c6:5125/5135)"

/* D-Bus errors: same names and meanings as fprintd. */
#define GXD_ERROR (gxd_error_quark ())
GQuark gxd_error_quark (void);

typedef enum {
  GXD_ERROR_CLAIM_DEVICE,
  GXD_ERROR_ALREADY_IN_USE,
  GXD_ERROR_INTERNAL,
  GXD_ERROR_PERMISSION_DENIED,
  GXD_ERROR_NO_ENROLLED_PRINTS,
  GXD_ERROR_FINGER_ALREADY_ENROLLED,
  GXD_ERROR_NO_ACTION_IN_PROGRESS,
  GXD_ERROR_INVALID_FINGERNAME,
  GXD_ERROR_NO_SUCH_DEVICE,
  GXD_ERROR_PRINTS_NOT_DELETED,
  GXD_ERROR_PRINTS_NOT_DELETED_FROM_DEVICE,
} GxdError;

/* Fingers: same numbering as FpFinger in libfprint. */
#define GXD_FINGER_UNKNOWN  0
#define GXD_FINGER_FIRST    1
#define GXD_FINGER_LAST     10

const char *gxd_finger_to_name (int finger);      /* 0 -> "any" */
int         gxd_finger_from_name (const char *name); /* unknown -> 0 */

/* Data directory (StateDirectory=fprint of fprintd.service). */
const char *gxd_state_dir (void);
void        gxd_set_state_dir (const char *dir);
