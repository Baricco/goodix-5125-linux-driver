/* goodix_fwupdate.c - STM32 MCU firmware download
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * (project 8 = ST32 3626, 27c6:5125, ST411SEC).
 *
 * Firmware data (gf_fw embedded in include/goodix_fw.h =
 * firmware/st411sec_app.bin raw image, 128404 bytes, starting with the ARM
 * vector table):
 *   gf_fw[]        = firmware image (128404 bytes, sent as is, no version prefix)
 *   GF_FW_VERSION  = "GF_ST411SEC_APP_12508" (only for the version comparison)
 *   GF_FW_CRC      = CRC-32/MPEG-2 of the image (0x6FBC0F40, checked at run time)
 *
 * Firmware update flow:
 *   1. MCU not in IAP and same version      -> no update needed (returns 1)
 *   2. MCU not in IAP and different version -> clear APP (0xA4)
 *      + hard reset + reread the EVK version -> returns -2097165 (soft reset)
 *   3. MCU in IAP -> transfer the firmware (gf_fw, 128404):
 *      - 1008-byte chunks, command 0xF0 {off32,len32,data}, evt5
 *      - end command 0xF4 {0,0,len32,crc32}, evt5 (the MCU checks the CRC)
 *      - the CRC32 is non-reflected (init 0xFFFFFFFF, poly 0x04C11DB7,
 *        MSB first, no final XOR) = 0x6FBC0F40 for this firmware
 *   4. transfer succeeded -> hard reset -> returns -2097165
 *
 * The init phase propagates -2097165 upwards as a "soft reset, stop
 * initialization" marker (4292870131 = 0xFFDFFFF3); the rest of the flow
 * stops and waits for the next run.
 */
#include <string.h>
#include <unistd.h>
#include "goodix.h"

/* The vendor firmware image is not shipped with distribution packages
 * (copyright Goodix, not covered by the open source license). With
 * goodix_fw.h the full flashing path is kept; without it only the version is
 * compared and the APP area is never erased or flashed (erasing without an
 * image = brick). */
#if __has_include("goodix_fw.h")
#include "goodix_fw.h"
#define GX_HAVE_BUNDLED_FW 1
#else
#define GF_FW_VERSION "GF_ST411SEC_APP_12508"
#define GX_HAVE_BUNDLED_FW 0
#endif

#if GX_HAVE_BUNDLED_FW
#define FW_SLICE 1008

/* ---- send one chunk: 0xF0 {off32le, len32le, data} ---- */
static int fw_send_slice(struct goodix_dev *d, const uint8_t *fw,
                         uint32_t offset, uint32_t len)
{
    uint8_t payload[8 + FW_SLICE];
    uint8_t rsp[16];
    uint16_t rl = sizeof(rsp);
    int r;

    memcpy(payload, &offset, 4);
    memcpy(payload + 4, &len, 4);
    memcpy(payload + 8, fw + offset, len);
    r = gx_send_cmd_wait(d, GF_CMD_FW_SLICE, payload, (uint16_t)(8 + len), rsp, &rl, 2000);
    if (r < 0)
        LOG("slice @0x%x (%u B) failed: %d", offset, len, r);
    return r;
}

/* ---- end: 0xF4 {0, 0, len32le, crc32le} -> MCU check ----
 * Then the first data byte of the reply is checked: non-zero = the CRC+len
 * check passed. The status byte is returned through *status (0 = rejected
 * by the MCU). */
static int fw_finish(struct goodix_dev *d, uint32_t len, uint32_t crc,
                     uint8_t *status)
{
    uint8_t payload[12] = { 0 };
    uint8_t rsp[16];
    uint16_t rl = sizeof(rsp);
    int r;

    memcpy(payload + 4, &len, 4);
    memcpy(payload + 8, &crc, 4);
    r = gx_send_cmd_wait(d, GF_CMD_FW_FINISH, payload, 12, rsp, &rl, 2000);
    if (r < 0) {
        LOG("firmware finish cmd failed: %d", r);
        return r;
    }
    if (status)
        *status = (rl > 0) ? rsp[0] : 0;
    LOG("firmware finish response: data[0]=0x%02x", status ? *status : 0);
    return r;
}

/* Core of the firmware transfer: chunks + CRC end command.
 * On success the MCU is hard reset; returns the FW_SOFT_RESET marker. */
static int fw_update_core(struct goodix_dev *d, const uint8_t *fw, uint32_t total)
{
    uint32_t frames = (total + FW_SLICE - 1) / FW_SLICE;
    uint32_t crc;
    int r;

    LOG("firmware download: %u bytes, %u frames", total, frames);

    /* Integrity pre-check of the embedded firmware: if GF_FW_CRC does not match
     * the CRC-32/MPEG-2 computed at run time (damaged image / broken
     * generation script) it refuses right away, so a bad firmware never
     * reaches the MCU. */
    crc = gx_crc32_mpeg2(fw, total, 0xFFFFFFFFu);
    if (crc != GF_FW_CRC) {
        LOG("embedded firmware CRC mismatch (calc 0x%08X, expected 0x%08X) - "
            "refusing to flash", crc, GF_FW_CRC);
        return -6;
    }

    for (uint32_t i = 0; i < frames; i++) {
        uint32_t off = i * FW_SLICE;
        uint32_t len = (total - off > FW_SLICE) ? FW_SLICE : (total - off);
        r = fw_send_slice(d, fw, off, len);
        if (r < 0)
            return r;
    }

    crc = gx_crc32_mpeg2(fw, total, 0xFFFFFFFFu);
    LOG("firmware CRC32: 0x%08X (non-reflected)", crc);

    uint8_t status = 0;
    r = fw_finish(d, total, crc, &status);
    if (r < 0)
        return r;

    /* The MCU must have verified CRC+len (reply data[0] != 0).
     * If the check did not pass -> "check firmware failed", returns 0. */
    if (status == 0) {
        LOG("MCU rejected firmware (CRC/LEN check failed, data[0]=0x00)");
        return 0;
    }
    LOG("firmware verified by MCU (data[0]=0x%02x)", status);

    /* Hard reset the MCU to run the APP: 0xA2 {2,20}, send without waiting.
     * Otherwise the MCU stays in IAP and keeps reporting IAP. */
    LOG("firmware OK, hard-reset MCU to APP (0xA2 {2,20})");
    uint8_t reset[2] = { 2, 20 };
    gx_send_cmd(d, 0xA2, reset, 2, 100);
    usleep(1000000);   /* MCU flash-verify + reboot into APP */
    LOG("firmware download complete, MCU reset to APP");
    return FW_SOFT_RESET;
}

/* Clear APP (cmd0=10 cmd1=2 {0,0}, ackt=1, tmo=500, evt=-1)
 * erases the APP area; the MCU then restarts into IAP. */
static int fw_clear_app(struct goodix_dev *d)
{
    uint8_t data[2] = { 0, 0 };
    uint8_t rsp[4]; uint16_t rl = sizeof(rsp);
    return gx_send_cmd_wait(d, GF_CMD_CLEAR_APP, data, 2, rsp, &rl, 500);
}

#endif /* GX_HAVE_BUNDLED_FW */

/* Main firmware update flow.
 * now_version: the 64 B EVK version string.
 * Returns 1 = no update needed; FW_SOFT_RESET = updated and reset;
 * 0 = failure. */
static int fw_update_master(struct goodix_dev *d, uint8_t now_version[64])
{
    char ver[65];
    memcpy(ver, now_version, 64);
    ver[64] = 0;

    /* 1. compare the version when the MCU is not in IAP */
    if (!strstr(ver, "IAP")) {
        LOG("MCU not in IAP (%.32s), compare version", ver);
        if (strncmp(ver, GF_FW_VERSION, strlen(GF_FW_VERSION)) == 0) {
            LOG("same version, no firmware update needed");
            return 1;
        }
#if !GX_HAVE_BUNDLED_FW
        LOG("firmware %.32s differs from the tested %s; this build bundles "
            "no firmware and never flashes - continuing with the installed one",
            ver, GF_FW_VERSION);
        return 1;
#else
        LOG("version differs, Clear App first");
        /* 0xA4 clears the APP area; gx_send_cmd_wait returns 0 on success (the
         * "r > 0" branch here used to be unreachable dead code - after the
         * clear the MCU restarts into IAP, and the path below handles it:
         * "reread the version -> download if in IAP"). */
        fw_clear_app(d);
        usleep(200000);
        memset(now_version, 0, 64);
        gx_dev_evk(d, now_version);
        memcpy(ver, now_version, 64);
        ver[64] = 0;
#endif
    }

    /* 2. now it must be in IAP */
    if (strstr(ver, "IAP")) {
#if GX_HAVE_BUNDLED_FW
        LOG("MCU in IAP, downloading firmware");
        return fw_update_core(d, gf_fw, GF_FW_LEN);
#else
        LOG("MCU is in IAP (no application firmware) and this build bundles "
            "no firmware: restore it with the vendor Windows driver");
        return 0;
#endif
    }

    LOG("Fail to clear APP, MCU still not in IAP");
    return 0;
}

/* Public entry point: runs the complete firmware update flow.
 * Returns 0 on success/no update needed, the FW_SOFT_RESET marker, or < 0 on
 * error. */
int gx_fw_update(struct goodix_dev *d, uint8_t evk[64])
{
    return fw_update_master(d, evk);
}
