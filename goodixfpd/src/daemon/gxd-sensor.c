/* gxd-sensor.c - sensore USB 27c6:5125/5135 sopra il core bloccante.
 *
 * Stessa logica del driver libfprint goodixgf che sostituisce:
 *   - apertura: USB -> init (PSK, TLS, baseline) in un thread;
 *   - acquisizione: gx_capture() -> immagine 8 bit (pipeline "ridge") ->
 *     controllo di contrasto -> SIFT; dopo ogni tocco si aspetta che il
 *     dito si sollevi (o 15 s);
 *   - la baseline di finger-detect si ricampiona a freddo alla prima apertura
 *     di ogni avvio (prima lo faceva goodixgf-warmup.service).
 * Le variabili GOODIX_* del core restano valide (drop-in di fprintd.service).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <glib/gstdio.h>

#include "gxd-sensor.h"
#include "gxd-template.h"
#include "goodix.h"

/* Contrasto minimo delle creste (std del passa-banda, conteggi a 12 bit):
 * dito pieno ~110-175, vuoto o sfiorato ~2-5. Sotto soglia si chiede di
 * ripetere invece di confrontare il rumore fisso del sensore.
 * GOODIX_MIN_RIDGE la cambia (0 = disattivato). */
#define MIN_RIDGE           10.0
#define FINGER_UP_TIMEOUT   15000
/* Errori di trasporto consecutivi oltre i quali il sensore e' perso. */
#define MAX_TRANSPORT_ERRORS 10

enum { SIGNAL_FINGER, SIGNAL_SCAN, SIGNAL_LOST, N_SIGNALS };
static guint signals[N_SIGNALS];

struct _GxdSensor
{
  GObject            parent;
  GMainContext      *ctx;
  struct goodix_dev *gx;        /* valido da open a close */
  gboolean           open;
  struct Worker     *worker;    /* scansione in corso */
  guint              stopping;  /* scansioni in attesa di join */
  gint               stop;      /* atomico */
  guint              gen;       /* generazione della scansione corrente */
  gboolean           scanning;
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

/* ---- eventi dal thread al contesto principale ---- */

typedef enum { EV_FINGER, EV_SCAN, EV_LOST } EventType;

typedef struct {
  GxdSensor *self;
  guint      gen;
  EventType  type;
  gboolean   present;
  GxdScan   *scan;
  GError    *error;
} Event;

static void
event_free (gpointer data)
{
  Event *ev = data;

  g_object_unref (ev->self);
  gxd_scan_free (ev->scan);
  g_clear_error (&ev->error);
  g_free (ev);
}

static gboolean
event_dispatch (gpointer data)
{
  Event *ev = data;
  GxdSensor *self = ev->self;

  /* Eventi di una scansione gia' fermata: scartati. */
  if (ev->gen != self->gen || !self->scanning)
    return G_SOURCE_REMOVE;

  switch (ev->type)
    {
    case EV_FINGER:
      g_signal_emit (self, signals[SIGNAL_FINGER], 0, ev->present);
      break;
    case EV_SCAN:
      g_signal_emit (self, signals[SIGNAL_SCAN], 0, ev->scan);
      break;
    case EV_LOST:
      g_signal_emit (self, signals[SIGNAL_LOST], 0, ev->error);
      break;
    }
  return G_SOURCE_REMOVE;
}

static void
post (GxdSensor *self, guint gen, EventType type, gboolean present,
      GxdScan *scan, GError *error)
{
  Event *ev = g_new0 (Event, 1);

  ev->self = g_object_ref (self);
  ev->gen = gen;
  ev->type = type;
  ev->present = present;
  ev->scan = scan;
  ev->error = error;
  g_main_context_invoke_full (self->ctx, G_PRIORITY_DEFAULT, event_dispatch,
                              ev, event_free);
}

/* ---- presenza del lettore ---- */

gboolean
gxd_sensor_present (void)
{
  g_autoptr(GDir) dir = g_dir_open ("/sys/bus/usb/devices", 0, NULL);
  const char *name;

  if (!dir)
    return FALSE;
  while ((name = g_dir_read_name (dir)))
    {
      g_autofree char *vp = g_build_filename ("/sys/bus/usb/devices", name, "idVendor", NULL);
      g_autofree char *pp = g_build_filename ("/sys/bus/usb/devices", name, "idProduct", NULL);
      g_autofree char *vid = NULL, *pid = NULL;

      if (!g_file_get_contents (vp, &vid, NULL, NULL) ||
          !g_file_get_contents (pp, &pid, NULL, NULL))
        continue;
      g_strstrip (vid);
      g_strstrip (pid);
      if (g_str_equal (vid, "27c6") &&
          (g_str_equal (pid, "5125") || g_str_equal (pid, "5135")))
        return TRUE;
    }
  return FALSE;
}

/* ---- apertura ---- */

/* Al primo avvio del sensore dopo il boot la baseline salvata si butta: a
 * freddo le soglie di finger-detect di una sessione calda non scattano piu'
 * e il core la ricampiona durante l'init (senza dito sul sensore). */
static void
reset_baseline_on_new_boot (void)
{
  g_autofree char *boot_id = NULL;
  g_autofree char *stamp = NULL;
  g_autofree char *seen = NULL;
  g_autofree char *dat = NULL;

  if (!g_file_get_contents ("/proc/sys/kernel/random/boot_id", &boot_id, NULL, NULL))
    return;
  if (gx_state_dir_ensure () != 0)
    return;
  stamp = g_build_filename (gx_state_dir (), "boot_id", NULL);
  if (g_file_get_contents (stamp, &seen, NULL, NULL) && g_str_equal (seen, boot_id))
    return;
  dat = g_build_filename (gx_state_dir (), "goodix.dat", NULL);
  if (g_unlink (dat) == 0)
    g_message ("New boot: stored finger-detect baseline discarded, will resample");
  g_file_set_contents_full (stamp, boot_id, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, NULL);
}

static void
open_cancelled (GCancellable *cancellable, gpointer data)
{
  gx_capture_cancel (data);
}

static void
open_thread (GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
  struct goodix_dev *gx = task_data;
  gulong id = 0;
  int r;

  if (cancellable)
    id = g_cancellable_connect (cancellable, G_CALLBACK (open_cancelled), gx, NULL);

  reset_baseline_on_new_boot ();
  r = gx_transport_open (gx);
  if (r == 0)
    r = gx_device_init (gx);
  if (r == 0 && (gx->img_w != GXD_IMG_W || gx->img_h != GXD_IMG_H))
    {
      g_warning ("Unsupported sensor geometry %ux%u (expected %ux%u)",
                 gx->img_w, gx->img_h, GXD_IMG_W, GXD_IMG_H);
      r = -5;
    }
  if (cancellable)
    g_cancellable_disconnect (cancellable, id);

  if (r == 0)
    {
      g_message ("Sensor ready: chip 0x%04x, %ux%u, TLS %s", gx->chipid,
                 gx->img_w, gx->img_h, gx->tls_inited ? "on" : "off");
      g_task_return_boolean (task, TRUE);
      return;
    }
  gx_transport_close (gx);
  if (g_cancellable_is_cancelled (cancellable))
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "open cancelled");
  else
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "sensor initialisation failed (%d)", r);
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
    {
      g_clear_pointer (&self->gx, g_free);
      g_task_return_error (outer, error);
    }
  g_object_unref (outer);
}

void
gxd_sensor_open (GxdSensor *self, GCancellable *cancellable,
                 GAsyncReadyCallback callback, gpointer user_data)
{
  GTask *outer = g_task_new (self, cancellable, callback, user_data);
  g_autoptr(GTask) inner = NULL;

  g_return_if_fail (!self->open && self->gx == NULL);

  self->gx = g_new0 (struct goodix_dev, 1);
  self->gx->vid = GOODIX_VID;
  self->gx->pid = 0;                    /* 0x5125 o 0x5135 */
  inner = g_task_new (self, cancellable, open_done, outer);
  g_task_set_task_data (inner, self->gx, NULL);
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

/* ---- scansione ---- */

typedef struct Worker {
  GxdSensor *self;
  guint      gen;
  GThread   *thread;
} Worker;

static void
core_event_cb (struct goodix_dev *gx, int event, void *user)
{
  Worker *w = user;

  if (event == GX_EV_FINGER_DOWN || event == GX_EV_FINGER_UP)
    post (w->self, w->gen, EV_FINGER, event == GX_EV_FINGER_DOWN, NULL, NULL);
}

static double
min_ridge (void)
{
  const char *e = g_getenv ("GOODIX_MIN_RIDGE");

  return e && *e ? g_ascii_strtod (e, NULL) : MIN_RIDGE;
}

/* Immagine 16 bit -> GxdScan (frame o "riprova"). */
static GxdScan *
process (struct goodix_dev *gx, const uint8_t *img16)
{
  struct gx_imgproc_params p = GX_IMGPROC_SIGFM_PARAMS;
  struct gx_imgproc_signal sig;
  GxdScan *scan = g_new0 (GxdScan, 1);

  gx_imgproc_apply_env (&p);
  scan->kind = GXD_SCAN_RETRY;
  if (gx_imgproc_to8bit (gx, &p, img16, scan->img8) != 0)
    return scan;
  if (gx_imgproc_signal (gx, &p, img16, &sig) == 0)
    {
      g_message ("Frame signal dc=%.0f ridge=%.1f", sig.dc, sig.ridge);
      if (sig.ridge < min_ridge ())
        {
          g_message ("Frame too weak (ridge %.1f), asking to retry", sig.ridge);
          return scan;
        }
    }
  if (gx_sift_extract (scan->img8, GXD_IMG_W, GXD_IMG_H, &scan->fs) != 0 ||
      scan->fs.n < GXD_MIN_KEYPOINTS)
    {
      g_message ("Not enough keypoints (%u), asking to retry", scan->fs.n);
      return scan;
    }
  scan->kind = GXD_SCAN_FRAME;
  return scan;
}

static gpointer
scan_thread (gpointer data)
{
  Worker *w = data;
  GxdSensor *self = w->self;
  struct goodix_dev *gx = self->gx;
  uint8_t *img16 = g_malloc (GF_IMG_MAX);
  const uint16_t want = (uint16_t) (GXD_IMG_W * GXD_IMG_H * 2);
  int errors = 0;

  gx_capture_set_event_cb (gx, core_event_cb, w);
  while (!g_atomic_int_get (&self->stop))
    {
      uint16_t sz = 0;
      int r = gx_capture (gx, img16, &sz);

      if (g_atomic_int_get (&self->stop) || r == -4)
        break;

      if (r == 0 && sz == want)
        {
          GxdScan *scan = process (gx, img16);
          int up;

          errors = 0;
          post (self, w->gen, EV_SCAN, FALSE, scan, NULL);
          /* Il core ha gia' armato il sollevamento (FDT up). Se il sensore
           * non lo segnala entro il timeout, il dito si considera sollevato. */
          up = gx_wait_finger_up (gx, FINGER_UP_TIMEOUT);
          if (up != 0 && up != -4)
            post (self, w->gen, EV_FINGER, FALSE, NULL, NULL);
          continue;
        }

      if (r == -3)
        {
          /* Errore di canale: spesso passa (il core riallaccia il TLS alla
           * prossima acquisizione), ma non all'infinito. */
          if (++errors >= MAX_TRANSPORT_ERRORS || gx->usb_devh == NULL)
            {
              post (self, w->gen, EV_LOST, FALSE, NULL,
                    g_error_new_literal (G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE,
                                         "fingerprint sensor stopped responding"));
              break;
            }
          g_usleep (200 * 1000);
        }
      /* -2: nessun dito entro il timeout del core, si riarma */
    }
  gx_capture_set_event_cb (gx, NULL, NULL);
  g_free (img16);
  return NULL;
}

void
gxd_sensor_scan_start (GxdSensor *self)
{
  Worker *w;

  /* Un solo thread alla volta sul dispositivo: la scansione precedente
   * deve essere completamente ferma (scan_stop completato). */
  g_return_if_fail (self->open && self->worker == NULL && self->stopping == 0);

  w = g_new0 (Worker, 1);
  w->self = self;
  w->gen = ++self->gen;
  self->scanning = TRUE;
  g_atomic_int_set (&self->stop, FALSE);
  w->thread = g_thread_new ("gxd-scan", scan_thread, w);
  self->worker = w;
}

gboolean
gxd_sensor_is_scanning (GxdSensor *self)
{
  return self->scanning;
}

static void
join_thread (GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
  Worker *w = task_data;

  g_thread_join (w->thread);
  g_free (w);
  g_task_return_boolean (task, TRUE);
}

static void
scan_joined (GObject *source, GAsyncResult *res, gpointer data)
{
  GxdSensor *self = GXD_SENSOR (source);
  GTask *outer = data;

  self->stopping--;
  g_task_return_boolean (outer, TRUE);
  g_object_unref (outer);
}

void
gxd_sensor_scan_stop (GxdSensor *self, GAsyncReadyCallback callback, gpointer user_data)
{
  GTask *outer = g_task_new (self, NULL, callback, user_data);
  g_autoptr(GTask) inner = NULL;
  Worker *worker = g_steal_pointer (&self->worker);

  /* Da qui in poi gli eventi della scansione vengono scartati. */
  self->scanning = FALSE;
  self->gen++;
  if (!worker)
    {
      g_task_return_boolean (outer, TRUE);
      g_object_unref (outer);
      return;
    }
  g_atomic_int_set (&self->stop, TRUE);
  gx_capture_cancel (self->gx);
  self->stopping++;
  inner = g_task_new (self, NULL, scan_joined, outer);
  g_task_set_task_data (inner, worker, NULL);
  g_task_run_in_thread (inner, join_thread);
}

void
gxd_sensor_scan_stop_finish (GxdSensor *self, GAsyncResult *res)
{
  g_task_propagate_boolean (G_TASK (res), NULL);
}

/* ---- chiusura ---- */

static void
close_thread (GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
  struct goodix_dev *gx = task_data;

  gx_transport_close (gx);
  g_free (gx);
  g_task_return_boolean (task, TRUE);
}

static void
close_after_stop (GObject *source, GAsyncResult *res, gpointer data)
{
  GxdSensor *self = GXD_SENSOR (source);
  GTask *outer = data;
  struct goodix_dev *gx = g_steal_pointer (&self->gx);

  self->open = FALSE;
  if (!gx)
    {
      g_task_return_boolean (outer, TRUE);
      g_object_unref (outer);
      return;
    }
  g_task_set_task_data (outer, gx, NULL);
  g_task_run_in_thread (outer, close_thread);
  g_object_unref (outer);
}

void
gxd_sensor_close (GxdSensor *self, GAsyncReadyCallback callback, gpointer user_data)
{
  GTask *outer = g_task_new (self, NULL, callback, user_data);

  gxd_sensor_scan_stop (self, close_after_stop, outer);
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
  self->gen++;
  if (w)
    {
      g_atomic_int_set (&self->stop, TRUE);
      gx_capture_cancel (self->gx);
      g_thread_join (w->thread);
      g_free (w);
    }
  /* eventuali join asincroni ancora in corso finiscono a breve */
  while (self->stopping > 0)
    g_main_context_iteration (self->ctx, TRUE);
  if (self->gx)
    {
      gx_transport_close (self->gx);
      g_clear_pointer (&self->gx, g_free);
    }
  self->open = FALSE;
}

/* ---- GObject ---- */

static void
gxd_sensor_finalize (GObject *object)
{
  GxdSensor *self = GXD_SENSOR (object);

  g_warn_if_fail (self->worker == NULL && self->stopping == 0);
  if (self->gx)
    {
      gx_transport_close (self->gx);
      g_free (self->gx);
    }
  g_main_context_unref (self->ctx);
  G_OBJECT_CLASS (gxd_sensor_parent_class)->finalize (object);
}

static void
gxd_sensor_class_init (GxdSensorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gxd_sensor_finalize;
  signals[SIGNAL_FINGER] = g_signal_new ("finger", G_TYPE_FROM_CLASS (klass),
                                         G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                         G_TYPE_NONE, 1, G_TYPE_BOOLEAN);
  signals[SIGNAL_SCAN] = g_signal_new ("scan", G_TYPE_FROM_CLASS (klass),
                                       G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                       G_TYPE_NONE, 1, G_TYPE_POINTER);
  signals[SIGNAL_LOST] = g_signal_new ("lost", G_TYPE_FROM_CLASS (klass),
                                       G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                       G_TYPE_NONE, 1, G_TYPE_ERROR);
}

static void
gxd_sensor_init (GxdSensor *self)
{
  self->ctx = g_main_context_ref_thread_default ();
}

GxdSensor *
gxd_sensor_new (void)
{
  return g_object_new (GXD_TYPE_SENSOR, NULL);
}
