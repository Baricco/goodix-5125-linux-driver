/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Driver libfprint per Goodix 27c6:5125 (firmware GF_ST411SEC_APP_12508).
 * Scheletro: struttura e macchina a stati. La logica di protocollo e' documentata
 * in docs/protocol.md e implementata nel prototipo Python (driver_5125.py). */
#pragma once

#include "fpi-image-device.h"

#define GOODIX5125_VID 0x27c6
#define GOODIX5125_PID 0x5125

#define GOODIX5125_WIDTH  80
#define GOODIX5125_HEIGHT 64
#define GOODIX5125_SAMPLES (GOODIX5125_WIDTH * GOODIX5125_HEIGHT)

/* Formato busta PSK (vedi goodix_pairing in C, algoritmo verificato sul 12508) */
#define GOODIX5125_PSK_LEN       32
#define GOODIX5125_ENVELOPE_LEN  102  /* TLV 0xbb010003 */
#define GOODIX5125_VALIDATOR_LEN 32   /* 0xbb020003 = SHA256(envelope) */
