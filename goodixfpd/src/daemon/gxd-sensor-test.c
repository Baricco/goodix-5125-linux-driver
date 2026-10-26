/* gxd-sensor-test.c - simulated sensor for the integration tests.
 *
 * Only in the goodixfpd-test binary (never installed). Same API as
 * gxd-sensor.c; instead of USB there is a UNIX socket (--test-socket) from
 * which, during a scan, commands are read line by line:
 *   image <file.pgm>   a touch with this 8-bit 80x64 image (P5)
 *   retry              a touch to be repeated
 *   finger 0|1         finger lifted/placed
 *   lost               sensor lost
 * Every executed command receives "ok\n": that way the test knows the service
 * was really capturing. With GXD_TEST_OPEN_FAIL=1 opening fails.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <glib-unix.h>
#include <glib/gstdio.h>

#include "gxd-sensor.h"
#include "gxd-template.h"

extern char *gxd_test_socket;

enum { SIGNAL_FINGER, SIGNAL_SCAN, SIGNAL_LOST, N_SIGNALS };
static guint signals[N_SIGNALS];

typedef struct Worker Worker;

struct _GxdSensor
{
  GObject       parent;
  GMainContext *ctx;
  gboolean      open;
  Worker       *worker;
  guint         stopping;
  guint         gen;
  gboolean      scanning;
  int           listen_fd;
  int           conn_fd;
  int           wake[2];      /* wakes the thread blocked in poll() */
};

struct Worker
{
  GxdSensor *self;
  guint      gen;
  GThread   *thread;
  gint       stop;
};

G_DEFINE_FINAL_TYPE (GxdSensor, gxd_sensor, G_TYPE_OBJECT)

void
gxd_scan_free (GxdScan *scan)
{
  if (!scan)
    return;
  gx_featset_clear (&scan->fs);
  g_free (scan);
}

gboolean
gxd_sensor_present (void)
{
  return TRUE;
}

/* ---- events ---- */

typedef struct {
  GxdSensor *self;
  guint      gen;
  int        type;
  gboolean   present;
  GxdScan   *scan;
} Event;

static void
event_free (gpointer data)
{
  Event *ev = data;

  g_object_unref (ev->self);
  gxd_scan_free (ev->scan);
  g_free (ev);
}

static gboolean
event_dispatch (gpointer data)
{
  Event *ev = data;

  if (ev->gen != ev->self->gen || !ev->self->scanning)
    return G_SOURCE_REMOVE;
  if (ev->type == SIGNAL_FINGER)
    g_signal_emit (ev->self, signals[SIGNAL_FINGER], 0, ev->present);
  else if (ev->type == SIGNAL_SCAN)
    g_signal_emit (ev->self, signals[SIGNAL_SCAN], 0, ev->scan);
  else
    {
      g_autoptr(GError) error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE,
                                                     "simulated sensor lost");
      g_signal_emit (ev->self, signals[SIGNAL_LOST], 0, error);
    }
  return G_SOURCE_REMOVE;
}

static void
post (Worker *w, int type, gboolean present, GxdScan *scan)
{
  Event *ev = g_new0 (Event, 1);

  ev->self = g_object_ref (w->self);
  ev->gen = w->gen;
  ev->type = type;
  ev->present = present;
  ev->scan = scan;
  g_main_context_invoke_full (w->self->ctx, G_PRIORITY_DEFAULT, event_dispatch, ev, event_free);
}

/* ---- commands ---- */

static GxdScan *
scan_from_pgm (const char *path)
{
  g_autofree char *buf = NULL;
  gsize len;
  int w, h, mv, off = 0;
  GxdScan *scan = g_new0 (GxdScan, 1);

  scan->kind = GXD_SCAN_RETRY;
  if (!g_file_get_contents (path, &buf, &len, NULL) ||
      sscanf (buf, "P5 %d %d %d%n", &w, &h, &mv, &off) != 3 ||
      w != GXD_IMG_W || h != GXD_IMG_H || mv > 255 ||
      len < (gsize) off + 1 + GXD_IMG_W * GXD_IMG_H)
    {
      g_warning ("test sensor: bad image %s", path);
      return scan;
    }
  memcpy (scan->img8, buf + off + 1, sizeof (scan->img8));
  if (gx_sift_extract (scan->img8, GXD_IMG_W, GXD_IMG_H, &scan->fs) == 0 &&
      scan->fs.n >= GXD_MIN_KEYPOINTS)
    scan->kind = GXD_SCAN_FRAME;
  return scan;
}

/* One line from the socket; FALSE if the scan must stop. */
static gboolean
read_line (Worker *w, char *line, gsize cap)
{
  GxdSensor *self = w->self;
  gsize n = 0;

  while (!g_atomic_int_get (&w->stop))
    {
      struct pollfd fds[2] = {
        { self->conn_fd >= 0 ? self->conn_fd : self->listen_fd, POLLIN, 0 },
        { self->wake[0], POLLIN, 0 },
      };

      if (poll (fds, 2, -1) < 0 && errno != EINTR)
        return FALSE;
      if (fds[1].revents)
        return FALSE;
      if (!(fds[0].revents & (POLLIN | POLLHUP)))
        continue;
      if (self->conn_fd < 0)
        {
          self->conn_fd = accept (self->listen_fd, NULL, NULL);
          continue;
        }
      /* one byte at a time: the rest stays in the socket for the next
       * scan */
      if (read (self->conn_fd, line + n, 1) != 1)
        {
          close (self->conn_fd);
          self->conn_fd = -1;
          n = 0;
          continue;
        }
      if (line[n] == '\n' || n + 2 >= cap)
        {
          line[n] = '\0';
          return TRUE;
        }
      n++;
    }
  return FALSE;
}

static gpointer
scan_thread (gpointer data)
{
  Worker *w = data;
  char line[4096];

  while (read_line (w, line, sizeof (line)))
    {
      gboolean lost = FALSE;

      if (g_str_has_prefix (line, "image "))
        {
          post (w, SIGNAL_FINGER, TRUE, NULL);
          post (w, SIGNAL_SCAN, FALSE, scan_from_pgm (line + 6));
          post (w, SIGNAL_FINGER, FALSE, NULL);
        }
      else if (g_str_equal (line, "retry"))
        {
          GxdScan *scan = g_new0 (GxdScan, 1);
          scan->kind = GXD_SCAN_RETRY;
          post (w, SIGNAL_SCAN, FALSE, scan);
        }
      else if (g_str_has_prefix (line, "finger "))
        post (w, SIGNAL_FINGER, line[7] == '1', NULL);
      else if (g_str_equal (line, "lost"))
        {
          post (w, SIGNAL_LOST, FALSE, NULL);
          lost = TRUE;
        }
      else
        g_warning ("test sensor: unknown command '%s'", line);
      if (write (w->self->conn_fd, "ok\n", 3) != 3)
        g_warning ("test sensor: cannot acknowledge");
      if (lost)
        break;
    }
  return NULL;
}

/* ---- API ---- */

static void
open_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  g_usleep (50 * 1000);
  if (g_cancellable_is_cancelled (cancellable))
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "open cancelled");
  else if (g_strcmp0 (g_getenv ("GXD_TEST_OPEN_FAIL"), "1") == 0)
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED, "simulated open failure");
  else
    g_task_return_boolean (task, TRUE);
}

static void
open_done (GObject *source, GAsyncResult *res, gpointer data)
{
  GxdSensor *self = GXD_SENSOR (source);
  GTask *outer = data;
  GError *error = NULL;

  if (g_task_propagate_boolean (G_TASK (res), &error))
    {
      self->open = TRUE;
      g_task_return_boolean (outer, TRUE);
    }
  else
    g_task_return_error (outer, error);
  g_object_unref (outer);
}

void
gxd_sensor_open (GxdSensor *self, GCancellable *cancellable,
                 GAsyncReadyCallback callback, gpointer user_data)
{
  GTask *outer = g_task_new (self, cancellable, callback, user_data);
  g_autoptr(GTask) inner = g_task_new (self, cancellable, open_done, outer);

  g_task_run_in_thread (inner, open_thread);
}

gboolean
gxd_sensor_open_finish (GxdSensor *self, GAsyncResult *res, GError **error)
{
  return g_task_propagate_boolean (G_TASK (res), error);
}

gboolean
gxd_sensor_is_open (GxdSensor *self)
{
  return self->open;
}

void
gxd_sensor_scan_start (GxdSensor *self)
{
  Worker *w;

  g_return_if_fail (self->open && self->worker == NULL && self->stopping == 0);
  w = g_new0 (Worker, 1);
  w->self = self;
  w->gen = ++self->gen;
  self->scanning = TRUE;
  w->thread = g_thread_new ("gxd-test-scan", scan_thread, w);
  self->worker = w;
}

gboolean
gxd_sensor_is_scanning (GxdSensor *self)
{
  return self->scanning;
}

static void
join_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  Worker *w = data;
  char c;

  g_thread_join (w->thread);
  /* drain the wake-up pipe */
  while (read (w->self->wake[0], &c, 1) == 1)
    ;
  g_free (w);
  g_task_return_boolean (task, TRUE);
}

static void
scan_joined (GObject *source, GAsyncResult *res, gpointer data)
{
  GTask *outer = data;

  GXD_SENSOR (source)->stopping--;
  g_task_return_boolean (outer, TRUE);
  g_object_unref (outer);
}

void
gxd_sensor_scan_stop (GxdSensor *self, GAsyncReadyCallback callback, gpointer user_data)
{
  GTask *outer = g_task_new (self, NULL, callback, user_data);
  g_autoptr(GTask) inner = NULL;
  Worker *w = g_steal_pointer (&self->worker);

  self->scanning = FALSE;
  self->gen++;
  if (!w)
    {
      g_task_return_boolean (outer, TRUE);
      g_object_unref (outer);
      return;
    }
  g_atomic_int_set (&w->stop, TRUE);
  if (write (self->wake[1], "x", 1) != 1)
    g_warning ("test sensor: cannot wake the scan thread");
  self->stopping++;
  inner = g_task_new (self, NULL, scan_joined, outer);
  g_task_set_task_data (inner, w, NULL);
  g_task_run_in_thread (inner, join_thread);
}

void
gxd_sensor_scan_stop_finish (GxdSensor *self, GAsyncResult *res)
{
  g_task_propagate_boolean (G_TASK (res), NULL);
}

static void
close_after_stop (GObject *source, GAsyncResult *res, gpointer data)
{
  GTask *outer = data;

  GXD_SENSOR (source)->open = FALSE;
  g_task_return_boolean (outer, TRUE);
  g_object_unref (outer);
}

void
gxd_sensor_close (GxdSensor *self, GAsyncReadyCallback callback, gpointer user_data)
{
  gxd_sensor_scan_stop (self, close_after_stop, g_task_new (self, NULL, callback, user_data));
}

void
gxd_sensor_close_finish (GxdSensor *self, GAsyncResult *res)
{
  g_task_propagate_boolean (G_TASK (res), NULL);
}

void
gxd_sensor_shutdown (GxdSensor *self)
{
  Worker *w = g_steal_pointer (&self->worker);

  self->scanning = FALSE;
  if (w)
    {
      g_atomic_int_set (&w->stop, TRUE);
      if (write (self->wake[1], "x", 1) != 1)
        g_warning ("test sensor: cannot wake the scan thread");
      g_thread_join (w->thread);
      g_free (w);
    }
  while (self->stopping > 0)
    g_main_context_iteration (self->ctx, TRUE);
  self->open = FALSE;
}

static void
gxd_sensor_finalize (GObject *object)
{
  GxdSensor *self = GXD_SENSOR (object);

  if (self->conn_fd >= 0)
    close (self->conn_fd);
  if (self->listen_fd >= 0)
    close (self->listen_fd);
  close (self->wake[0]);
  close (self->wake[1]);
  g_main_context_unref (self->ctx);
  G_OBJECT_CLASS (gxd_sensor_parent_class)->finalize (object);
}

static void
gxd_sensor_class_init (GxdSensorClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = gxd_sensor_finalize;
  signals[SIGNAL_FINGER] = g_signal_new ("finger", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                                         0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_BOOLEAN);
  signals[SIGNAL_SCAN] = g_signal_new ("scan", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                                       0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_POINTER);
  signals[SIGNAL_LOST] = g_signal_new ("lost", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                                       0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_ERROR);
}

static void
gxd_sensor_init (GxdSensor *self)
{
  struct sockaddr_un addr = { .sun_family = AF_UNIX };

  self->ctx = g_main_context_ref_thread_default ();
  self->conn_fd = -1;
  if (!g_unix_open_pipe (self->wake, FD_CLOEXEC, NULL) ||
      !g_unix_set_fd_nonblocking (self->wake[0], TRUE, NULL))
    g_error ("test sensor: pipe");
  self->listen_fd = socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  g_assert (gxd_test_socket && strlen (gxd_test_socket) < sizeof (addr.sun_path));
  strcpy (addr.sun_path, gxd_test_socket);
  g_unlink (gxd_test_socket);
  if (bind (self->listen_fd, (struct sockaddr *) &addr, sizeof (addr)) != 0 ||
      listen (self->listen_fd, 1) != 0)
    g_error ("test sensor: cannot listen on %s: %s", gxd_test_socket, g_strerror (errno));
}

GxdSensor *
gxd_sensor_new (void)
{
  return g_object_new (GXD_TYPE_SENSOR, NULL);
}
