/* goodix_frame.c - frame layer (building and parsing plaintext command frames,
 * TLS frames and response frames)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * == Plaintext command frame ==
 *   [0]    = 0xA0 (type; the low nibble keeps the previous value, 0 the first time)
 *   [1:2]  = payload length LE = datalen + 4
 *            (payload = cmd[1] + len[2] + data[datalen] + cks[1])
 *   [3]    = header checksum: (b0 + b1 + b2) & 0xFF
 *   [4]    = command byte: (cmd0<<4)|(cmd1<<1)  [|1 only when not waiting for an ACK]
 *   [5:6]  = datalen + 1 (LE)
 *   [7..]  = data
 *   [last] = data checksum: 0xAA - (sum(data) + (dlen+1) + ((dlen+1)>>8) + cmd)
 *   total  = datalen + 8
 *
 * == TLS frame (type 0xB) ==
 *   [0]    = 0xB0
 *   [1:2]  = TLS record length (LE)   <- payload = raw TLS record
 *   [3]    = header checksum
 *   [4..]  = TLS record (no trailing checksum at frame level: the payload goes
 *            straight to the TLS layer)
 *
 * == Response frame ==
 *   [0]    = 0xA0|0xB0|0xC0 (type, high nibble 0xA/0xB/0xC)
 *   [1:2]  = payload length LE; total = payload + 4
 *   [3]    = header checksum
 *   [4]    = response command byte (type<<4|sub<<1|more)
 *   [5:6]  = data length LE
 *   [7..]  = data (+ trailing checksum or 0x88)
 *
 * Transport: sent in 64-byte chunks, each padded to 64 bytes.
 */
#include <stdlib.h>
#include <string.h>
#include "goodix.h"

static inline uint8_t hdr_cks(uint8_t a, uint8_t b, uint8_t c)
{ return (uint8_t)(a + b + c); }

/* Data checksum: 0xAA - (sum(data) + (len+1) + ((len+1)>>8) + cmd)
 * The checksum uses the command byte without the more bit, so
 * cks_cmd = cmd & 0xFE. */
static inline uint8_t data_cks(uint8_t cmd, const uint8_t *data, uint16_t len)
{
    uint8_t s = (uint8_t)((cmd & 0xFE) + (len + 1) + ((len + 1) >> 8));
    for (uint16_t i = 0; i < len; i++)
        s += data[i];
    return (uint8_t)(0xAA - s);
}

/* Sends the buffer in zero-padded 64 B chunks */
static int usb_send_chunks(struct goodix_dev *d, const uint8_t *frame, uint16_t total)
{
    uint16_t off = 0;
    while (off < total) {
        uint8_t chunk[64];
        uint16_t n = total - off;
        if (n > 64)
            n = 64;
        memset(chunk, 0, sizeof(chunk));
        memcpy(chunk, &frame[off], n);
        if (gx_usb_write(d, chunk, 64) != 64)
            return -2;
        off = (uint16_t)(off + n);
    }
    return 0;
}

/* Sends a plaintext command frame */
int gx_send_cmd_frame(struct goodix_dev *d, uint8_t cmd,
                      const uint8_t *data, uint16_t datalen)
{
    uint8_t *frame;
    uint16_t total = datalen + 8;        /* hdr4 + payload(datalen+4) */
    uint16_t paylen = datalen + 4;       /* cmd+len+data+cks */
    int r;

    if (total > MAX_FRAME)
        return -1;
    frame = malloc(total);
    if (!frame)
        return -1;

    frame[0] = GF_TYPE_PLAIN;
    frame[1] = (uint8_t)paylen;
    frame[2] = (uint8_t)(paylen >> 8);
    frame[3] = hdr_cks(frame[0], frame[1], frame[2]);
    frame[4] = cmd;
    frame[5] = (uint8_t)(datalen + 1);
    frame[6] = (uint8_t)((datalen + 1) >> 8);
    if (datalen)
        memcpy(&frame[7], data, datalen);
    frame[7 + datalen] = data_cks(cmd, data, datalen);

    r = usb_send_chunks(d, frame, total);
    free(frame);
    return r;
}

/* Sends a TLS frame: [0xB0][rlen][cks][TLS record] (no trailing checksum) */
int gx_send_tls_frame(struct goodix_dev *d, const uint8_t *record, uint16_t rlen)
{
    uint8_t *frame;
    uint16_t total = 4 + rlen;
    int r;

    if (total > MAX_FRAME)
        return -1;
    frame = malloc(total);
    if (!frame)
        return -1;

    frame[0] = GF_TYPE_TLS;
    frame[1] = (uint8_t)rlen;
    frame[2] = (uint8_t)(rlen >> 8);
    frame[3] = hdr_cks(frame[0], frame[1], frame[2]);
    memcpy(&frame[4], record, rlen);

    r = usb_send_chunks(d, frame, total);
    free(frame);
    return r;
}

/* Reads one frame (A0/B0/C0), reassembling the 64 B chunks.
 * Header [1:2] = payload length; total = payload + 4.
 * The caller's buffer must be >= MAX_FRAME. */
int gx_read_frame(struct goodix_dev *d, uint8_t *frame, uint16_t *len, int tmo)
{
    uint8_t buf[64];
    uint16_t total = 0, paylen = 0;
    int r;
    int deadline = tmo > 0 ? tmo : 2000;

    while (deadline > 0) {
        r = gx_usb_read(d, buf, 64, 100);
        if (r < 0) {
            deadline -= 100;
            continue;
        }
        /* The non-progress path consumes the budget too: every read blocks for
         * up to 100 ms. If only the error path were charged, noise frames or
         * half packets would make the real timeout of this function unbounded
         * (possibly far beyond tmo). */
        if (r < 4) {
            deadline -= 100;
            continue;
        }
        if ((buf[0] >> 4) != 0xA && (buf[0] >> 4) != 0xB && (buf[0] >> 4) != 0xC) {
            deadline -= 100;
            continue;
        }
        if (buf[3] != hdr_cks(buf[0], buf[1], buf[2])) {
            deadline -= 100;
            continue;
        }
        paylen = (uint16_t)(buf[1] | (buf[2] << 8));
        if (paylen < 1 || paylen > MAX_FRAME - 4) {
            deadline -= 100;
            continue;
        }
        total = (uint16_t)(paylen + 4);
        memcpy(frame, buf, r < 64 ? (size_t)r : 64);
        if (r >= total) {
            *len = total;
            return 0;
        }
        /* read remaining chunks */
        uint16_t want = (uint16_t)(total - r);
        while (want > 0) {
            r = gx_usb_read(d, buf, 64, 100);
            if (r < 0) {
                deadline -= 100;
                if (deadline <= 0)
                    return -1;
                continue;
            }
            if (r > want)
                r = want;
            memcpy(&frame[total - want], buf, (size_t)r);
            want = (uint16_t)(want - r);
        }
        *len = total;
        return 0;
    }
    return -1;
}

/* Sends the NOP command: send (dev, 0, 0, {0,0,0,0}, 4, 0, 0)
 *   cmd0=0 cmd1=0, a7=0 -> more=1, a6=0 -> no checksum (0x88).
 *   Frame: A0 08 00 A8 01 05 00 00 00 00 00 88  (verified on pcap packet 17)
 * Sent before reading the EVK version / setting the driver state / reading
 * the MCU state.
 */
int gx_send_nop(struct goodix_dev *d)
{
    uint8_t frame[12];
    frame[0]  = 0xA0;
    frame[1]  = 8;            /* payload length datalen+4 */
    frame[2]  = 0;
    frame[3]  = (uint8_t)(0xA0 + 8);   /* header checksum */
    frame[4]  = 0x01;         /* command: more=1 */
    frame[5]  = 5;            /* data length datalen+1 */
    frame[6]  = 0;
    frame[7]  = 0;            /* 4 zero data bytes */
    frame[8]  = 0;
    frame[9]  = 0;
    frame[10] = 0;
    frame[11] = 0x88;         /* no-checksum marker: trailing 0x88 */
    return usb_send_chunks(d, frame, sizeof(frame));
}
