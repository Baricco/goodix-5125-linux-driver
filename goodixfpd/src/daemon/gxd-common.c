/* gxd-common.c - errori, nomi delle dita, directory dei dati.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "gxd.h"

static const GDBusErrorEntry error_entries[] = {
  { GXD_ERROR_CLAIM_DEVICE, "net.reactivated.Fprint.Error.ClaimDevice" },
  { GXD_ERROR_ALREADY_IN_USE, "net.reactivated.Fprint.Error.AlreadyInUse" },
  { GXD_ERROR_INTERNAL, "net.reactivated.Fprint.Error.Internal" },
  { GXD_ERROR_PERMISSION_DENIED, "net.reactivated.Fprint.Error.PermissionDenied" },
  { GXD_ERROR_NO_ENROLLED_PRINTS, "net.reactivated.Fprint.Error.NoEnrolledPrints" },
  { GXD_ERROR_FINGER_ALREADY_ENROLLED, "net.reactivated.Fprint.Error.FingerAlreadyEnrolled" },
  { GXD_ERROR_NO_ACTION_IN_PROGRESS, "net.reactivated.Fprint.Error.NoActionInProgress" },
  { GXD_ERROR_INVALID_FINGERNAME, "net.reactivated.Fprint.Error.InvalidFingername" },
  { GXD_ERROR_NO_SUCH_DEVICE, "net.reactivated.Fprint.Error.NoSuchDevice" },
  { GXD_ERROR_PRINTS_NOT_DELETED, "net.reactivated.Fprint.Error.PrintsNotDeleted" },
  { GXD_ERROR_PRINTS_NOT_DELETED_FROM_DEVICE, "net.reactivated.Fprint.Error.PrintsNotDeletedFromDevice" },
};

GQuark
gxd_error_quark (void)
{
  static gsize quark = 0;

  g_dbus_error_register_error_domain ("gxd-error-quark", &quark,
                                      error_entries,
                                      G_N_ELEMENTS (error_entries));
  return (GQuark) quark;
}

static const char *const finger_names[] = {
  [GXD_FINGER_UNKNOWN] = "any",
  "left-thumb",
  "left-index-finger",
  "left-middle-finger",
  "left-ring-finger",
  "left-little-finger",
  "right-thumb",
  "right-index-finger",
  "right-middle-finger",
  "right-ring-finger",
  "right-little-finger",
};

const char *
gxd_finger_to_name (int finger)
{
  if (finger < GXD_FINGER_UNKNOWN || finger > GXD_FINGER_LAST)
    return NULL;
  return finger_names[finger];
}

int
gxd_finger_from_name (const char *name)
{
  if (name == NULL || *name == '\0')
    return GXD_FINGER_UNKNOWN;
  for (int i = GXD_FINGER_FIRST; i <= GXD_FINGER_LAST; i++)
    if (g_str_equal (name, finger_names[i]))
      return i;
  return GXD_FINGER_UNKNOWN;
}

static char *state_dir;

const char *
gxd_state_dir (void)
{
  if (state_dir == NULL)
    {
      /* systemd lo esporta per StateDirectory=fprint di fprintd.service */
      const char *env = g_getenv ("STATE_DIRECTORY");

      state_dir = g_strdup (env && *env && !strchr (env, ':') ? env : "/var/lib/fprint");
    }
  return state_dir;
}

void
gxd_set_state_dir (const char *dir)
{
  g_free (state_dir);
  state_dir = g_strdup (dir);
}
