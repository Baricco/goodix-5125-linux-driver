/* gxd-sensor.h - the sensor as seen by the service.
 *
 * The core is blocking: opening and capture run in a worker thread, and
 * events reach the main context as signals.
 * All functions must be called from the main context.
 *
 * Signals:
 *   "finger"  (gboolean present)   finger placed/lifted
 *   "scan"    (GxdScan *scan)      a processed touch: frame or "retry"
 *   "lost"    (GError *error)      sensor no longer reachable; the scan
 *                                  stops by itself
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <gio/gio.h>

#include "goodix_sift.h"

#define GXD_IMG_W 80
#define GXD_IMG_H 64

typedef enum {
  GXD_SCAN_FRAME,       /* valid image with at least GXD_MIN_KEYPOINTS */
  GXD_SCAN_RETRY,       /* touch too weak/poor: to be repeated */
} GxdScanKind;

typedef struct {
  GxdScanKind       kind;
  guint8            img8[GXD_IMG_W * GXD_IMG_H];
  struct gx_featset fs;
} GxdScan;

void gxd_scan_free (GxdScan *scan);

#define GXD_TYPE_SENSOR (gxd_sensor_get_type ())
G_DECLARE_FINAL_TYPE (GxdSensor, gxd_sensor, GXD, SENSOR, GObject)

/* The USB sensor or, only in the test binary, the simulated sensor driven
 * through a socket: see gxd-sensor-test.c. */
GxdSensor *gxd_sensor_new (void);

/* TRUE if the 27c6:5125/5135 reader is connected. */
gboolean   gxd_sensor_present (void);

/* Full opening (USB, PSK, TLS, baseline). Cancellable. */
void       gxd_sensor_open (GxdSensor *self, GCancellable *cancellable,
                            GAsyncReadyCallback callback, gpointer user_data);
gboolean   gxd_sensor_open_finish (GxdSensor *self, GAsyncResult *res, GError **error);

/* Closing: stops any scan and releases the device. */
void       gxd_sensor_close (GxdSensor *self, GAsyncReadyCallback callback,
                             gpointer user_data);
void       gxd_sensor_close_finish (GxdSensor *self, GAsyncResult *res);

gboolean   gxd_sensor_is_open (GxdSensor *self);

/* Continuous scan: one "scan" event per touch, until it is stopped.
 * After every touch the thread waits for the finger to be lifted. */
void       gxd_sensor_scan_start (GxdSensor *self);
void       gxd_sensor_scan_stop (GxdSensor *self, GAsyncReadyCallback callback,
                                 gpointer user_data);
void       gxd_sensor_scan_stop_finish (GxdSensor *self, GAsyncResult *res);
gboolean   gxd_sensor_is_scanning (GxdSensor *self);

/* Service exit: stops the scan and closes, synchronously. */
void       gxd_sensor_shutdown (GxdSensor *self);
