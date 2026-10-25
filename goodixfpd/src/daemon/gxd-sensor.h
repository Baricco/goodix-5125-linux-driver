/* gxd-sensor.h - il sensore visto dal servizio.
 *
 * Il core e' bloccante: apertura e acquisizione girano in un thread di
 * lavoro, gli eventi arrivano nel contesto principale come segnali.
 * Tutte le funzioni vanno chiamate dal contesto principale.
 *
 * Segnali:
 *   "finger"  (gboolean present)   dito appoggiato/sollevato
 *   "scan"    (GxdScan *scan)      un tocco elaborato: frame o "riprova"
 *   "lost"    (GError *error)      sensore non piu' raggiungibile; la
 *                                  scansione si ferma da sola
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <gio/gio.h>

#include "goodix_sift.h"

#define GXD_IMG_W 80
#define GXD_IMG_H 64

typedef enum {
  GXD_SCAN_FRAME,       /* immagine valida con almeno GXD_MIN_KEYPOINTS */
  GXD_SCAN_RETRY,       /* tocco troppo debole/povero: va ripetuto */
} GxdScanKind;

typedef struct {
  GxdScanKind       kind;
  guint8            img8[GXD_IMG_W * GXD_IMG_H];
  struct gx_featset fs;
} GxdScan;

void gxd_scan_free (GxdScan *scan);

#define GXD_TYPE_SENSOR (gxd_sensor_get_type ())
G_DECLARE_FINAL_TYPE (GxdSensor, gxd_sensor, GXD, SENSOR, GObject)

/* Il sensore USB, oppure (solo nel binario di test) il sensore simulato
 * comandato da un socket: vedi gxd-sensor-test.c. */
GxdSensor *gxd_sensor_new (void);

/* TRUE se il lettore 27c6:5125/5135 e' collegato. */
gboolean   gxd_sensor_present (void);

/* Apertura completa (USB, PSK, TLS, baseline). Annullabile. */
void       gxd_sensor_open (GxdSensor *self, GCancellable *cancellable,
                            GAsyncReadyCallback callback, gpointer user_data);
gboolean   gxd_sensor_open_finish (GxdSensor *self, GAsyncResult *res, GError **error);

/* Chiusura: ferma l'eventuale scansione e rilascia il dispositivo. */
void       gxd_sensor_close (GxdSensor *self, GAsyncReadyCallback callback,
                             gpointer user_data);
void       gxd_sensor_close_finish (GxdSensor *self, GAsyncResult *res);

gboolean   gxd_sensor_is_open (GxdSensor *self);

/* Scansione continua: un evento "scan" per tocco, finche' non si ferma.
 * Dopo ogni tocco il thread aspetta che il dito si sollevi. */
void       gxd_sensor_scan_start (GxdSensor *self);
void       gxd_sensor_scan_stop (GxdSensor *self, GAsyncReadyCallback callback,
                                 gpointer user_data);
void       gxd_sensor_scan_stop_finish (GxdSensor *self, GAsyncResult *res);
gboolean   gxd_sensor_is_scanning (GxdSensor *self);

/* Uscita del servizio: ferma la scansione e chiude, in modo sincrono. */
void       gxd_sensor_shutdown (GxdSensor *self);
