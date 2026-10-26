/* gxd-manager.c - net.reactivated.Fprint.Manager.
 *
 * As in fprintd: the Manager lives at /net/reactivated/Fprint/Manager, the
 * devices are exported by an ObjectManager on /net/reactivated/Fprint.
 * Here there is a single device, /net/reactivated/Fprint/Device/0.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "gxd-manager.h"

struct _GxdManager
{
  FprintDBusManager        *skeleton;
  GDBusObjectManagerServer *objects;
};

static gboolean
handle_get_devices (FprintDBusManager *skeleton, GDBusMethodInvocation *invocation,
                    gpointer data)
{
  const char *const devices[] = { GXD_DEVICE_PATH, NULL };

  fprint_dbus_manager_complete_get_devices (skeleton, invocation, devices);
  return TRUE;
}

static gboolean
handle_get_default_device (FprintDBusManager *skeleton, GDBusMethodInvocation *invocation,
                           gpointer data)
{
  fprint_dbus_manager_complete_get_default_device (skeleton, invocation, GXD_DEVICE_PATH);
  return TRUE;
}

GxdManager *
gxd_manager_new (GDBusConnection *connection, GxdDevice *device)
{
  GxdManager *self = g_new0 (GxdManager, 1);
  g_autoptr(FprintDBusObjectSkeleton) object = NULL;
  g_autoptr(GError) error = NULL;

  self->objects = g_dbus_object_manager_server_new (GXD_SERVICE_PATH);
  object = fprint_dbus_object_skeleton_new (GXD_DEVICE_PATH);
  fprint_dbus_object_skeleton_set_device (object, FPRINT_DBUS_DEVICE (device));
  g_dbus_object_manager_server_export (self->objects, G_DBUS_OBJECT_SKELETON (object));
  g_dbus_object_manager_server_set_connection (self->objects, connection);

  self->skeleton = fprint_dbus_manager_skeleton_new ();
  g_signal_connect (self->skeleton, "handle-get-devices", G_CALLBACK (handle_get_devices), NULL);
  g_signal_connect (self->skeleton, "handle-get-default-device",
                    G_CALLBACK (handle_get_default_device), NULL);
  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (self->skeleton), connection,
                                         GXD_SERVICE_PATH "/Manager", &error))
    g_warning ("Could not export the manager: %s", error->message);
  return self;
}

void
gxd_manager_free (GxdManager *self)
{
  if (!self)
    return;
  g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (self->skeleton));
  g_object_unref (self->skeleton);
  g_object_unref (self->objects);
  g_free (self);
}
