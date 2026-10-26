/* goodix_capture.c - image capture (POV wake-up path + finger-detection
 * image path + baseline sampling).
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Architecture notes (checked against the reference implementation's
 * behaviour + interaction with the ST411SEC firmware):
 *   - this unit's chipid 0x2504 -> sensor type 12 (ChicagoH); SetMode is a
 *     real command (the early conclusion "ST32 ops are all dummies" applies
 *     only to the ST32/MilanG branch).
 *   - capture trigger: send the FDT-down set command
 *     (3,1,1) "Fdt down" -> cmd0=3 cmd1=1 (0x32),
 *     always a3=1: data=[8,1,fdt_down_base(12B),ts16], 16 bytes in total.
 *     (The 4-byte a3=0 form [8,0,tbl0,tbl1] is only the fallback of the
 *     baseline presence check, when baseline sampling failed and there is
 *     no baseline at all.)
 *   - baseline source: init-time baseline sampling sends
 *     FDT-manual (cmd0=3 cmd1=3 = 0x36, data=[9,1,12B table]); the MCU
 *     samples the no-finger baseline on the spot and pushes a cmd0=3
 *     IrqStatus=0x100 frame, which the host learns as the down table
 *     (down table: ((raw>>1)<<8)|0x80). Later, when a finger touches, the
 *     MCU first pushes a cmd0=3 FDT event frame (IrqStatus=2 finger-down,
 *     also used to learn the up table), then a cmd0=2 image frame.
 *   - POV path: when MCU state bit0 (isPOVImageValid)=1, send 0xD2 {0,0}
 *     (POV wake-up command) to fetch the image cached during the
 *     power-saving wake-up.
 *   - channel (USB data frames, state>=16): once TLS is active, A0 plaintext
 *     frames are still dispatched directly, and B0 frames are decrypted by
 *     OpenSSL and dispatched with the same payload format - FDT/image frames
 *     can show up on either side, so both must be received together.
 *
 * Device data payload (== USB frame without its 4-byte header):
 *   payload[0]   = cmd byte (cmd0<<4|cmd1<<1|more)
 *   payload[1:2] = data length v19 (LE), data from [3] on
 *   payload[3..] = data[0..] : data[0]=0xAA (POV) / not 0xAA (image)
 *                  image pixels = data[5 ..], length v19-6 (== img_size)
 *   last byte    = checksum (0x88 => skip the check, otherwise sum == 0xAA)
 *
 * An image frame is recognized by cmd0==2 (device data) only, cmd1/more are
 * not checked.
 */
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include "goodix.h"

/* CRC check: the sum of the data bytes must equal 0xAA */
static int verify_crc(const uint8_t *data, uint16_t len)
{
    uint8_t s = 0;
    for (uint16_t i = 0; i < len; i++)
        s += data[i];
    return s == 0xAA ? 0 : -1;
}

/* ---- on-wire image CRC-32 (CRC-32 of the image data) ----
 * The same CRC-32/MPEG-2 as goodix.dat (poly 0x04C11DB7, initial value
 * FFFFFFFF, no final XOR), table implementation in goodix_crc.c. The last 4
 * bytes of the image data are the CRC, stored with each 16-bit half-word big
 * endian and the half-words little endian: crc = b1|b0<<8 | b3<<16|b2<<24. */

/* ---- 12-bit unpacking + transposition (column-major pixel unpacking) ----
 * On the wire 6 bytes pack 4 12-bit pixels; the pixel stream is sent column
 * by column (64 px per column) and is transposed while unpacking into a
 * row-major 80x64 image, 16 bits per pixel (value in the low 12 bits).
 * in: 7680B packed; out: 10240B (80*64*2). */
static void unpack_12bit_transpose(const uint8_t *in, uint8_t *out,
                                   unsigned int packed_len)
{
    uint32_t k = 0;                         /* pixel number in wire order */
    for (unsigned int j = 0; j < packed_len; j += 6) {
        uint16_t p0 = (uint16_t)(in[j + 1] + ((in[j] & 0xF) << 8));
        uint16_t p1 = (uint16_t)((in[j] >> 4) + 16 * in[j + 3]);
        uint16_t p2 = (uint16_t)(in[j + 2] + ((in[j + 5] & 0xF) << 8));
        uint16_t p3 = (uint16_t)((in[j + 5] >> 4) + 16 * in[j + 4]);
        uint16_t px[4] = { p0, p1, p2, p3 };
        for (int t = 0; t < 4; t++, k++) {
            uint32_t dst = k / 64 + 80 * (k % 64);   /* transpose: column -> row */
            out[2 * dst] = (uint8_t)px[t];
            out[2 * dst + 1] = (uint8_t)(px[t] >> 8);
        }
    }
}

/* ---------------- host-side finger detection ----------------
 * The new frame is compared with the no-finger baseline image (d->img_base,
 * the global baseline image) through 8x8 block statistics; the parameters
 * depend on the sensor type (initialized after OTP parsing):
 *   - grid: brows x bcols 8x8 blocks, block origins from the row_off/col_off
 *     tables
 *   - threshold T = tcode*16*delta/tdiv (tcode/delta from the OTP, tdiv per
 *     type)
 *   - need = number of high-variance blocks required for a decision
 * Decision tree (a2 = baseline, a3 = new frame):
 *   v38 (high-variance blocks) >= need -> new frame much brighter and the
 *                                         baseline not brighter ?
 *                                         3(bad) : 1(finger)
 *   any block mean|diff| > 1.4T        -> new block mean 1.4T brighter than
 *                                         the baseline ? 3 : 1
 *   mid-difference blocks (0.6T<d<1.4T) < need -> 2(void trigger)
 *   otherwise                          -> 0(temperature drift, the baseline
 *                                         must be rebuilt) */
struct fdt_disc_params {
    uint8_t need, brows, bcols;
    uint8_t row_off[5], col_off[5];
    uint8_t h, w;
    uint16_t tdiv;
};
static const struct fdt_disc_params *fdt_disc_params(uint8_t type)
{
    static const struct fdt_disc_params tbl[] = {
        /* type 2 MilanG 176x54 (a1=6, tdiv=256) */
        { 6, 2, 5, { 8, 38 }, { 12, 48, 84, 120, 156 }, 54, 176, 256 },
        /* type 3 MilanL 112x132 (a1=7, tdiv=256) */
        { 7, 3, 4, { 8, 52, 96 }, { 8, 44, 80, 116 }, 112, 132, 256 },
        /* type 12 ChicagoHS 80x64 (a1=4, tdiv=128) */
        { 4, 2, 3, { 12, 44 }, { 4, 36, 68 }, 64, 80, 128 },
        /* type 14 ChicagoT 36x160 (a1=7, tdiv=256) */
        { 7, 5, 2, { 12, 44, 76, 108, 140 }, { 4, 24 }, 160, 36, 256 },
    };
    switch (type) {
    case 2:  return &tbl[0];
    case 3:  return &tbl[1];
    case 12: return &tbl[2];
    case 14: return &tbl[3];
    default: return NULL;
    }
}

static inline uint16_t px16(const uint8_t *img, uint32_t idx)
{
    return (uint16_t)(img[2 * idx] | (img[2 * idx + 1] << 8));
}

/* mode-0 path of finger detection (a1+4==0, 8x8 block grid).
 * Parameters: a2 = baseline (base), a3 = new frame (new); in this function
 * img = new, d->img_base = base.
 * Returns 1=finger / 2=void / 3=bad / 0=temperature / -1=cannot decide. */
int gx_fdt_check_finger(struct goodix_dev *d, const uint8_t *img)
{
    const struct fdt_disc_params *p = fdt_disc_params(d->sensor_type);
    if (!p || !d->img_base_valid)
        return -1;
    if (d->img_w != p->w || d->img_h != p->h)
        return -1;

    /* threshold (tcode*16*delta/tdiv, integer arithmetic) */
    uint32_t t = (uint32_t)d->fdt_tcode * 16u * d->fdt_delta / p->tdiv;
    int hi = (int)(t * 1.4);            /* (int)(double(T)*1.4) */
    int lo = (int)(t * 0.6);

    uint8_t v38 = 0, v40 = 0;
    int v39 = 0, v41 = 0;
    for (uint8_t br = 0; br < p->brows; br++) {
        for (uint8_t bc = 0; bc < p->bcols; bc++) {
            uint32_t sum_new = 0, sum_base = 0, sum_diff = 0;
            /* Single pass: pixel differences are cached in di[] (64 pixels,
             * |a-b| <= 4095 fits in a signed 16-bit value), and the second
             * (variance) pass reads the cache instead of rereading both
             * images. The order of operations is exactly that of the original
             * two-pass implementation, and the results are bit-identical. */
            int16_t di[64];
            uint32_t di_n = 0;
            for (uint8_t m = 0; m < 8; m++) {
                for (uint8_t n = 0; n < 8; n++) {
                    uint32_t idx = (uint32_t)p->w * (m + p->row_off[br])
                                   + n + p->col_off[bc];
                    uint16_t a = px16(d->img_base, idx);/* baseline (a2) */
                    uint16_t b = px16(img, idx);        /* new frame (a3) */
                    int16_t diff = (int16_t)((a > b) ? (a - b) : (b - a));
                    sum_base += a;
                    sum_new += b;
                    sum_diff += (uint32_t)diff;
                    di[di_n++] = diff;
                }
            }
            uint32_t mean_base = sum_base / 64;         /* block mean (offset +56) */
            uint32_t mean_new = sum_new / 64;           /* block mean (offset +64) */
            uint32_t mean_diff = sum_diff / 64;         /* block mean (offset +72) */
            /* variance of the in-block differences (+80): Σ(mean_d - d_i)^2 / 63 */
            uint64_t var = 0;
            for (uint32_t k = 0; k < 64; k++) {
                int dd = (int)mean_diff - (int)di[k];
                var += (uint64_t)(dd * dd);
            }
            var /= 63;
            if (var > (uint32_t)hi)
                v38++;
            if ((int)mean_diff > hi)
                v39 = 1;
            if ((int)mean_diff < hi && (int)mean_diff > lo)
                v40++;
            /* v41: new block mean brighter than the baseline by more than hi
             * (+64 > +56 + 1.4T) */
            if (mean_new > mean_base + (uint32_t)hi)
                v41 = 1;
        }
    }

    /* Inner pixel direction statistics (border excluded, inner pixel scan):
     * v53 = pixels where the new frame is 32+ brighter than the baseline
     *       (a3 > a2+32)
     * v54 = pixels where the baseline is 32+ brighter than the new frame
     *       (a2 > a3+32) */
    uint32_t v53 = 0, v54 = 0;
    for (uint16_t r = 1; r < p->h - 1; r++) {
        for (uint16_t c = 1; c < p->w - 1; c++) {
            uint32_t idx = (uint32_t)c + p->w * r;
            uint16_t a = px16(d->img_base, idx);        /* baseline */
            uint16_t b = px16(img, idx);                /* new frame */
            if (b > a + 32)
                v53++;
            if (a > b + 32)
                v54++;
        }
    }
    uint32_t plim = (uint32_t)((p->w * p->h - 2 * (p->w + p->h) + 4) * 0.1);
    int v51 = v53 >= plim;      /* new frame much brighter overall */
    int v52 = v54 >= plim;      /* baseline much brighter overall */

    /* Decision tree:
     *   v38>=need: (!v51 || v52) ? 1 : 3
     *     -- only "new frame much brighter and the baseline not brighter" is
     *        bad, any other structured difference = finger
     *   v39:       v41 ? 3 : 1  -- an abnormally brighter new frame = bad
     *   v40<need:  2(void); otherwise 0(temperature) */
    int ret;
    if (v38 >= p->need)
        ret = (v51 && !v52) ? 3 : 1;
    else if (v39)
        ret = v41 ? 3 : 1;
    else if (v40 < p->need)
        ret = 2;
    else
        ret = 0;
    LOG("capture: finger check ret=%d (%s) [v38=%u v39=%d v40=%u v41=%d "
        "v51=%d v52=%d T=%u]", ret,
        ret == 1 ? "finger down" : ret == 2 ? "void" :
        ret == 3 ? "bad" : "temperature",
        v38, v39, v40, v41, v51, v52, t);
    return ret;
}

/* Parses one device data payload and extracts an image frame.
 * payload[0]=cmd, [1:2]=v19, [3..]=data.
 * (device data): image == cmd0==2, cmd1/more not checked;
 * data[0]==0xAA is a POV frame and is skipped; image data in data[5..],
 * length v19-6.
 * On-wire format of type 12 (image data):
 *   data[5..5+7680) = 12-bit packed pixels (column major), followed by a 4B
 *   CRC-32/MPEG-2; after the CRC check they are unpacked and transposed into
 *   an 80x64x16-bit frame (10240B).
 * Returns 0 + the unpacked 16-bit frame (size in *size). */
static int parse_image_payload(const uint8_t *p, uint16_t plen,
                               uint8_t *img, uint16_t cap, uint16_t *size)
{
    if (plen < 4)
        return -1;
    uint8_t cmd0 = p[0] >> 4;
    uint16_t v19 = (uint16_t)(p[1] | (p[2] << 8));
    if (v19 < 4 || 3 + v19 > plen)
        return -1;
    const uint8_t *data = &p[3];   /* data[0..v19-1] */
    if (cmd0 != 2)
        return -1;                 /* not an image frame (only cmd0==2 is) */
    if (data[0] == 0xAA)
        return -1;                 /* POV frame - not image pixels */
    uint16_t ilen = (uint16_t)(v19 - 6);
    if (ilen < 2)
        return -1;
    /* trailing checksum byte: data[v19-1]; 0x88 => skip, otherwise sum == 0xAA */
    if (data[v19 - 1] != GF_NO_CHECK) {
        if (verify_crc(&data[5], ilen) != 0)
            return -1;
    }
    const uint8_t *w = &data[5];   /* on-wire image: packed pixels + crc32 */
    /* On-wire format: the last 4 bytes are the CRC-32
     * (crc32(payload[0..ilen-4])), stored as big-endian half-words. A failed
     * check only warns and the frame is kept (the reference implementation
     * rejects it and waits again). */
    if (ilen >= 10 && ((ilen - 4) % 6) == 0) {
        uint32_t cw = (uint32_t)w[ilen - 3] | ((uint32_t)w[ilen - 4] << 8) |
                      ((uint32_t)w[ilen - 1] << 16) | ((uint32_t)w[ilen - 2] << 24);
        uint32_t cc = gx_crc32_mpeg2(w, ilen - 4, 0xFFFFFFFFu);
        LOG("capture: image wire CRC32 %s (wire 0x%08x calc 0x%08x)",
            cw == cc ? "OK" : "MISMATCH", cw, cc);
        unsigned int packed = ilen - 4;             /* 7680 */
        unsigned int npix = packed / 6 * 4;         /* 5120 */
        if (npix * 2 <= cap) {
            unpack_12bit_transpose(w, img, packed);
            if (size)
                *size = (uint16_t)(npix * 2);       /* 10240 */
            return 0;
        }
    }
    /* unpacked format (other sensors): delivered as is */
    if (ilen > cap)
        return -1;
    if (img)
        memcpy(img, w, ilen);
    if (size)
        *size = ilen;
    return 0;
}

/* Image preprocessing (16 bit -> 8 bit) lives in goodix_imgproc.c: the
 * parameterized pipeline gx_imgproc_to8bit + SIGFM unsharp-mask enhancement,
 * declared in goodix_imgproc.h (included at the end of goodix.h). */

/* POV (finger present) notification frame: cmd0=13 cmd1=1, data[0]=0xAA
 * (used to report finger events while waiting for POV). */
static int is_pov_frame(const uint8_t *p, uint16_t plen)
{
    if (plen < 4)
        return 0;
    return (p[0] >> 4) == 13 && ((p[0] >> 1) & 0x7) == 1 && p[3] == 0xAA;
}

/* TLS reconnection requested by the device: cmd0=13 cmd1=5 (0xDA).
 * In device data frames it triggers a TLS server reconnection. */
static int is_tls_reconnect_req(const uint8_t *p, uint16_t plen)
{
    if (plen < 1)
        return 0;
    return (p[0] >> 4) == 13 && ((p[0] >> 1) & 0x7) == 5;
}

/* FDT/IRQ push frame: cmd0=3 (device data "--- cmd: fdt").
 * data[0:1] = IrqStatus (2=fdt-down finger touch,
 * 0x100=fdt-manual sample report, 0x200=fdt-up, 0x80/0x82=reverse,
 * 0x800=fp reset).
 * Returns the IrqStatus; -1 for non-FDT frames. */
static int fdt_irq_status(const uint8_t *p, uint16_t plen)
{
    if (plen < 5)
        return -1;
    if ((p[0] >> 4) != 3)
        return -1;
    return p[3] | (p[4] << 8);
}

/* Handles an FDT event frame and learns the baseline on the way:
 * data = payload+3: [IrqStatus 2B][touchflag 2B][raw base 12B].
 * IrqStatus=2     (finger down) -> learn the up table
 * IrqStatus=0x200 (finger up)   -> learn the down table
 * IrqStatus=0x100 (manual)      -> handled by gx_fdt_sample_base itself */
static void handle_fdt_frame(struct goodix_dev *d, const uint8_t *p,
                             uint16_t plen, int irq)
{
    const uint8_t *data = &p[3];
    uint16_t touchflag;
    d->last_fdt_irq = irq;
    LOG("capture: FDT event IrqStatus=0x%x%s", irq,
        irq == 2 ? " (finger down)" :
        irq == 0x200 ? " (finger up)" :
        irq == 0x100 ? " (manual sample)" : "");
    if (plen < 3 + 16)
        return;
    touchflag = (uint16_t)(data[2] | (data[3] << 8));
    if (irq == 2)
        gx_fdt_learn_up_base(d, data + 4, touchflag);
    else if (irq == 0x200)
        gx_fdt_learn_down_base(d, data + 4);
    /* Event callback (libfprint glue: reporting finger down/up).
     * 2/0x80/0x82 = finger down: 0x80/0x82 "reverse" are real touches too and
     * must be reported as PRESENT so libfprint enters the CAPTURE state,
     * otherwise delivering the image fails the state==CAPTURE assertion of
     * fpi_image_device_image_captured (hang).
     * The baseline is not learned from reverse frames (above, the up/down
     * tables are learned only on 2/0x200). */
    if (d->event_cb) {
        if (irq == 2 || irq == 0x80 || irq == 0x82)
            d->event_cb(d, GX_EV_FINGER_DOWN, d->event_cb_user);
        else if (irq == 0x200)
            d->event_cb(d, GX_EV_FINGER_UP, d->event_cb_user);
    }
}

void gx_capture_set_event_cb(struct goodix_dev *d,
                             void (*cb)(struct goodix_dev *d, int event,
                                        void *user),
                             void *user)
{
    d->event_cb = cb;
    d->event_cb_user = user;
}

void gx_capture_cancel(struct goodix_dev *d)
{
    d->capture_stop = 1;
}

/* ---------------- mixed-channel payload reading ----------------
 * Once TLS is active, A0 plaintext frames are dispatched directly to device
 * data processing, and B0 frames are dispatched the same way after ssl_read
 * decryption. A plaintext frame is one whole payload; TLS-decrypted data is a
 * byte stream and must first be collected into a complete payload
 * (cmd+v19+data).
 * The aggregation buffer (d->rx_acc / d->rx_acc_len) is part of the device
 * state (see goodix.h); the capture path accesses it from a single thread,
 * but keeping it in the device makes multiple devices/reentrancy safe. */

/* takes one complete payload from the aggregation buffer; returns 1 if it
 * got one */
static int acc_take(struct goodix_dev *d, uint8_t *out, uint16_t *outlen,
                    uint16_t cap)
{
    if (d->rx_acc_len < 3)
        return 0;
    uint16_t v19 = (uint16_t)(d->rx_acc[1] | (d->rx_acc[2] << 8));
    size_t total = 3 + v19;
    if (v19 < 4 || total > sizeof(d->rx_acc)) {
        LOG("capture: TLS payload desync (v19=%u), resync", v19);
        d->rx_acc_len = 0;
        return 0;
    }
    if (d->rx_acc_len < total)
        return 0;
    if (total <= cap) {
        memcpy(out, d->rx_acc, total);
        *outlen = (uint16_t)total;
    }
    memmove(d->rx_acc, d->rx_acc + total, d->rx_acc_len - total);
    d->rx_acc_len -= total;
    return total <= cap;
}

/* Reads the next device data payload.
 * Plaintext A0/C0 frames: payload = frame[4..flen) returned directly;
 * B0 frames: injected into TLS (gx_tls_feed), the decrypted byte stream goes
 * into the aggregation buffer.
 * Returns 1 = got a payload; 0 = timeout; -1 = TLS channel error. */
static int next_payload(struct goodix_dev *d, uint8_t *out, uint16_t *outlen,
                        uint16_t cap, int tmo_ms)
{
    uint8_t *frame = d->rx_frame;   /* device-level frame reassembly buffer (see goodix.h) */
    int tls = d->tls_inited && d->tls;
    int deadline = tmo_ms > 0 ? tmo_ms : 200;

    while (deadline > 0) {
        if (acc_take(d, out, outlen, cap))
            return 1;
        uint16_t flen = 0;
        int r = gx_read_frame(d, frame, &flen, 200);
        if (r < 0) {
            deadline -= 200;
            continue;
        }
        if (flen < 5)
            continue;
        if (tls && frame[0] == GF_TYPE_TLS) {
            if (gx_tls_feed(d, &frame[4], (size_t)(flen - 4)) < 0)
                return -1;
            for (;;) {
                uint8_t tmp[2048];
                int n = gx_tls_recv(d, tmp, sizeof(tmp), 0);
                if (n <= 0)
                    break;          /* WANT_READ / nothing more for now */
                if (d->rx_acc_len + (size_t)n > sizeof(d->rx_acc)) {
                    d->rx_acc_len = 0; /* overflow: out of sync, reset */
                    break;
                }
                memcpy(d->rx_acc + d->rx_acc_len, tmp, (size_t)n);
                d->rx_acc_len += (size_t)n;
            }
            continue;
        }
        /* plaintext frame */
        if (gx_debug) {
            fprintf(stderr, "[goodix] capture: frame %uB cmd=0x%02x dlen=%u [",
                    flen, frame[4], frame[5] | (frame[6] << 8));
            for (uint16_t i = 7; i < flen && i < 7 + 16; i++)
                fprintf(stderr, "%02X ", frame[i]);
            fprintf(stderr, "]\n");
        }
        if (flen - 4 > cap)
            continue;
        memcpy(out, &frame[4], (size_t)(flen - 4));
        *outlen = (uint16_t)(flen - 4);
        return 1;
    }
    return 0;
}

/* Dispatches one payload (POV/FDT/TLS-reconnect/image).
 * After an FDT finger-down event, as the reference implementation does, it
 * immediately sends SetMode Image (0x20 [1,0]) so the MCU exposes and pushes
 * an image (img_req makes sure it is sent only once).
 * Returns 1 = got an image. */
static int dispatch_payload(struct goodix_dev *d, const uint8_t *pl,
                            uint16_t plen, uint8_t *img, uint16_t *size,
                            int *img_req)
{
    if (is_tls_reconnect_req(pl, plen)) {
        /* device data cmd0==13 cmd1==5 -> trigger a TLS server reconnection */
        LOG("capture: device requested TLS reconnect (0xDA)");
        if (d->tls) {
            gx_tls_capture_mode(d, 0);   /* during the handshake the bio has to read USB itself */
            int r = gx_tls_reconnect(d);
            gx_tls_capture_mode(d, 1);
            d->rx_acc_len = 0;              /* aggregated data of the old session is void */
            if (r < 0)
                return -1;
        }
        return 0;
    }
    if (is_pov_frame(pl, plen)) {
        LOG("capture: POV frame - finger present");
        return 0;
    }
    {
        int irq = fdt_irq_status(pl, plen);
        if (irq >= 0) {
            /* FDT event: 0x2 = finger down, 0x200 = lifted; the image frame is pushed
             * afterwards */
            handle_fdt_frame(d, pl, plen, irq);
            /* FDT down event -> request the image ->
             * SetMode Image (cmd0=2 cmd1=0 = 0x20, data=[1,0]),
             * the MCU then exposes and pushes a cmd0=2 image frame */
            /* irq==2 finger down; 0x80/0x82 "reverse": a baseline offset makes a
             * real touch report with reversed polarity, so an image is
             * requested too (bad frames are filtered downstream by
             * gx_fdt_check_finger + the reject/re-arm loop). Fixes an
             * occasional enrollment hang: finger detect reported 0x80, it was
             * ignored -> no image requested -> "no image frame received". */
            if ((irq == 2 || irq == 0x80 || irq == 0x82) &&
                img_req && !*img_req) {
                uint8_t im[2] = { 1, 0 };
                *img_req = 1;
                LOG("capture: finger touch (irq 0x%x), request image "
                    "(SetMode Image 0x20 [1,0])", irq);
                gx_send_cmd(d, 0x20, im, 2, 200);
            }
            return 0;
        }
    }
    if (parse_image_payload(pl, plen, img, GF_IMG_MAX, size) == 0)
        return 1;
    /* other frames (commands/ACKs) ignored */
    return 0;
}

/* Waits for the device to push a cmd0=2 image frame (mixed channel).
 * cancellable=0: used once SetMode Image has been sent and the image is on
 * its way - giving up halfway would leave half a TLS record in the channel
 * and no later image could be decrypted (libfprint's deactivate between
 * enrollment stages falls exactly in this window). In that case the read is
 * completed (~150 ms, bounded by the timeout) and the caller checks
 * capture_stop afterwards. */
static int wait_image(struct goodix_dev *d, uint8_t *img, uint16_t *size,
                      int timeout_ms, int cancellable)
{
    uint8_t *pl = d->rx_scratch;    /* device-level payload work area */
    int deadline = timeout_ms > 0 ? timeout_ms : 10000;
    int tls = d->tls_inited && d->tls;
    int img_req = 0;

    d->rx_acc_len = 0;
    if (tls)
        gx_tls_capture_mode(d, 1);
    while (deadline > 0) {
        if (cancellable && d->capture_stop) {
            if (tls)
                gx_tls_capture_mode(d, 0);
            return -4;
        }
        uint16_t plen = sizeof(d->rx_scratch);
        int r = next_payload(d, pl, &plen, sizeof(d->rx_scratch), 200);
        if (r < 0) {
            if (tls)
                gx_tls_capture_mode(d, 0);
            return -3;
        }
        if (r == 0) {
            deadline -= 200;
            continue;
        }
        int dr = dispatch_payload(d, pl, plen, img, size, &img_req);
        if (dr < 0) {
            if (tls)
                gx_tls_capture_mode(d, 0);
            return -3;
        }
        if (dr == 1) {
            if (tls)
                gx_tls_capture_mode(d, 0);
            return 0;
        }
    }
    if (tls)
        gx_tls_capture_mode(d, 0);
    return -2;
}

/* Baseline sampling (FDT-manual): SetMode(3,3,1) FDT-manual ->
 * cmd0=3 cmd1=3 (0x36), data=[9, 1, current down table 12B] (all zeros on
 * the first run, like the reference implementation's initial value). The
 * MCU samples the no-finger baseline on the spot and pushes a cmd0=3
 * IrqStatus=0x100 frame: data=[irq 2B][touchflag 2B][raw 12B].
 * It is learned as the down threshold table; on success base_valid=true.
 * The reference implementation does this at init (baseline sampling, after
 * the TLS handshake); 3 retries if nothing arrives or the check fails. */
int gx_fdt_sample_base(struct goodix_dev *d)
{
    uint8_t *pl = d->rx_scratch;    /* device-level payload work area */
    int tls = d->tls_inited && d->tls;

    if (tls)
        gx_tls_capture_mode(d, 1);
    d->rx_acc_len = 0;

    for (int attempt = 0; attempt < 3; attempt++) {
        uint8_t req[14];
        req[0] = 9;                     /* FDT manual sample */
        req[1] = 1;                     /* a3=1: carries the current down table */
        memcpy(&req[2], d->fdt_down_base, 12);
        LOG("FDT manual sample try %d (0x36 [9,1,table12])", attempt + 1);
        if (gx_send_cmd(d, 0x36, req, sizeof(req), 200) < 0) {
            LOG("FDT manual cmd send failed");
            continue;
        }
        int deadline = 1500;
        while (deadline > 0) {
            uint16_t plen = sizeof(d->rx_scratch);
            int r = next_payload(d, pl, &plen, sizeof(d->rx_scratch), 200);
            if (r <= 0) {
                deadline -= 200;
                continue;
            }
            int irq = fdt_irq_status(pl, plen);
            if (irq < 0)
                continue;               /* ACK / responses to other commands */
            if (irq == 0x100 && plen >= 3 + 17) {
                /* [cmd][v19][irq 2B][touchflag 2B][raw 12B][cks] */
                const uint8_t *raw = &pl[3 + 4];
                if (gx_fdt_learn_down_base(d, raw) == 0) {
                    d->base_valid = true;
                    d->base_dirty = true;
                    LOG("FDT base sampled ok (IrqStatus=0x100)");
                    if (tls)
                        gx_tls_capture_mode(d, 0);
                    return 0;
                }
                LOG("FDT sample invalid, retry");
                break;
            }
            /* a finger was detected during sampling (down/up event): this round is
             * void, sample again (the reference implementation compares two
             * samples by delta to tell whether a finger is on it) */
            if (irq == 2 || irq == 0x200) {
                LOG("FDT sample disturbed by finger (irq 0x%x), retry", irq);
                break;
            }
        }
    }
    if (tls)
        gx_tls_capture_mode(d, 0);
    return -1;
}

/* FDT down arming (capture trigger chain = cmd0=3 cmd1=1 = 0x32).
 * The reference implementation always uses a3=1: data=[8,1,12B down table,
 * ts16], 16B (the baseline sampling at init has already filled the table).
 * Only when not even a sampled baseline exists does it fall back to the
 * 4B a3=0 form [8,0,tbl0,tbl1]
 * (the fallback branch of the baseline presence check;
 *  tbl0/tbl1 are the first two bytes of the current table - all zeros when
 *  there is no table). */
static void arm_fdt_down(struct goodix_dev *d)
{
    if (d->base_valid) {
        uint8_t fdt[16] = { 0 };
        struct timeval tv;
        fdt[0] = 8;
        fdt[1] = 1;                 /* a3=1: carries the threshold table */
        memcpy(&fdt[2], d->fdt_down_base, 12);
        gettimeofday(&tv, NULL);
        uint16_t ts = (uint16_t)(tv.tv_sec % 60 * 1000 + tv.tv_usec / 1000);
        fdt[14] = (uint8_t)ts;
        fdt[15] = (uint8_t)(ts >> 8);
        LOG("capture: arming finger detect (FDT down with base, 0x32)");
        gx_send_cmd(d, 0x32, fdt, sizeof(fdt), 200);
    } else {
        uint8_t fdt[4] = { 8, 0, d->fdt_down_base[0],
                           d->fdt_down_base[1] };
        LOG("capture: arming finger detect "
            "(FDT down default-config, 0x32)");
        gx_send_cmd(d, 0x32, fdt, sizeof(fdt), 200);
    }
}

/* Image baseline part of baseline sampling: SetMode Image (cmd0=2 cmd1=0 =
 * 0x20, data=[1,0]) takes one no-finger image by direct exposure (used at
 * init / when rebuilding after temperature drift; there must be no finger on
 * the sensor). */
int gx_capture_base_image(struct goodix_dev *d)
{
    uint8_t im[2] = { 1, 0 };
    uint16_t sz = 0;
    LOG("capture base image (SetMode Image 0x20 [1,0], no-finger frame)");
    gx_send_cmd(d, 0x20, im, 2, 200);
    if (wait_image(d, d->img_base, &sz, 4000, 1) != 0 ||
        sz != d->img_size) {
        LOG("base image capture failed (sz=%u)", sz);
        return -1;
    }
    d->img_base_valid = true;
    d->base_dirty = true;
    LOG("base image captured (%u B) - goodix.dat imagebase", sz);
    return 0;
}

/* whether to take a fresh no-finger frame before every arming: on by default
 * (GOODIX_IMGPROC_FRESH_BASE=0 turns it off; always taken in calibration dump
 * mode for offline comparison) */
static int fresh_wanted(const struct goodix_dev *d)
{
    const char *e = getenv("GOODIX_IMGPROC_FRESH_BASE");
    if (d->fresh_broken)
        return 0;
    return getenv("GOODIX_DUMP_IMGPROC") != NULL || !e || atoi(e) != 0;
}

int gx_capture_fresh_image(struct goodix_dev *d)
{
    uint8_t im[2] = { 1, 0 };
    uint16_t sz = 0;
    uint8_t *tmp;
    void (*cb)(struct goodix_dev *, int, void *) = d->event_cb;
    const char *e = getenv("GOODIX_FRESH_MAXDC");
    double maxdc = e && *e ? atof(e) : 300.0;
    const char *er = getenv("GOODIX_FRESH_MAXRIDGE");
    double maxridge = er && *er ? atof(er) : 12.0;
    const uint8_t *ref;
    double dc = 0.0;
    int r;

    tmp = malloc(GF_IMG_MAX);
    if (!tmp)
        return -1;
    /* give a just-lifted finger a moment to leave; no finger events are
     * reported to libfprint meanwhile */
    usleep(100000);
    if (d->capture_stop) {          /* no command sent yet, safe to give up */
        free(tmp);
        return -4;
    }
    d->event_cb = NULL;
    gx_send_cmd(d, 0x20, im, 2, 200);
    r = wait_image(d, tmp, &sz, 2000, 0);
    d->event_cb = cb;
    if (r != 0 || sz != d->img_size) {
        free(tmp);
        /* Timeout/failure: the image request may still be pending on the MCU,
         * and stacking another request would hang the following captures -
         * fresh frames are disabled for the rest of this open. */
        d->fresh_broken = true;
        LOG("fresh no-finger frame failed (%d, sz=%u) - disabled for this "
            "session, using stored base", r, sz);
        return -1;
    }
    gx_imgproc_dump16(d, "pre16", tmp);
    /* Sanity check: compare the overall lowering with the last trusted fresh
     * frame (tracking drift) or the saved baseline. A finger on the sensor
     * lowers it by ~1000+ counts; when rejected, the last trusted fresh frame
     * is kept as is (when retrying repeatedly with the finger held down, the
     * baseline is still the one from a few seconds earlier). */
    ref = d->img_fresh_valid ? d->img_fresh : d->img_base_valid ? d->img_base : NULL;
    if (ref) {
        uint32_t npix = (uint32_t)d->img_w * d->img_h;
        for (uint32_t i = 0; i < npix; i++)
            dc += (double)px16(tmp, i) - (double)px16(ref, i);
        dc /= npix;
    }
    if (ref && (dc > maxdc || dc < -maxdc)) {
        /* With no trusted fresh frame yet, the saved baseline itself may have
         * drifted a lot: no ridge/valley structure (low band-pass contrast)
         * means it is just drift and not a finger, so it can still be used. */
        struct gx_imgproc_params p = GX_IMGPROC_SIGFM_PARAMS;
        struct gx_imgproc_signal sig = { 0 };
        p.fresh_base = 0;
        if (d->img_fresh_valid || gx_imgproc_signal(d, &p, tmp, &sig) != 0 ||
            sig.ridge >= maxridge) {
            LOG("fresh no-finger frame rejected: dc=%.0f ridge=%.1f vs %s "
                "(finger on sensor?)", dc, sig.ridge,
                d->img_fresh_valid ? "last fresh" : "stored base");
            free(tmp);
            return 1;
        }
        LOG("fresh no-finger frame: stored base drifted (dc=%.0f) but no "
            "ridge structure (%.1f), accepted", dc, sig.ridge);
    }
    memcpy(d->img_fresh, tmp, sz);
    d->img_fresh_valid = true;
    LOG("fresh no-finger frame ok (dc=%.0f)", dc);
    free(tmp);
    return 0;
}

/* Waits for the device to push the FDT up frame (IrqStatus=0x200): after the
 * 0x34 arming at the end of gx_capture, the MCU pushes this event when the
 * finger leaves the sensor (dispatched as device data cmd0=3). The libfprint
 * driver uses it to report FP_FINGER_NONE. */
int gx_wait_finger_up(struct goodix_dev *d, int timeout_ms)
{
    uint8_t *pl = d->rx_scratch;    /* device-level payload work area */
    int deadline = timeout_ms > 0 ? timeout_ms : 10000;
    int tls = d->tls_inited && d->tls;
    int ret = -2;

    d->rx_acc_len = 0;
    if (tls)
        gx_tls_capture_mode(d, 1);
    while (deadline > 0) {
        if (d->capture_stop) {
            ret = -4;
            break;
        }
        uint16_t plen = sizeof(d->rx_scratch);
        int r = next_payload(d, pl, &plen, sizeof(d->rx_scratch), 200);
        if (r < 0) {
            ret = -3;
            break;
        }
        if (r == 0) {
            deadline -= 200;
            continue;
        }
        int irq = fdt_irq_status(pl, plen);
        if (irq < 0)
            continue;               /* ACK / other frames */
        handle_fdt_frame(d, pl, plen, irq);
        if (irq == 0x200 || irq == 2) {
            /* 0x200 = lifted; 2 = lifted and pressed again (edge bounce): both end this
             * round */
            LOG("capture: finger up (irq 0x%x)", irq);
            ret = 0;
            break;
        }
    }
    if (tls)
        gx_tls_capture_mode(d, 0);
    return ret;
}

/* gx_capture: implementation of image capture + the POV wake-up path.
 *
 * Capture flow (image / POV / normal path):
 *   1. At the start: if the TLS project flag is set and (MCU state bit1
 *      isTlsConnected==0 or host TLS not ready) -> trigger a TLS server
 *      reconnection.
 *   2. The POV path runs only when MCU state bit0 (isPOVImageValid)==1:
 *      send 0xD2 {0,0} (POV wake-up command), wait for the POV frame
 *      (cmd0=13 cmd1=1 data[0]=0xAA); with bit0==0 no command is sent.
 *   3. Normal path: the capture trigger command = FDT down (0x32, 16B form
 *      [8,1,12B down table,ts16]). Then, when a finger touches, the device
 *      first pushes a cmd0=3 FDT event frame (also used to learn the up
 *      baseline) and the host sends SetMode Image to get the image.
 *   4. Host-side decision once the image arrives: 1=finger, delivered;
 *      2=void/3=bad, FDT DOWN re-armed; 0=temperature, baseline rebuilt
 *      (FDT/image baselines rebuilt, then armed again).
 *   5. For debugging, GOODIX_CAPTURE_IMAGE_MODE=1 skips FDT and sends
 *      SetMode Image (0x20 [1,0]) directly; the first frame is taken.
 * Only a finger touching the sensor produces FDT-driven image frames.
 */
int gx_capture(struct goodix_dev *d, uint8_t *img, uint16_t *size)
{
    int r;
    int pov_valid = 0;

    d->capture_stop = 0;            /* clear the cancel flag at the start of every capture */

    /* step 1: get the MCU state (pre-capture check).
     * state[1] bit0 = isPOVImageValid (POV condition),
     *          bit1 = isTlsConnected (TLS connection state),
     *          bit3 = isLocked. */
    {
        uint8_t st[16] = { 0 };
        int sr = gx_get_mcu_state(d, st);
        if (sr == 0) {
            LOG("MCU state: %02X %02X %02X %02X %02X %02X %02X %02X "
                "(POV_valid=%d tls_conn=%d locked=%d)",
                st[0], st[1], st[2], st[3], st[4], st[5], st[6], st[7],
                (st[1] & 1) ? 1 : 0, (st[1] & 2) ? 1 : 0,
                (st[1] & 8) ? 1 : 0);
            pov_valid = st[1] & 1;
            /* Pre-capture TLS liveness check: the host thinks TLS is up but the
             * MCU reports isTlsConnected==0 -> trigger a TLS server
             * reconnection */
            if (d->tls_inited && !(st[1] & 0x02)) {
                LOG("MCU reports TLS not connected, reconnecting");
                gx_tls_reconnect(d);
            }
        } else {
            LOG("GetMcuState failed (%d)", sr);
        }
    }

    /* step 2: POV wake-up - only when the MCU reports isPOVImageValid (POV
     * condition state[1]&1); otherwise take the normal path and arm FDT down
     * finger detection. */
    if (pov_valid) {
        LOG("capture: POV image valid, WakeupMCU (0xD2 {0,0})");
        uint8_t wake[2] = { 0, 0 };
        gx_send_cmd(d, 0xD2, wake, 2, 200);   /* evt=-1, the read loop skips the ACK */
        usleep(300000);                        /* let the MCU enter detection mode */
    } else if (getenv("GOODIX_CAPTURE_IMAGE_MODE")) {
        /* Debug bypass: SetMode Image directly (cmd0=2 cmd1=0 = 0x20,
         * data=[1,0]; set mode "setmode: Image").
         * The MCU exposes and pushes an image right away, no FDT trigger
         * needed. */
        uint8_t im[2] = { 1, 0 };
        LOG("capture: SetMode Image (0x20 [1,0]) - direct exposure");
        gx_send_cmd(d, 0x20, im, 2, 200);
    } else {
        /* fresh no-finger frame: a baseline right next to the finger frame
         * (calibration dump / FRESH_BASE mode) */
        if (fresh_wanted(d) &&
            (gx_capture_fresh_image(d) == -4 || d->capture_stop))
            return -4;
        arm_fdt_down(d);
    }

    LOG("capture: waiting for device-pushed image frame (tls=%d)",
        d->tls_inited ? 1 : 0);
    LOG("capture: PLACE YOUR FINGER ON THE SENSOR NOW");

    /* step 3: wait for the device to push a cmd0=2 image frame, then make the
     * host-side finger decision:
     *  1 finger      -> accepted and delivered
     *  2 void/3 bad  -> "not finger nor temp: to FDT DOWN", armed again
     *  0 temperature -> temperature drift: rebuild FDT + image baseline,
     *                   then arm again
     * Direct-exposure debug mode / no baseline image: old behaviour, the
     * first frame is taken. */
    r = -2;
    {
        int direct = getenv("GOODIX_CAPTURE_IMAGE_MODE") != NULL;
        int rejects = 0;
        int resampled = 0;
        for (;;) {
            r = wait_image(d, img, size, 10000, 1);
            if (r != 0)
                break;
            if (direct || !d->img_base_valid)
                break;
            int v = gx_fdt_check_finger(d, img);
            if (v == 1 || v == -1)
                break;
            if (++rejects > 4) {
                LOG("capture: too many rejected frames, giving up");
                r = -2;
                break;
            }
            if (v == 0) {
                /* temperature drift: rebuild the baseline -> FDT DOWN */
                LOG("capture: temperature drift, rebuild all base");
                gx_fdt_sample_base(d);
                if (gx_capture_base_image(d) == 0)
                    gx_base_save(d);
            } else if (v == 2 && !resampled &&
                       (d->last_fdt_irq == 0x80 || d->last_fdt_irq == 0x82)) {
                /* A reverse trigger but an empty frame: the down table no longer
                 * matches the current no-finger state (with fast repeated
                 * touches the down table was learned on 0x200 before the
                 * finger had fully left; from then on every arming fires
                 * immediately in reverse, and a real finger is photographed
                 * only as it first lands). An empty image = no finger on the
                 * sensor right now: resample the down baseline on the spot,
                 * as init does (empty table); keep the old table on failure.
                 * At most once per capture, to avoid loops. */
                uint8_t old[sizeof(d->fdt_down_base)];
                resampled = 1;
                LOG("capture: void frame on reverse event - finger-detect "
                    "base out of date, resampling");
                memcpy(old, d->fdt_down_base, sizeof(old));
                memset(d->fdt_down_base, 0, sizeof(d->fdt_down_base));
                if (gx_fdt_sample_base(d) == 0)
                    gx_base_save(d);
                else
                    memcpy(d->fdt_down_base, old, sizeof(old));
            } else {
                LOG("capture: %s frame rejected, re-arm FDT down",
                    v == 2 ? "void" : "bad");
            }
            arm_fdt_down(d);
        }
    }

    if (r == 0) {
        d->have_image = true;
        LOG("captured %u bytes", size ? *size : 0);
        /* End of capture: "to get upbase and to set FDT UP" ->
         * SetMode(3,2,1) = cmd (3<<4)|(2<<1) = 0x34,
         * data=[10, 1, fdt_up_base 12B], arms finger-lift detection (to
         * prepare the next capture loop; the next capture arms down again). */
        {
            uint8_t fup[14];
            fup[0] = 10;
            fup[1] = 1;
            memcpy(&fup[2], d->fdt_up_base, 12);
            gx_send_cmd(d, 0x34, fup, sizeof(fup), 200);
        }
        /* save the baseline: an image was captured and a new baseline was learned
         * meanwhile -> save it */
        if (d->base_dirty)
            gx_base_save(d);
    } else {
        LOG("no image frame received (finger not detected?)");
    }
    return r;
}
