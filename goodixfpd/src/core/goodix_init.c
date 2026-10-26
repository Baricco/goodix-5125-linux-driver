/* goodix_init.c - device activation sequence
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Device initialization sequence. Only the OS-level APIs (libusb, usleep)
 * differ; the rest of the logic matches the reference implementation.
 *
 * Device init:
 *   device_action(15,{0,0})   BESD off
 *   init MCU
 *   init sensor
 *   device_action(15,{1,0})   BESD on
 *
 * Init MCU:
 *   version handshake with retries  (NOP + 0xA8 x N, N=retry_count)
 *   project 8 (ST32): firmware update (always run)
 *     - firmware update logic: skip on same version / clear app + reset /
 *       IAP download
 *     - returns the firmware soft-reset marker -> init stops (MCU rebooting)
 *   PSK handling  (checks the host hash against the MCU, writes a new PSK if
 *                  they differ)
 *
 * Init sensor:
 *   reset the sensor (SetIdle)
 *   read ChipID x N (0x82) -> chipid
 *   pick the sensor parameters by chipid
 *   read OTP (0xA6) -> parse OTP -> FDT init -> download config (0x90)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "goodix.h"

#define RETRY_COUNT 5

/* defined at the end of the file: after a device re-enumeration, close +
 * poll and reopen (used by evk wake-up level L3) */
static int gx_transport_reopen(struct goodix_dev *d, int max_ms);

/* ================= configuration download tables =================
 * One 224-byte configuration per sensor type, organized by type offset:
 * base + 224*type; at init the matching 224 bytes are sent to the MCU with
 * 0x90 (cmd0=9 cmd1=0).
 * Each one is a real configuration, never all zeros - all zeros would make
 * the MCU capture with wrong exposure/threshold tables.
 * The sensor type is determined by the chipid. */
static const uint8_t gf_config_type2[224] = {   /* MilanG, chipid 0x2208 */
    0x30, 0x11, 0x64, 0x75, 0x00, 0x75, 0x2c, 0xa1, 0x1c, 0xbd, 0x18, 0xd5, 0x00, 0xd5, 0x00, 0xd5,
    0x00, 0xba, 0x00, 0x00, 0x80, 0xca, 0x00, 0x06, 0x00, 0x84, 0x00, 0xbe, 0xb2, 0x86, 0x00, 0xc5,
    0xb9, 0x88, 0x00, 0xb5, 0xad, 0x8a, 0x00, 0x9d, 0x95, 0x8c, 0x00, 0x00, 0xbe, 0x8e, 0x00, 0x00,
    0xc5, 0x90, 0x00, 0x00, 0xb5, 0x92, 0x00, 0x00, 0x9d, 0x94, 0x00, 0x00, 0xaf, 0x96, 0x00, 0x00,
    0xbf, 0x98, 0x00, 0x00, 0xb6, 0x9a, 0x00, 0x00, 0xa7, 0xd2, 0x00, 0x00, 0x00, 0xd4, 0x00, 0x00,
    0x00, 0xd6, 0x00, 0x00, 0x00, 0xd8, 0x00, 0x00, 0x00, 0x12, 0x00, 0x03, 0x04, 0xd0, 0x00, 0x00,
    0x00, 0x70, 0x00, 0x00, 0x00, 0x72, 0x00, 0x78, 0x56, 0x74, 0x00, 0x34, 0x12, 0x20, 0x00, 0x10,
    0x40, 0x20, 0x02, 0x08, 0x10, 0x2a, 0x01, 0x82, 0x03, 0x22, 0x00, 0x01, 0x20, 0x24, 0x00, 0x14,
    0x00, 0x80, 0x00, 0x01, 0x04, 0x5c, 0x00, 0x00, 0x01, 0x56, 0x00, 0x0c, 0x24, 0x58, 0x00, 0x05,
    0x00, 0x32, 0x00, 0x08, 0x02, 0x66, 0x00, 0x00, 0x02, 0x7c, 0x00, 0x00, 0x38, 0x82, 0x00, 0x80,
    0x15, 0x2a, 0x01, 0x08, 0x00, 0x5c, 0x00, 0x80, 0x00, 0x54, 0x00, 0x00, 0x01, 0x62, 0x00, 0x38,
    0x04, 0x64, 0x00, 0x10, 0x00, 0x66, 0x00, 0x00, 0x02, 0x7c, 0x00, 0x01, 0x38, 0x2a, 0x01, 0x08,
    0x00, 0x5c, 0x00, 0x80, 0x00, 0x52, 0x00, 0x08, 0x00, 0x54, 0x00, 0x00, 0x01, 0x66, 0x00, 0x00,
    0x02, 0x7c, 0x00, 0x01, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t gf_config_type3[224] = {   /* MilanL, chipid 0x2205 */
    0x18, 0x11, 0x70, 0x81, 0x00, 0x81, 0x24, 0xa5, 0x10, 0xb5, 0x10, 0xc5, 0x00, 0xc5, 0x00, 0xc5,
    0x00, 0x04, 0x02, 0x00, 0x00, 0x08, 0x00, 0x11, 0x11, 0xba, 0x00, 0x01, 0x80, 0xca, 0x00, 0x07,
    0x00, 0x84, 0x00, 0xbe, 0xb2, 0x86, 0x00, 0xc5, 0xb9, 0x88, 0x00, 0xb5, 0xad, 0x8a, 0x00, 0x9d,
    0x95, 0x8c, 0x00, 0x00, 0xbe, 0x8e, 0x00, 0x00, 0xc5, 0x90, 0x00, 0x00, 0xb5, 0x92, 0x00, 0x00,
    0x9d, 0x94, 0x00, 0x00, 0xaf, 0x96, 0x00, 0x00, 0xbf, 0x98, 0x00, 0x00, 0xb6, 0x9a, 0x00, 0x00,
    0xa7, 0xd2, 0x00, 0x00, 0x00, 0xd4, 0x00, 0x00, 0x00, 0xd6, 0x00, 0x00, 0x00, 0xd8, 0x00, 0x00,
    0x00, 0x50, 0x00, 0x01, 0x05, 0xd0, 0x00, 0x00, 0x00, 0x70, 0x00, 0x00, 0x00, 0x72, 0x00, 0x78,
    0x56, 0x74, 0x00, 0x34, 0x12, 0x20, 0x00, 0x10, 0x40, 0x12, 0x00, 0x03, 0x04, 0x20, 0x02, 0x08,
    0x10, 0x2a, 0x01, 0x82, 0x03, 0x22, 0x00, 0x01, 0x20, 0x24, 0x00, 0x14, 0x00, 0x80, 0x00, 0x01,
    0x00, 0x5c, 0x00, 0x00, 0x01, 0x56, 0x00, 0x08, 0x2c, 0x58, 0x00, 0x03, 0x00, 0x32, 0x00, 0x08,
    0x04, 0x82, 0x00, 0x80, 0x15, 0x2a, 0x01, 0x08, 0x00, 0x5c, 0x00, 0x80, 0x00, 0x62, 0x00, 0x0a,
    0x04, 0x64, 0x00, 0x18, 0x00, 0x2a, 0x01, 0x08, 0x00, 0x5c, 0x00, 0x80, 0x00, 0x52, 0x00, 0x08,
    0x00, 0x54, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t gf_config_type12[224] = {  /* ChicagoHS, chipid 0x2503/0x2504 */
    0x70, 0x11, 0x74, 0x85, 0x00, 0x85, 0x2c, 0xb1, 0x18, 0xc9, 0x14, 0xdd, 0x00, 0xdd, 0x00, 0xdd,
    0x00, 0xba, 0x00, 0x01, 0x80, 0xca, 0x00, 0x04, 0x00, 0x84, 0x00, 0x15, 0xb3, 0x86, 0x00, 0x00,
    0xc4, 0x88, 0x00, 0x00, 0xba, 0x8a, 0x00, 0x00, 0xb2, 0x8c, 0x00, 0x00, 0xaa, 0x8e, 0x00, 0x00,
    0xc1, 0x90, 0x00, 0xbb, 0xbb, 0x92, 0x00, 0xb1, 0xb1, 0x94, 0x00, 0x00, 0xa8, 0x96, 0x00, 0x00,
    0xb6, 0x98, 0x00, 0x00, 0x00, 0x9a, 0x00, 0x00, 0x00, 0xd2, 0x00, 0x00, 0x00, 0xd4, 0x00, 0x00,
    0x00, 0xd6, 0x00, 0x00, 0x00, 0xd8, 0x00, 0x00, 0x00, 0x50, 0x00, 0x01, 0x05, 0xd0, 0x00, 0x00,
    0x00, 0x70, 0x00, 0x00, 0x00, 0x72, 0x00, 0x78, 0x56, 0x74, 0x00, 0x34, 0x12, 0x20, 0x00, 0x10,
    0x40, 0x5c, 0x00, 0x80, 0x01, 0x20, 0x02, 0x08, 0x08, 0x36, 0x02, 0x80, 0x00, 0x38, 0x02, 0x80,
    0x00, 0x3a, 0x02, 0x80, 0x00, 0x2a, 0x01, 0x82, 0x03, 0x22, 0x00, 0x01, 0x20, 0x24, 0x00, 0x14,
    0x00, 0x80, 0x00, 0x01, 0x00, 0x5c, 0x00, 0x00, 0x01, 0x56, 0x00, 0x04, 0x20, 0x58, 0x00, 0x03,
    0x02, 0x32, 0x00, 0x0c, 0x02, 0x66, 0x00, 0x03, 0x00, 0x7c, 0x00, 0x00, 0x58, 0x82, 0x00, 0x80,
    0x15, 0x2a, 0x01, 0x08, 0x00, 0x54, 0x00, 0x10, 0x01, 0x62, 0x00, 0x04, 0x03, 0x64, 0x00, 0x19,
    0x00, 0x66, 0x00, 0x03, 0x00, 0x7c, 0x00, 0x00, 0x58, 0x2a, 0x01, 0x08, 0x00, 0x52, 0x00, 0x08,
    0x00, 0x54, 0x00, 0x00, 0x01, 0x66, 0x00, 0x03, 0x00, 0x7c, 0x00, 0x00, 0x58, 0x00, 0x00, 0x00,
};
static const uint8_t gf_config_type14[224] = {  /* ChicagoT, chipid 0x2505-0x2508/0x2510 */
    0x90, 0x11, 0x74, 0x85, 0x00, 0x85, 0x2c, 0xb1, 0x18, 0xc9, 0x14, 0xdd, 0x00, 0xdd, 0x00, 0xdd,
    0x00, 0xba, 0x00, 0x01, 0x80, 0xca, 0x00, 0x07, 0x00, 0x84, 0x00, 0x15, 0xb3, 0x86, 0x00, 0x00,
    0xc4, 0x88, 0x00, 0x00, 0xba, 0x8a, 0x00, 0x00, 0xb2, 0x8c, 0x00, 0x00, 0xaa, 0x8e, 0x00, 0x00,
    0xc1, 0x90, 0x00, 0x00, 0xbb, 0x92, 0x00, 0x00, 0xb1, 0x94, 0x00, 0x00, 0xa8, 0x96, 0x00, 0x00,
    0xb6, 0x98, 0x00, 0x00, 0x00, 0x9a, 0x00, 0x00, 0x00, 0xd2, 0x00, 0x00, 0x00, 0xd4, 0x00, 0x00,
    0x00, 0xd6, 0x00, 0x00, 0x00, 0xd8, 0x00, 0x00, 0x00, 0x50, 0x00, 0x01, 0x05, 0xd0, 0x00, 0x00,
    0x00, 0x70, 0x00, 0x00, 0x00, 0x72, 0x00, 0x78, 0x56, 0x74, 0x00, 0x34, 0x12, 0x20, 0x00, 0x10,
    0x40, 0x5c, 0x00, 0x80, 0x01, 0x20, 0x02, 0x08, 0x08, 0x36, 0x02, 0x80, 0x00, 0x38, 0x02, 0x80,
    0x00, 0x3a, 0x02, 0x80, 0x00, 0x2a, 0x01, 0x82, 0x03, 0x22, 0x00, 0x01, 0x20, 0x24, 0x00, 0x14,
    0x00, 0x80, 0x00, 0x01, 0x00, 0x5c, 0x00, 0x00, 0x01, 0x56, 0x00, 0x04, 0x14, 0x58, 0x00, 0x02,
    0x02, 0x32, 0x00, 0x0c, 0x05, 0x66, 0x00, 0x03, 0x00, 0x7c, 0x00, 0x00, 0xbc, 0x82, 0x00, 0x80,
    0x15, 0x2a, 0x01, 0x08, 0x00, 0x54, 0x00, 0x10, 0x01, 0x62, 0x00, 0x04, 0x03, 0x64, 0x00, 0x0a,
    0x00, 0x66, 0x00, 0x03, 0x00, 0x7c, 0x00, 0x00, 0xbc, 0x2a, 0x01, 0x08, 0x00, 0x52, 0x00, 0x08,
    0x00, 0x54, 0x00, 0x00, 0x01, 0x66, 0x00, 0x03, 0x00, 0x7c, 0x00, 0x00, 0xbc, 0x00, 0xbc, 0xff,
};

/* chipid -> sensor type/geometry/config (switch on chipid +
 * geometry table: [0]=col [1]=row [4]=frame bytes) */
struct gf_sensor_info {
    uint8_t type;
    uint16_t w, h;
    const uint8_t *config;
};
static const struct gf_sensor_info *gf_sensor_by_chipid(uint16_t chipid)
{
    static const struct gf_sensor_info tbl[] = {
        {  2, 176,  54, gf_config_type2  },   /* 0x2208 MilanG (GF5116M) */
        {  3, 112, 132, gf_config_type3  },   /* 0x2205 MilanL */
        { 12,  80,  64, gf_config_type12 },   /* 0x2503/0x2504 ChicagoHS */
        { 14,  36, 160, gf_config_type14 },   /* 0x2505-0x2508/0x2510 ChicagoT */
    };
    int idx;
    if (chipid == 0x2205)            idx = 1;
    else if (chipid == 0x2208)       idx = 0;
    else if (chipid == 0x2503 || chipid == 0x2504)
        idx = 2;
    else if ((chipid >= 0x2505 && chipid <= 0x2508) || chipid == 0x2510)
        idx = 3;
    else
        return NULL;
    return &tbl[idx];
}

/* ================= command helpers ================ */

/* Version handshake with retries: after N attempts, MCU soft reset + retry.
 *
 * Reference flow:
 *   for i in retry_count:
 *       read version (NOP + 0xA8)          [ack 100ms, data 500ms, evt 9]
 *       if ok: return
 *   all failed:
 *       MCU hard reset command:
 *           0xA2 {2,20}, ack_tmo=0, data_tmo=0, evt=-1 (fire & forget)
 *           ({2,20} = reset MCU, {1,20} = reset sensor)
 *       read the version again after the reset
 *
 * This is the device's "wake-up" mechanism: a freshly plugged / sleeping MCU
 * does not answer 0xA8 until it receives the 0xA2 {2,20} soft reset.
 */
static int evk_with_retry(struct goodix_dev *d, uint8_t out[64])
{
    int r;

    /* level 0: version read only (NOP + 0xA8), retry_count attempts */
    for (int i = 0; i < RETRY_COUNT; i++) {
        if (gx_stop_requested(d))
            return -4;              /* cancelled (fprintd stop): leave quickly */
        memset(out, 0, 64);
        r = gx_dev_evk(d, out);
        if (r == 0)
            return 0;
        LOG("GetEvkVersion try %d failed (%d)", i + 1, r);
        usleep(100000);
    }

    /* level 1: MCU soft reset 0xA2 {2,20}. Wakes up a sleeping / cold MCU. */
    if (gx_stop_requested(d))
        return -4;
    LOG("wake L1: soft-reset MCU (0xA2 {2,20})");
    uint8_t reset_mcu[2] = { 2, 20 };
    gx_send_cmd(d, 0xA2, reset_mcu, 2, 100);
    usleep(600000);
    memset(out, 0, 64);
    r = gx_dev_evk(d, out);
    if (r == 0)
        return 0;
    LOG("GetEvkVersion after L1 wake failed (%d)", r);

    /* level 2: reset MCU + sensor 0xA2 {3,20} */
    if (gx_stop_requested(d))
        return -4;
    LOG("wake L2: soft-reset MCU+sensor (0xA2 {3,20})");
    uint8_t reset_both[2] = { 3, 20 };
    gx_send_cmd(d, 0xA2, reset_both, 2, 100);
    usleep(600000);
    memset(out, 0, 64);
    r = gx_dev_evk(d, out);
    if (r == 0)
        return 0;
    LOG("GetEvkVersion after L2 wake failed (%d)", r);

    /* level 3: USB port reset (the host usually does it when the device is
     * added; libusb_open does not). Wakes the MCU's USB peripheral again.
     * gx_usb_reset redoes CDC activation internally (a port reset clears
     * DTR), otherwise after the reset the device still silently drops
     * packets; if there is still no answer, a full close + reopen
     * (detach the kernel driver / claim / CDC activation again) covers the
     * case where the kernel rebinds cdc_acm after the reset and the handle
     * becomes invalid. */
    if (gx_stop_requested(d))
        return -4;
    LOG("wake L3: USB port reset + retry");
    gx_usb_reset(d);
    usleep(600000);
    memset(out, 0, 64);
    r = gx_dev_evk(d, out);
    if (r == 0)
        return 0;
    LOG("GetEvkVersion after L3 USB reset failed (%d)", r);
    if (gx_transport_reopen(d, 5000) == 0) {
        memset(out, 0, 64);
        r = gx_dev_evk(d, out);
        if (r == 0)
            return 0;
        LOG("GetEvkVersion after L3 reopen failed (%d)", r);
    }

    return -1;
}

/* read OTP: 0xA6 {0,0}, evt 8.
 * The valid OTP length comes from the sensor parameter table of the
 * reference implementation: type 12/14 = 64 bytes (the device really returns
 * only 64 B of data). */
static int cmd_read_otp(struct goodix_dev *d, uint8_t *out, uint16_t len)
{
    uint8_t data[2] = { 0, 0 };
    uint8_t rsp[256];
    uint16_t rl = sizeof(rsp);
    int r = gx_send_cmd_wait(d, GF_CMD_READ_OTP, data, 2, rsp, &rl, 500);
    if (r == 0 && out && rl > 0) {
        uint16_t n = rl < len ? rl : len;
        memcpy(out, rsp, n);
        d->otp_len = n;         /* real valid OTP length (type 12 = 64) */
    }
    return r;
}

/* config download: 0x90 config, evt 3 (224 bytes) */
static int cmd_download_config(struct goodix_dev *d, const uint8_t *cfg, uint16_t len)
{
    uint8_t rsp[8]; uint16_t rl = sizeof(rsp);
    /* reset the sensor into Idle first, then download the config */
    gx_dev_reset(d);
    usleep(50000);
    return gx_send_cmd_wait(d, GF_CMD_DOWNLOAD_CONFIG, cfg, len, rsp, &rl, 500);
}

/* set BESD (device_action 15): local flag only, no USB command */
static void cmd_set_besd(struct goodix_dev *d, uint8_t en)
{
    LOG("SetBesDenable(%d)", en);
    d->besd = en;
}

/* ================= init MCU ================================== */

static int init_mcu(struct goodix_dev *d)
{
    uint8_t evk[64] = { 0 };
    int r;

    /* version handshake (with retries) */
    r = evk_with_retry(d, evk);
    if (r < 0) {
        LOG("GetEvkVersion failed - MCU unresponsive");
        return r;
    }
    LOG("MCU version: %.32s", evk);
    memcpy(d->evk, evk, 64);

    /* set driver state: 0x97 {1,2}, after the version handshake */
    gx_set_driver_state(d, 1);

    /* read MCU state: 0xAF {0x55, ts16le, 0, 0}, evt6.
     * The reference flow calls it on every init; it returns the 16-byte MCU
     * state, whose byte[1] flags drive the POV/capture path
     * ((state[1] & 2) = pov_valid). Here it is only logged for diagnosis. */
    {
        uint8_t mcu_state[16] = { 0 };
        int sr = gx_get_mcu_state(d, mcu_state);
        if (sr == 0) {
            LOG("MCU state: %02X %02X %02X %02X %02X %02X %02X %02X "
                "(pov_valid=%d)", mcu_state[0], mcu_state[1], mcu_state[2],
                mcu_state[3], mcu_state[4], mcu_state[5], mcu_state[6],
                mcu_state[7], (mcu_state[1] & 2) ? 1 : 0);
        } else {
            LOG("GetMcuState failed (%d)", sr);
        }
    }

    /* project 8 (ST32): firmware update always runs.
     * Internal logic: skip on same version / clear app + reset / IAP
     * download. */
    r = gx_fw_update(d, evk);
    if (r == FW_SOFT_RESET) {
        /* MCU is rebooting into APP; stop init, wait for next run.
         * (init stops after the firmware soft-reset marker is returned) */
        LOG("firmware updated, MCU reset - init deferred");
        return FW_SOFT_RESET;
    }
    if (r < 0) {
        LOG("firmware update failed (%d)", r);
        return r;
    }
    /* r == 1 (same version) or 0: continue */

    /* PSK handling:
     *   1. check that the PSK is valid (host hash vs MCU hash 0xBB020003),
     *      2 attempts
     *   2. if invalid, write a new PSK (random -> WB encryption -> TLV 0xE0),
     *      2 attempts
     * Host side: the PSK is persisted in a 0600 file (the equivalent of the
     * reference implementation's cache file; the reference seals it with
     * DPAPI, but the MCU never checks the seal, so any Linux storage works:
     * a file for now, libsecret could be added later). */
    r = -1;
    /* try local store first (avoids MCU read; we wrote it last run).
     * GOODIX_RESET_PSK=1 forces a fresh random PSK + rewrite (use after
     * the white-box AES key fix; the MCU still holds a blob made with the
     * OLD key, whose decrypted PSK differs from what we have locally). */
    if (!getenv("GOODIX_RESET_PSK")) {
        uint32_t plen = 32;
        if (gx_psk_store_load(d->psk, &plen) == 0 && plen == 32) {
            d->psk_len = 32;
            d->have_psk = true;
            r = 0;
            LOG("PSK recovered from local store");
        }
    } else {
        LOG("GOODIX_RESET_PSK: ignoring local store, will rewrite");
    }
    /* Self-healing (key point): the local cache is only a cache; it must hold
     * the same PSK as the MCU or the TLS handshake cannot succeed. The local
     * cache is verified with the MCU hash (0xBB020003):
     *   - match (0)       -> use it;
     *   - mismatch (-6)   -> stale cache, dropped, then the "read MCU /
     *     rewrite" path; typical case: the CLI (~/.config/goodix) and fprintd
     *     (/var/lib/fprint/goodix) use different state directories but share
     *     the same MCU, so the last writer overwrites the MCU's PSK and the
     *     other cache goes stale; or the MCU still holds an old blob after a
     *     WB key change. Trusting a stale cache always ends in
     *     bad_record_mac, the MCU locks up and every later evk times out
     *     with no way to self-heal.
     *   - other errors    -> the transport cannot answer right now: keep the
     *     local PSK and check again at the next init. */
    if (r == 0) {
        int vr = gx_psk_verify_mcu_hash(d);
        if (vr == -6) {
            LOG("local PSK mismatch with MCU - re-sync");
            r = -1;
        } else if (vr != 0) {
            LOG("PSK verify failed (%d), keeping local PSK", vr);
        }
    }
    /* Recovery path (no local store / cache expired): try MCU WB read.
     * 0xBB010003 is the WB storage segment (production_write_key 2nd TLV).
     * Success means the WB decryption has verified that the keys agree, and
     * the MCU's PSK is saved back to the local cache.
     * With GOODIX_RESET_PSK the MCU read is skipped and a new PSK is forced
     * (as the comment says). */
    if (r != 0 && !getenv("GOODIX_RESET_PSK")) {
        r = gx_psk_read_from_mcu(d);
        if (r == 0)
            gx_psk_store_save(d->psk, 32);
    }
    if (r == 0) {
        LOG("PSK valid, no update needed");
    } else {
        LOG("PSK invalid (%d), writing new PSK", r);
        uint8_t new_psk[32];
        FILE *ur = fopen("/dev/urandom", "rb");
        if (ur) {
            size_t got = fread(new_psk, 1, 32, ur);
            fclose(ur);
            if (got == 32) {
                r = gx_psk_write_to_mcu(d, new_psk);
                if (r == 0) {
                    LOG("PSK written, persist + verify");
                    gx_psk_store_save(new_psk, 32);
                    /* don't re-read: local store already holds the PSK;
                     * verify via MCU hash (0xBB020003) instead. */
                    memcpy(d->psk, new_psk, 32);
                    d->psk_len = 32;
                    d->have_psk = true;
                    r = 0;
                    gx_psk_verify_mcu_hash(d);
                } else {
                    LOG("PSK write failed (%d)", r);
                }
            }
        }
    }
    d->have_psk = (r == 0);

    d->mcu_inited = true;
    return 0;
}

/* ================= init sensor =============================== */

static int init_fpsensor(struct goodix_dev *d)
{
    uint8_t otp[256] = { 0 };
    uint8_t rsp[16];
    int r;

    /* --- reset the sensor into Idle --- */
    LOG("SetIdle (ResetSensor)");
    r = gx_dev_reset(d);
    if (r < 0) {
        LOG("switch to idle failed");
        return r;
    }
    usleep(100000);

    /* --- read ChipID: register 0x82, 4 bytes x retry --- */
    uint16_t chipid = 0;
    for (int i = 0; i < RETRY_COUNT; i++) {
        if (gx_stop_requested(d))
            return -4;
        uint16_t rl = 16;
        r = gx_send_cmd_wait(d, GF_CMD_CHIPREG_READ,
                             (const uint8_t[]){ 0, 0, 0, 4, 0 }, 5,
                             rsp, &rl, 500);
        if (r == 0 && rl >= 3) {
            /* chipid = Dst[1] | Dst[2]<<8 */
            chipid = (uint16_t)(rsp[1] | (rsp[2] << 8));
            LOG("Get Chip ID: 0x%04x", chipid);
            break;
        }
        LOG("read chipid failed, try again, reset sensor");
        gx_dev_reset(d);
        usleep(100000);
    }
    d->chipid = chipid;
    if (chipid == 0) {
        LOG("Get Chip ID Failed");
        return -1;
    }

    /* --- pick the sensor parameters by chipid ---
     * The chipid decides the sensor type / geometry / 224 B config table; an
     * unknown chipid takes the "wrong senseor type kept" path of the
     * reference implementation and counts as an init failure, and here too. */
    const struct gf_sensor_info *si = gf_sensor_by_chipid(chipid);
    if (!si) {
        LOG("unknown chipid 0x%04x - no sensor config, init fails",
            chipid);
        return -1;
    }
    d->sensor_type = si->type;
    d->img_w = si->w;
    d->img_h = si->h;
    d->img_size = (uint16_t)(si->w * si->h * 2);
    LOG("sensor type %d, %ux%u, frame %u bytes",
        si->type, si->w, si->h, d->img_size);

    /* --- OTP: read OTP -> parse OTP -> FDT init -> download config ---
     * Order: read OTP -> parse (patch the 224 B config table on the spot
     * with the OTP factory calibration) -> FDT init -> download the patched
     * config. Sending the static table unpatched = FDT thresholds/exposure
     * all at generic defaults, and finger detection never fires. */
    r = cmd_read_otp(d, otp, sizeof(otp));
    if (r == 0) {
        LOG("Success to get OTP data (%d bytes)", (int)d->otp_len);
        memcpy(d->otp, otp, sizeof(otp));   /* binding credential of goodix.dat */
        /* baseline presence check: load the FDT baseline (CRC + OTP check) */
        gx_base_load(d);
        /* OTP parsing: patch the config table from the OTP
         * (tcode/delta/offset/DAC) */
        {
            uint8_t cfg[224];
            memcpy(cfg, si->config, sizeof(cfg));
            d->fdt_delta = 21;              /* reference implementation defaults */
            if (gx_otp_patch_config(d, cfg, &d->fdt_delta))
                LOG("OTP parsed, config patched (fdt_delta=%u)",
                    d->fdt_delta);
            LOG("OTP data valid, download config");
            /* download the 224 B config: send the patched config */
            r = cmd_download_config(d, cfg, sizeof(cfg));
        }
        d->have_config = (r == 0);
        LOG("download config %s", d->have_config ? "ok" : "failed");
    } else {
        LOG("fail to get OTP data, reset sensor");
        gx_dev_reset(d);
    }

    d->fp_inited = true;
    return 0;
}

/* ================= device init (main entry point) ========================== */

static int device_init(struct goodix_dev *d)
{
    int r;

    cmd_set_besd(d, 0);

    r = init_mcu(d);
    if (r < 0)
        return r;   /* includes FW_SOFT_RESET */

    r = init_fpsensor(d);
    if (r < 0) {
        LOG("fail to init FP");
        return r;
    }

    cmd_set_besd(d, 1);
    return 0;
}

/* ================= top-level init flow ============================== */

/* After an MCU hard reset (0xA2 {2,20}) the USB device re-enumerates: it
 * disappears from the bus for a few hundred milliseconds, then comes back
 * with a (possibly different) PID. The reference implementation closes and
 * reopens the device at this point. After the reset our libusb handle is
 * invalid (LIBUSB_ERROR_NO_DEVICE), so the handle is closed and we poll
 * until the device reappears, then reopen it (claim + CDC activate). */
static int gx_transport_reopen(struct goodix_dev *d, int max_ms)
{
    gx_transport_close(d);
    /* After an MCU reset the device may re-enumerate with a different PID (see
     * the comment above): scan for either.
     * When gx_transport_open matches it updates d->pid to the real PID. */
    d->pid = 0;
    int waited = 0;
    while (waited < max_ms) {
        if (gx_stop_requested(d))
            return -4;              /* cancelled: stop waiting for the re-enumeration */
        usleep(200000);
        waited += 200;
        if (gx_transport_open(d) == 0) {
            LOG("device re-enumerated after %d ms, reopened", waited);
            return 0;
        }
    }
    LOG("device did not re-enumerate within %d ms", max_ms);
    return -1;
}

int gx_device_init(struct goodix_dev *d)
{
    int r;

    LOG("=== device init %04x:%04x ===", d->vid, d->pid);

    for (int try = 0; try < RETRY_COUNT; try++) {
        if (gx_stop_requested(d)) {
            LOG("device init cancelled");
            return -4;
        }
        /* A failed reopen during evk wake-up L3 closes the transport
         * (usb_devh=NULL); reopen before the next retry instead of carrying
         * on with a null handle (see the NULL guard in transport.c; this is
         * the active recovery). */
        if (!d->usb_devh && gx_transport_open(d) != 0) {
            LOG("device gone, cannot reopen transport");
            return -1;
        }
        r = device_init(d);
        if (r == 0)
            break;
        if (r == FW_SOFT_RESET) {
            /* After a firmware update the MCU restarts and the reference
             * implementation stops init. The USB device will re-enumerate
             * and must be reopened. */
            LOG("soft reset marker, waiting for MCU reboot + re-enum...");
            usleep(1000000);
            if (gx_transport_reopen(d, 8000) < 0) {
                LOG("Init Device Failed (no device after MCU reset)");
                return -1;
            }
            /* device state is fresh after re-open */
            memset(d->evk, 0, sizeof(d->evk));
            d->mcu_inited = false;
            d->fp_inited = false;
            d->besd = 0;
            continue;
        }
        LOG("device init try %d failed (%d)", try + 1, r);
        usleep(300000);
    }
    if (r != 0) {
        LOG("Init Device Failed");
        return r;
    }

    LOG("Init Device Success");

    /* --- TLS handshake (only when there is a PSK) ---
     * gx_tls_server_init: sets up the OpenSSL context
     * gx_tls_handshake:   server handshake (0xD1) -> OpenSSL handshake ->
     *                     handshake command (0xD4) -> read MCU state bit1
     * The reference implementation retries the whole sequence. Here it runs
     * once; on failure the caller may call gx_device_init again. */
    if (d->have_psk && !getenv("GOODIX_NO_TLS")) {
        r = gx_tls_server_init(d);
        if (r == 0) {
            r = gx_tls_handshake(d);
        } else {
            LOG("TLS server init failed (%d)", r);
        }
    } else if (d->have_psk) {
        LOG("TLS skipped (GOODIX_NO_TLS) - testing plaintext capture");
    }

    /* --- "Init: update all base..." (after the TLS handshake) ---
     * Without a baseline file the reference implementation updates every
     * baseline: FDT-manual (0x36) makes the MCU sample the no-finger FDT
     * baseline on the spot, then one no-finger image baseline is captured by
     * direct exposure, and both are saved to goodix.dat. Without a real
     * baseline table FDT-down arming reports no finger events, and without
     * an image baseline host-side finger detection cannot work, so these two
     * steps are a prerequisite of the first capture. (There must be no
     * finger on the sensor at this point - the reference implementation
     * assumes the same.) */
    if (d->fp_inited && !d->base_valid) {
        LOG("no FDT base, sampling via FDT-manual");
        if (gx_fdt_sample_base(d) == 0) {
            gx_base_save(d);
        } else {
            LOG("FDT base sampling failed - capture falls back to "
                "default-config arm");
        }
    }
    if (d->fp_inited && !d->img_base_valid) {
        LOG("no image base, capturing no-finger frame");
        if (gx_capture_base_image(d) == 0) {
            gx_base_save(d);
        } else {
            LOG("image base capture failed - finger check disabled");
        }
    }

    LOG("=== device init complete ===");
    return 0;
}
