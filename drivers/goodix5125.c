/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Driver libfprint per il lettore d'impronte Goodix 27c6:5125.
 *
 * SCHELETRO. Mappa il prototipo Python (driver_5125.py) sulla struttura di un
 * image-device di libfprint. Le funzioni di protocollo (framing A0, provisioning E0,
 * TLS-PSK, decodifica immagine) sono specificate in docs/protocol.md e vanno portate
 * dal prototipo; qui sono dichiarate come stub con il contratto atteso.
 *
 * Trasporto: interfaccia CDC Data, bulk OUT 0x01 / IN 0x81, pacchetti 64 byte.
 * Catena: init -> (provisioning PSK se assente) -> TLS-PSK -> arma FDT -> cattura.
 */
#include "drivers_api.h"
#include "goodix5125.h"

struct _FpiDeviceGoodix5125
{
  FpImageDevice parent;
  guint8        psk[GOODIX5125_PSK_LEN];   /* PSK Linux, persistita fuori dal device */
  gpointer      tls;                       /* contesto TLS-PSK (host = server) */
};
G_DECLARE_FINAL_TYPE (FpiDeviceGoodix5125, fpi_device_goodix5125, FPI,
                      DEVICE_GOODIX5125, FpImageDevice)
G_DEFINE_TYPE (FpiDeviceGoodix5125, fpi_device_goodix5125, FP_TYPE_IMAGE_DEVICE)

/* --- Stub di protocollo (da portare da driver_5125.py / tools/pairing.py) --------- */

/* Costruisce la busta 0xbb010003 e il validator da una PSK. Algoritmo in
 * tools/pairing.py, costanti verificate sul firmware 12508. In C riusare OpenSSL
 * (HMAC-SHA256 counter KDF, AES-256-GCM) come in grold goodix_pairing_crypto.c. */
static gboolean goodix5125_build_envelope (const guint8 psk[GOODIX5125_PSK_LEN],
                                           guint8 envelope[GOODIX5125_ENVELOPE_LEN],
                                           guint8 validator[GOODIX5125_VALIDATOR_LEN]);

/* Legge bb010002 esistente, invia un E0 con TLV(bb010002)+TLV(bb010003), verifica
 * il validator riletto da bb020003. Non puo' bloccare il chip (solo settore 4). */
static void goodix5125_provision (FpDevice *dev, FpiSsm *ssm);

/* Handshake TLS-PSK sui record B0; identity "Client_identity". */
static void goodix5125_tls_handshake (FpDevice *dev, FpiSsm *ssm);

/* mcu_get_image -> record TLS -> decifra -> spacchetta (colonna-major) -> FpImage. */
static void goodix5125_capture (FpDevice *dev, FpiSsm *ssm);

/* --- Macchina a stati di attivazione ---------------------------------------------- */

enum goodix5125_activate_state {
  GOODIX5125_ACT_INIT,          /* nop, firmware_version, enable_chip, reset, read_otp */
  GOODIX5125_ACT_CONFIG,        /* upload_config_mcu (0x90, 224 byte), scan freq */
  GOODIX5125_ACT_PROVISION,     /* E0 solo se il validator non combacia con la PSK locale */
  GOODIX5125_ACT_TLS,           /* request_tls_connection + handshake + tls_established */
  GOODIX5125_ACT_ARM_FDT,       /* mcu_switch_to_fdt_mode / fdt_down */
  GOODIX5125_ACT_NUM_STATES,
};

static void
goodix5125_activate_run (FpiSsm *ssm, FpDevice *dev)
{
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case GOODIX5125_ACT_INIT:
    case GOODIX5125_ACT_CONFIG:
      /* TODO: portare da driver_5125.py init_and_capture() */
      fpi_ssm_next_state (ssm);
      break;
    case GOODIX5125_ACT_PROVISION:
      goodix5125_provision (dev, ssm);
      break;
    case GOODIX5125_ACT_TLS:
      goodix5125_tls_handshake (dev, ssm);
      break;
    case GOODIX5125_ACT_ARM_FDT:
      fpi_ssm_mark_completed (ssm);
      break;
    }
}

/* --- Ciclo image-device ----------------------------------------------------------- */

static void
goodix5125_activate (FpImageDevice *idev)
{
  FpDevice *dev = FP_DEVICE (idev);
  FpiSsm *ssm = fpi_ssm_new (dev, goodix5125_activate_run, GOODIX5125_ACT_NUM_STATES);
  fpi_ssm_start (ssm, NULL); /* on completed -> fpi_image_device_activate_complete */
}

static void
goodix5125_change_state (FpImageDevice *idev, FpiImageDeviceState state)
{
  if (state == FPI_IMAGE_DEVICE_STATE_CAPTURE)
    goodix5125_capture (FP_DEVICE (idev), NULL); /* -> fpi_image_device_image_captured */
}

static void goodix5125_open (FpImageDevice *idev)  { /* claim intf 1, detach kernel */ fpi_image_device_open_complete (idev, NULL); }
static void goodix5125_close (FpImageDevice *idev) { fpi_image_device_close_complete (idev, NULL); }
static void goodix5125_deactivate (FpImageDevice *idev) { fpi_image_device_deactivate_complete (idev, NULL); }

static const FpIdEntry goodix5125_id_table[] = {
  { .vid = GOODIX5125_VID, .pid = GOODIX5125_PID },
  { .vid = 0, .pid = 0 },
};

static void
fpi_device_goodix5125_class_init (FpiDeviceGoodix5125Class *klass)
{
  FpDeviceClass *dev = FP_DEVICE_CLASS (klass);
  FpImageDeviceClass *img = FP_IMAGE_DEVICE_CLASS (klass);

  dev->id = "goodix5125";
  dev->full_name = "Goodix 27c6:5125 Fingerprint Sensor";
  dev->type = FP_DEVICE_TYPE_USB;
  dev->id_table = goodix5125_id_table;
  dev->scan_type = FP_SCAN_TYPE_PRESS;

  img->img_width = GOODIX5125_WIDTH;
  img->img_height = GOODIX5125_HEIGHT;
  img->bz3_threshold = 12;   /* soglia matcher (vedi tools/match.py) */

  img->img_open = goodix5125_open;
  img->img_close = goodix5125_close;
  img->activate = goodix5125_activate;
  img->deactivate = goodix5125_deactivate;
  img->change_state = goodix5125_change_state;
}

static void fpi_device_goodix5125_init (FpiDeviceGoodix5125 *self) {}
