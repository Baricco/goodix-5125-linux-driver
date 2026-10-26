/* goodix_cmd.c - command layer (command bytes and their functions)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Command byte = (cmd0<<4)|(cmd1<<1):
 *   ResetSensor    0xA2  cmd0=10 cmd1=1  data={1,20}   [reset MCU and fingerprint module]
 *   GetEvkVersion  0xA8  cmd0=10 cmd1=4  data={0,0}    [read the EVK version]
 *   GetMcuState    0xAF  cmd0=10 cmd1=7  data={0x55,ts16,0,0} [read the MCU state, more=1]
 *   ChipRegRead    0x82  cmd0=8  cmd1=1  data={0,reg16,len16} [read chip registers]
 *   SetMode idle   0x20  cmd0=2  cmd1=0                 [set idle mode]
 *   SetMode capture 0x36 cmd0=3  cmd1=3                 [set capture mode]
 *   TLS init       0xD1  cmd0=13 cmd1=0  data={0,0}    [TLS server init, more=1]
 *   TLS handshake  0xD4  cmd0=13 cmd1=2  data={0,0}    [TLS handshake]
 *   MCU read       0xE4  cmd0=14 cmd1=2  data={type32,0}[MCU production read]
 *   MCU write      0xE0  cmd0=14 cmd1=0                 [MCU production write]
 *   ClearApp       0xA4  cmd0=10 cmd1=2  data={0,0}    [erase the APP area]
 *   ReadOtpData    0xA6  cmd0=10 cmd1=3  data={0,0}    [read OTP data]
 *   DownloadConfig 0x90  cmd0=9  cmd1=0  data=cfg(224B)[download configuration]
 *   FW slice       0xF0  cmd0=15 cmd1=0  {off32,len32,data} [firmware chunk transfer]
 *   FW finish      0xF4  cmd0=15 cmd1=2  {0,0,len32,crc32} [end of firmware transfer]
 */
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include "goodix.h"

/* sends a plaintext command */
int gx_send_cmd(struct goodix_dev *d, uint8_t cmd,
                const uint8_t *data, uint16_t len, int tmo)
{
    (void)tmo;
    return gx_send_cmd_frame(d, cmd, data, len);
}

/* Sends a command and waits for the matching data response frame.
 *
 * Protocol exchange:
 *
 *   1. Send the plaintext frame (command byte, possibly with the more bit).
 *   2. The device replies with an ACK frame (command byte 0xB0, data[0] =
 *      original command, data[1] = status bits: 0x02 config lost, 0x04 TLS
 *      lost), consumed and skipped here.
 *   3. The device then replies with the DATA response frame, whose command
 *      byte must equal the requested command (more bit cleared). The frame
 *      triggers each command's event
 *      (read EVK version: cmd0=10 cmd1=4 -> event 9;
 *       read MCU state/reset sensor: cmd0=10 cmd1=1/7 -> event 6;
 *       register read/write: cmd0=8 -> event 2).
 *   4. Response payload[1:2] is the data length, including the trailing
 *      checksum byte.
 *
 * Returns 0 on success with the data copied to rsp (length in *rsp_len,
 * checksum byte removed), -3 on timeout.
 */
int gx_send_cmd_wait(struct goodix_dev *d, uint8_t cmd,
                     const uint8_t *data, uint16_t len,
                     uint8_t *rsp, uint16_t *rsp_len, int tmo)
{
    uint8_t frame[MAX_FRAME];
    uint16_t flen = 0;
    int r;
    int deadline = tmo > 0 ? tmo : 1500;
    uint8_t want = (uint8_t)(cmd & 0xFE);   /* expected response command, more bit cleared */

    r = gx_send_cmd(d, cmd, data, len, tmo);
    if (r < 0)
        return r;

    while (deadline > 0) {
        r = gx_read_frame(d, frame, &flen, 200);
        if (r < 0) {
            deadline -= 200;
            continue;
        }
        if (flen < 7)
            continue;
        uint8_t cb = frame[GF_CMD_BYTE];
        uint8_t c0 = cb >> 4;
        if ((cb & 1) != 0)       /* more bit: not the final frame */
            continue;
        if (c0 == 0xB || c0 == 0xC)
            continue;            /* ACK (0xB0) / notification (0xC0): skip and keep waiting */
        /* Production commands (cmd0=0xE): the response carries the same cmd0 but
         * a different cmd1 (read 0xE4 -> 0xE4/0xE6, write 0xE0 -> 0xE2). Match
         * on cmd0 only. */
        if ((want >> 4) == 0xE) {
            if (c0 != 0xE)
                continue;
        } else if (cb != want) {
            continue;            /* responses of the other commands */
        }

        uint16_t dlen = (uint16_t)(frame[GF_DATA_LEN] | (frame[GF_DATA_LEN + 1] << 8));
        if (dlen < 1)
            continue;
        if (dlen > 1)            /* drop the trailing checksum byte */
            dlen--;
        if (dlen > flen - GF_DATA)
            dlen = (uint16_t)(flen - GF_DATA);
        if (rsp && rsp_len && *rsp_len >= dlen)
            memcpy(rsp, &frame[GF_DATA], dlen);
        if (rsp_len)
            *rsp_len = dlen;
        return 0;
    }
    return -3;
}

/* MCU production read: 0xE4, data={data_type(4B),0}
 * Response: rsp[0]=status (0=ok), data from rsp[1] on. */
int gx_mcu_read(struct goodix_dev *d, uint32_t data_type, uint8_t *out, uint16_t *len)
{
    uint8_t data[8] = { 0 };
    uint8_t rsp[1024];
    uint16_t rl = sizeof(rsp);
    int r;
    /* Production read: send (dev, 14, 2, data, 8, 1, 500, 1000, 7), retried
     * once on failure. Reply data[0] is the MCU execution code:
     * 0 = ok, 1 = no key, anything else = error. */
    memcpy(data, &data_type, 4);
    for (int try_i = 0; try_i < 2; try_i++) {
        rl = sizeof(rsp);
        r = gx_send_cmd_wait(d, GF_CMD_MCU_READ, data, 8, rsp, &rl, 1500);
        if (r < 0) {
            LOG("gx_mcu_read try %d failed (%d)", try_i + 1, r);
            usleep(200000);
            continue;
        }
        if (rl < 1) {
            LOG("gx_mcu_read empty response");
            usleep(200000);
            continue;
        }
        if (rsp[0] != 0) {
            LOG("gx_mcu_read(0x%08X): MCU status 0x%02x (1=no data)",
                data_type, rsp[0]);
            return -5;
        }
        if (rl > 1) {
            uint16_t n = (uint16_t)(rl - 1);
            if (n > *len)
                n = *len;
            memcpy(out, &rsp[1], n);
            *len = n;
        } else {
            *len = 0;
        }
        return 0;
    }
    return r;
}

/* MCU production write: 0xE0, the response status must be 0 */
int gx_mcu_write(struct goodix_dev *d, const uint8_t *data, uint16_t len)
{
    uint8_t rsp[8];
    uint16_t rl = sizeof(rsp);
    int r;
    /* Production write: send (dev, 14, 0, data, len, 1, 500, 1000, 7):
     *   cmd0=14 cmd1=0 (0xE0), ack_tmo=500, data_tmo=1000, evt=7,
     * retried once if the first write fails. A 192-byte blob can take the
     * MCU more than 1 second to digest, hence the longer timeout and the
     * retry. */
    for (int try_i = 0; try_i < 2; try_i++) {
        r = gx_send_cmd_wait(d, GF_CMD_MCU_WRITE, data, len, rsp, &rl, 3000);
        if (r == 0) {
            if (rl < 1 || rsp[0] != 0) {
                LOG("MCU rejected PSK write (status 0x%02x)", rl >= 1 ? rsp[0] : 0);
                return -5;
            }
            return 0;
        }
        LOG("gx_mcu_write try %d failed (%d), retrying", try_i + 1, r);
        usleep(200000);
    }
    return r;
}

/* Set driver state: NOP + 0x97 {state, 2}
 * cmd0=9 cmd1=3, more=1, type 2 = USB. The checksum uses 0x96.
 * Sent after reading the EVK version.
 * Frame: A0 06 00 A6 97 03 00 01 02 0E */
int gx_set_driver_state(struct goodix_dev *d, uint8_t state)
{
    uint8_t data[2] = { state, 2 };
    gx_send_nop(d);
    usleep(10000);
    return gx_send_cmd_wait(d, GF_CMD_SET_DRIVER_STATE, data, 2, NULL, NULL, 200);
}

/* Reset sensor: 0xA2, data={1,20} (resets the MCU and the fingerprint module) */
int gx_dev_reset(struct goodix_dev *d)
{
    uint8_t data[2] = { 1, 20 };
    uint8_t rsp[8];
    uint16_t rl = sizeof(rsp);
    return gx_send_cmd_wait(d, GF_CMD_RESET_SENSOR, data, 2, rsp, &rl, 1000);
}

/* Read EVK version: first a NOP, then 0xA8 {0,0} -> 64 B */
int gx_dev_evk(struct goodix_dev *d, uint8_t out[64])
{
    uint8_t data[2] = { 0, 0 };
    uint16_t rl = 64;
    int r = gx_send_nop(d);          /* NOP for synchronization first (pcap packet 17) */
    if (r < 0)
        return r;
    usleep(10000);                    /* 10 ms delay */
    return gx_send_cmd_wait(d, GF_CMD_EVK_VERSION, data, 2, out, &rl, 500);
}

/* Read MCU state: 0xAF (more=1, no ACK) {0x55, ts16le, 0, 0} -> 16 B
 * Capture packet 23: A0 09 00 A9 AF 06 00 55 [ts16] 00 00 [cks]
 * The checksum uses 0xAE.
 * ts16 = local time: milliseconds + 1000*seconds (low 16 bits), filled in on
 * every call. The MCU uses it for POV timing: it must be the real local time,
 * not zero. */
int gx_get_mcu_state(struct goodix_dev *d, uint8_t out[16])
{
    uint8_t data[5] = { 0x55, 0, 0, 0, 0 };
    uint16_t rl = 16;
    struct timeval tv;
    struct tm tmv;
    if (gettimeofday(&tv, NULL) == 0 && localtime_r(&tv.tv_sec, &tmv)) {
        uint16_t ts = (uint16_t)(tmv.tm_sec * 1000 + tv.tv_usec / 1000);
        data[1] = (uint8_t)ts;
        data[2] = (uint8_t)(ts >> 8);
    }
    return gx_send_cmd_wait(d, GF_CMD_MCU_STATE, data, 5, out, &rl, 500);
}

/* TLS server init: 0xD1 {0,0} - send only, no wait (event=-1, ack_tmo=0).
 * Returns as soon as the frame is accepted; the actual TLS handshake state is
 * tracked by OpenSSL + reading the MCU state. */
int gx_tls_server_cmd(struct goodix_dev *d)
{
    uint8_t z[2] = { 0, 0 };
    return gx_send_cmd(d, GF_CMD_TLS_SERVER_INIT, z, 2, 200);
}

/* TLS handshake command: 0xD4 {0,0} (event=-1). Returns right after sending;
 * completion of the handshake is confirmed by bit1 of the MCU state. */
int gx_tls_handshake_cmd(struct goodix_dev *d)
{
    uint8_t z[2] = { 0, 0 };
    return gx_send_cmd(d, GF_CMD_TLS_HANDSHAKE, z, 2, 200);
}
