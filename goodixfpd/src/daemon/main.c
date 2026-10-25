/* main.c - goodixfpd.
 *
 * Avviato al posto di fprintd (drop-in di fprintd.service). Se il lettore
 * Goodix 27c6:5125/5135 non c'e' cede il posto al vero fprintd con gli
 * stessi argomenti, cosi' su altre macchine non cambia nulla.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <errno.h>
#include <locale.h>
#include <unistd.h>
#include <glib-unix.h>

#include "gxd.h"
#include "gxd-device.h"
#include "gxd-manager.h"
#include "gxd-sensor.h"

#ifndef FPRINTD_PATH
#define FPRINTD_PATH "/usr/lib/fprintd"
#endif

static gboolean no_timeout;
static gboolean fatal_warnings;
static char *state_dir;
#ifdef GXD_TEST_BACKEND
char *gxd_test_socket;
#endif

static const GOptionEntry entries[] = {
  { "no-timeout", 't', 0, G_OPTION_ARG_NONE, &no_timeout,
    "Accepted for compatibility with fprintd (the service never exits on its own)", NULL },
  { "g-fatal-warnings", 0, 0, G_OPTION_ARG_NONE, &fatal_warnings, "Make all warnings fatal", NULL },
  { "state-dir", 0, 0, G_OPTION_ARG_FILENAME, &state_dir, "Data directory (default: $STATE_DIRECTORY or /var/lib/fprint)", "DIR" },
#ifdef GXD_TEST_BACKEND
  { "test-socket", 0, 0, G_OPTION_ARG_FILENAME, &gxd_test_socket, "Simulated sensor control socket", "PATH" },
#endif
  { NULL }
};

static gboolean
on_signal (gpointer data)
{
  g_main_loop_quit (data);
  return G_SOURCE_REMOVE;
}

static void
on_name_lost (GDBusConnection *connection, const char *name, gpointer data)
{
  g_warning ("Failed to get name: %s", name);
  g_main_loop_quit (data);
}

int
main (int argc, char **argv)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GDBusConnection) connection = NULL;
  g_autoptr(GMainLoop) loop = NULL;
  g_autoptr(GxdSensor) sensor = NULL;
  g_autoptr(GxdDevice) device = NULL;
  g_autoptr(GxdManager) manager = NULL;
  g_auto(GStrv) orig_argv = g_strdupv (argv);
  guint owner;

  setlocale (LC_ALL, "");

#ifndef GXD_TEST_BACKEND
  if (!gxd_sensor_present ())
    {
      g_message ("No Goodix 27c6:5125/5135 reader, starting fprintd instead");
      execv (FPRINTD_PATH, orig_argv);
      g_critical ("Could not start %s: %s", FPRINTD_PATH, g_strerror (errno));
      return 1;
    }
#endif

  context = g_option_context_new ("- fingerprint service for Goodix 27c6:5125/5135, fprintd compatible");
  g_option_context_add_main_entries (context, entries, NULL);
  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_printerr ("%s\n", error->message);
      return 1;
    }
  if (fatal_warnings)
    g_log_set_always_fatal (g_log_set_always_fatal (G_LOG_FATAL_MASK) |
                            G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL);
  if (state_dir)
    gxd_set_state_dir (state_dir);

  connection = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);
  if (!connection)
    {
      g_warning ("Failed to open connection to bus: %s", error->message);
      return 1;
    }

  loop = g_main_loop_new (NULL, FALSE);
  g_unix_signal_add (SIGTERM, on_signal, loop);
  g_unix_signal_add (SIGINT, on_signal, loop);

  sensor = gxd_sensor_new ();
  device = gxd_device_new (sensor, &error);
  if (!device)
    {
      g_warning ("Cannot set up the device: %s", error->message);
      return 1;
    }
  manager = gxd_manager_new (connection, device);

  /* Il nome si prende a oggetti esportati: chi chiede subito i dispositivi
   * li trova. */
  owner = g_bus_own_name_on_connection (connection, GXD_SERVICE_NAME,
                                        G_BUS_NAME_OWNER_FLAGS_NONE,
                                        NULL, on_name_lost, loop, NULL);
  g_message ("goodixfpd %s ready (prints in %s/goodixfpd)", PACKAGE_VERSION, gxd_state_dir ());
  g_main_loop_run (loop);
  g_bus_unown_name (owner);
  gxd_sensor_shutdown (sensor);
  return 0;
}
