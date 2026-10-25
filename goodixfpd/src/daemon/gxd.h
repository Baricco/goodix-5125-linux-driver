/* gxd.h - goodixfpd: servizio compatibile con l'API D-Bus di fprintd per il
 * lettore Goodix 27c6:5125/5135.
 *
 * pam_fprintd, i comandi fprintd-*, GNOME/KDE e la GUI del progetto parlano
 * con net.reactivated.Fprint: questo servizio ne implementa lo stesso
 * contratto (metodi, segnali, errori, controlli polkit) sopra il core del
 * sensore, senza modificare libfprint ne' fprintd.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <gio/gio.h>

#include "fprintd-dbus.h"

#define GXD_SERVICE_NAME  "net.reactivated.Fprint"
#define GXD_SERVICE_PATH  "/net/reactivated/Fprint"
#define GXD_DEVICE_PATH   GXD_SERVICE_PATH "/Device/0"
/* Nome con cui il dispositivo si presentava con libfprint: pam_fprintd lo
 * usa nei messaggi ("Place your finger on ..."). */
#define GXD_DEVICE_NAME   "Goodix fingerprint (27c6:5125/5135)"

/* Errori D-Bus: stessi nomi e significati di fprintd. */
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

/* Dita: stessa numerazione di FpFinger in libfprint. */
#define GXD_FINGER_UNKNOWN  0
#define GXD_FINGER_FIRST    1
#define GXD_FINGER_LAST     10

const char *gxd_finger_to_name (int finger);      /* 0 -> "any" */
int         gxd_finger_from_name (const char *name); /* sconosciuto -> 0 */

/* Directory dei dati (StateDirectory=fprint di fprintd.service). */
const char *gxd_state_dir (void);
void        gxd_set_state_dir (const char *dir);
