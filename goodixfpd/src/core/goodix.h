/* goodix.h - Goodix USB fingerprint sensor driver (complete device driver
 * implementation)
 *
 * Implements the complete device protocol on Linux:
 *   - libusb bulk transport layer
 *   - frame and command layers
 *   - initialization sequence
 *   - PSK management through the MCU
 *   - white-box AES for the PSK
 *   - TLS-PSK server channel (OpenSSL memory BIO)
 *   - image capture
 *
 * Devices: 27c6:5125 / 27c6:5135, GF5116M (Milan), CDC ACM bulk transport.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef GOODIX_H
#define GOODIX_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

extern int gx_debug;  /* USB hex dump switch (GOODIX_DEBUG=1) */

/* shared log macro (all modules) */
#define LOG(fmt, ...) fprintf(stderr, "[goodix] " fmt "\n", ##__VA_ARGS__)

/* firmware update "MCU soft reset, stop init" marker (0xFFDFFFF3) */
#define FW_SOFT_RESET (-2097165)

#define GOODIX_VID         0x27c6
#define GOODIX_PID_5125    0x5125
#define GOODIX_PID_5135    0x5135

/* Endpoints verified from the 27c6:5125 configuration descriptor (pkt9):
 * USB CDC-ACM, 2 interfaces:
 *   iface 0 (CDC comm): EP 0x82 INT-IN(8B)   -- notifications
 *   iface 1 (CDC data): EP 0x01 BULK-OUT(64B)
 *                       EP 0x81 BULK-IN(64B) -- data channel
 * The bulk endpoints 0x01/0x81 on iface 1 are used. */
#define GOODIX_EP_OUT      0x01    /* bulk OUT 64B, interface 1 */
#define MAX_FRAME        20480   /* TLS record (16K) / image frame (19K) */
#define GOODIX_EP_IN       0x81    /* bulk IN 64B, interface 1 */
#define GOODIX_IFACE_DATA  1

/* ---- frame types ---- */
#define GF_TYPE_PLAIN   0xA0
#define GF_TYPE_TLS     0xB0
#define GF_NO_CHECK     0x88    /* frame tail value: skip the checksum */

/* ---- frame offsets ---- */
#define GF_HDR_TYPE     0
#define GF_HDR_LEN      1       /* 2-byte little-endian payload length */
#define GF_HDR_CKS      3       /* b0+b1+b2 */
#define GF_CMD_BYTE     4       /* (cmd0<<4)|(cmd1<<1) */
#define GF_DATA_LEN     5       /* 2-byte little endian: data length + 1 */
#define GF_DATA         7

/* USB command bytes (command byte = (cmd0<<4)|(cmd1<<1), |1 = more bit) */
enum {
    GF_CMD_RESET_SENSOR    = 0xA2,  /* (10,1) data={1,20} */
    GF_CMD_CLEAR_APP       = 0xA4,  /* (10,2) data={0,0} */
    GF_CMD_READ_OTP        = 0xA6,  /* (10,3) data={0,0} */
    GF_CMD_EVK_VERSION     = 0xA8,  /* (10,4) data={0,0} */
    GF_CMD_MCU_STATE       = 0xAF,  /* (10,7) data={0x55,ts16,0,0}, more=1 */
    GF_CMD_CHIPREG_READ    = 0x82,  /* (8,1)  data={0,reg16,len16} */
    GF_CMD_SETMODE_IDLE    = 0x20,  /* (2,0) */
    GF_CMD_SETMODE_CAPTURE = 0x36,  /* (3,3) */
    GF_CMD_DOWNLOAD_CONFIG = 0x90,  /* (9,0)  data=cfg(224B) */
    GF_CMD_SET_DRIVER_STATE= 0x97,  /* (9,3)  data={state,2} */
    GF_CMD_TLS_SERVER_INIT = 0xD1,  /* (13,0) data={0,0}, more=1 */
    GF_CMD_TLS_HANDSHAKE   = 0xD4,  /* (13,2) data={0,0} */
    GF_CMD_MCU_READ        = 0xE4,  /* (14,2) data={type32,0} */
    GF_CMD_MCU_WRITE       = 0xE0,  /* (14,0) */
    GF_CMD_FW_SLICE        = 0xF0,  /* (15,0) {off32,len32,data} */
    GF_CMD_FW_FINISH       = 0xF4,  /* (15,2) {0,0,len32,crc32} */
};

/* SetMode */
enum {
    GF_MODE_IDLE = 0,
    GF_MODE_FDT_DOWN = 1,
    GF_MODE_FDT_UP = 2,
    GF_MODE_CAPTURE = 3,
};

/* response types (high nibble of the response byte) */
enum {
    GF_RESP_IMAGE = 2,
    GF_RESP_FDT = 3,
    GF_RESP_NAV = 5,
    GF_RESP_REGRW = 8,
    GF_RESP_CONFIG = 9,
    GF_RESP_OTHER = 10,
    GF_RESP_NOTICE = 12,
    GF_RESP_PRODUCTION = 13,
    GF_RESP_PROD_RESP = 14,
};

/* Sensor geometry. Picked by chipid:
 *   type 2  MilanG   (chipid 0x2208): 176x54,  frame 19008
 *   type 3  MilanL   (chipid 0x2205): 112x132, frame 29568
 *   type 12 ChicagoHS(0x2503/0x2504): 80x64,   frame 10240
 *   type 14 ChicagoT (0x2505-0x2508/0x2510): 36x160, frame 11520
 * The target device (27c6:5125 GF5116M) is MilanG. */
#define GF_IMG_W    176     /* MilanG defaults */
#define GF_IMG_H    54
#define GF_IMG_SIZE (GF_IMG_W * GF_IMG_H * 2)   /* 19008 */
#define GF_IMG_MAX  29568   /* largest frame (MilanL 112x132x2) */

/* production data types */
/* Production data types:
 *  0xBB020002 = PSK data block: [seal][entropy 8B][0xBB010003 wb block]
 *  0xBB020003 = PSK hash (32B) computed by the MCU from the PSK it decrypted
 *               from the WB
 *  write TLV:  [0xBB010002][len][seal][entropy][0xBB010003][len][wb] */
#define GF_DT_PSK_DATA   0xBB010002u  /* host PSK data (seal+entropy) */
#define GF_DT_PSK_HASH   0xBB020003u  /* WB hash computed by the MCU (checks that the PSK is valid) */
#define GF_DT_PSK_WB     0xBB010003u  /* WB block storage segment (second TLV of the key write) */

/* ---- device ---- */
struct goodix_dev {
    void *usb_ctx;
    void *usb_devh;
    uint16_t vid, pid;

    /* bulk endpoints (verified 0x01 OUT / 0x81 IN) */
    uint8_t ep_out;
    uint8_t ep_in;

    /* sensor info */
    uint8_t evk[64];
    uint16_t chipid;
    uint8_t sensor_type;
    uint16_t img_w, img_h, img_size;

    /* PSK (32B, provided by the MCU through white-box AES) */
    uint8_t psk[32];
    size_t psk_len;
    bool have_psk;

    /* TLS (OpenSSL, see goodix_tls.c) */
    void *tls;          /* TLS context */
    bool tls_inited;    /* TLS handshake done and confirmed by the device */

    /* capture */
    uint8_t image[GF_IMG_MAX];
    bool have_image;
    volatile int capture_stop;      /* set by gx_capture_cancel, checked by the capture loop */
    /* Optional capture event callback (for the libfprint glue): fired
     * synchronously in the capture context when FDT down/up push frames
     * arrive. See gx_capture_set_event_cb. */
    void (*event_cb)(struct goodix_dev *d, int event, void *user);
    void *event_cb_user;

    /* init state */
    bool mcu_inited;      /* MCU init done */
    bool fp_inited;       /* sensor init done */
    bool have_config;     /* config download succeeded */
    uint8_t besd;         /* BESD enable flag */

    /* OTP + FDT baseline */
    uint8_t otp[256];       /* contents read from the OTP (0xA6) */
    uint16_t otp_len;       /* valid length: type 12/14 = 64 */
    uint8_t fdt_delta;      /* FDT delta parsed from the OTP (default 21) */
    uint16_t fdt_tcode;     /* tcode parsed from the OTP (default 128) */
    uint8_t fdt_down_base[12]; /* FDT-down threshold table (transformed) */
    uint8_t fdt_up_base[12];   /* FDT-up threshold table */
    bool base_valid;        /* base file loaded and OTP matches / sampling succeeded */
    bool base_dirty;        /* a new baseline was learned from FDT IRQs at run time, to be saved */
    int last_fdt_irq;       /* IrqStatus of the latest FDT event (tells reverse triggers apart) */

    /* No-finger baseline image (the imagebase section of goodix.dat), the
     * reference for host-side finger detection.
     * Captured by direct exposure with SetMode Image at init and saved;
     * resampled on temperature drift (when the decision returns 0). */
    uint8_t img_base[GF_IMG_MAX];
    bool img_base_valid;    /* baseline image available (loaded from file or sampled at init) */

    /* Fresh no-finger frame: captured by direct exposure before arming FDT
     * down (on by default, GOODIX_IMGPROC_FRESH_BASE=0 turns it off); being
     * close in time to the finger frame, it removes baseline drift. It is
     * marked valid only if it passes the sanity check (its overall lowering
     * relative to img_base does not look like a finger). */
    uint8_t img_fresh[GF_IMG_MAX];
    bool img_fresh_valid;
    bool fresh_broken;      /* a fresh frame timed out during this open: no more requests, fall back to
     * the saved baseline */

    /* Aggregation buffer for TLS-decrypted data (the mixed channel of
     * goodix_capture.c: the byte stream decrypted from B0 frames is collected
     * into a complete payload before being dispatched). Kept in the device
     * struct instead of a file-level static, so multiple device instances
     * and reentrancy are safe. */
    uint8_t rx_acc[3 + 0x6C00];
    size_t rx_acc_len;

    /* Capture receive work buffers (goodix_capture.c), used sequentially by the
     * single-threaded capture path. rx_scratch is the payload work area of
     * the capture functions (wait_image / gx_fdt_sample_base /
     * gx_wait_finger_up, never concurrent); rx_frame is the USB frame
     * reassembly buffer of next_payload. Also kept in the device to avoid
     * file-level statics. */
    uint8_t rx_scratch[3 + 0x6C00];
    uint8_t rx_frame[MAX_FRAME];
};

/* ---- transport layer (transport.c) ---- */
int gx_transport_open(struct goodix_dev *d);
void gx_transport_close(struct goodix_dev *d);
int gx_usb_write(struct goodix_dev *d, const uint8_t *b, size_t n);
int gx_usb_read(struct goodix_dev *d, uint8_t *b, size_t cap, int tmo);
int gx_usb_reset(struct goodix_dev *d);

/* ---- shared CRC-32/MPEG-2 (goodix_crc.c) ----
 * poly 0x04C11DB7, initial/continuation value crc (usually 0xFFFFFFFFu),
 * MSB first, no final XOR. Shared by the image line CRC, the goodix.dat
 * baseline CRC and the firmware CRC. */
uint32_t gx_crc32_mpeg2(const uint8_t *data, size_t len, uint32_t crc);

/* After a capture cancel (gx_capture_cancel) the long flows (init wake-up
 * retries, re-enumeration polling, capture loop) use this to leave quickly,
 * so fprintd stop/deactivate does not wait for seconds. */
static inline int gx_stop_requested(struct goodix_dev *d)
{
    return d->capture_stop;
}

/* ---- frame layer (goodix_frame.c) ---- */
int  gx_send_cmd_frame(struct goodix_dev *d, uint8_t cmd,
                       const uint8_t *data, uint16_t datalen);
int  gx_send_tls_frame(struct goodix_dev *d, const uint8_t *record, uint16_t rlen);
int  gx_read_frame(struct goodix_dev *d, uint8_t *frame, uint16_t *len, int tmo);
int  gx_send_nop(struct goodix_dev *d);

/* ---- command layer (goodix_cmd.c) ---- */
int gx_send_cmd(struct goodix_dev *d, uint8_t cmd,
                const uint8_t *data, uint16_t len, int tmo);
int gx_send_cmd_wait(struct goodix_dev *d, uint8_t cmd,
                     const uint8_t *data, uint16_t len,
                     uint8_t *rsp, uint16_t *rsp_len, int tmo);
int gx_set_driver_state(struct goodix_dev *d, uint8_t state);
/* MCU production read/write */
int gx_mcu_read(struct goodix_dev *d, uint32_t data_type, uint8_t *out, uint16_t *len);
int gx_mcu_write(struct goodix_dev *d, const uint8_t *data, uint16_t len);
/* device command helpers */
int gx_dev_reset(struct goodix_dev *d);
int gx_dev_evk(struct goodix_dev *d, uint8_t out[64]);
int gx_get_mcu_state(struct goodix_dev *d, uint8_t out[16]);
int gx_tls_server_cmd(struct goodix_dev *d);
int gx_tls_handshake_cmd(struct goodix_dev *d);

/* ---- device init (goodix_init.c) ---- */
int gx_device_init(struct goodix_dev *d);

/* ---- PSK (goodix_psk.c) ---- */
int gx_psk_read_from_mcu(struct goodix_dev *d);          /* read + WB decrypt */
int gx_psk_write_to_mcu(struct goodix_dev *d, const uint8_t *psk); /* generate + WB encrypt + write */
int gx_wb_encrypt(const uint8_t *plain, uint32_t len, uint8_t *out, uint32_t *out_len);
int gx_wb_decrypt(const uint8_t *wb, uint32_t wb_len, uint8_t *plain, uint32_t *plain_len);
  /* Host-side PSK storage (the equivalent of Goodix_Cache.bin).
   * The reference implementation wraps the PSK with DPAPI
   * (CryptProtectData) for local persistence; the MCU never sees it. On
   * Linux the plaintext PSK is stored in a 0600 file under gx_state_dir() -
   * the equivalent single-user protection. A Secret Service (libsecret)
   * backend could be used instead, with no change to the MCU protocol. */
  int gx_psk_store_save(const uint8_t *psk, uint32_t len);
  int gx_psk_store_load(uint8_t *psk, uint32_t *len);
  /* host hash = SHA256(the whole WB block) */
  int gx_psk_host_hash(const uint8_t *wb, uint32_t wb_len, uint8_t out[32]);
  /* Check: reads the MCU hash 0xBB020003 and compares it with SHA256(our
   * WB), proving the MCU decrypted our WB block correctly (the WB encryption
   * is right). */
  int gx_psk_verify_mcu_hash(struct goodix_dev *d);

/* ---- TLS (goodix_tls.c) ---- */
int gx_tls_server_init(struct goodix_dev *d);
int gx_tls_handshake(struct goodix_dev *d);
int gx_tls_reconnect(struct goodix_dev *d);   /* reconnection (0xDA request) */
int gx_tls_recv(struct goodix_dev *d, uint8_t *data, size_t cap, int tmo);
/* frees the TLS context (called by gx_transport_close; may be called more
 * than once) */
void gx_tls_free(struct goodix_dev *d);
/* Mixed-channel support for the capture wait mode (behaviour of the capture
 * channel: A0 plaintext frames go straight into frame processing, B0 frames
 * are decrypted by OpenSSL and then enter the same flow):
 *   after gx_tls_capture_mode(1) tls_bio_recv no longer reads USB itself (so
 *   plaintext push frames are not swallowed), it only drains the injection
 *   buffer; the caller receives frames with gx_read_frame, injects B0
 *   records with gx_tls_feed and takes the decrypted data with gx_tls_recv. */
void gx_tls_capture_mode(struct goodix_dev *d, int on);
int  gx_tls_feed(struct goodix_dev *d, const uint8_t *record, size_t len);

/* ---- capture (goodix_capture.c) ---- */
int gx_capture(struct goodix_dev *d, uint8_t *img, uint16_t *size);
/* FDT baseline sampling: FDT-manual (0x36 [9,1,table]) makes the MCU sample
 * the no-finger FDT baseline on the spot and push an IrqStatus=0x100 frame;
 * it is learned as the down threshold table.
 * Returns 0 on success with d->base_valid=true. */
int gx_fdt_sample_base(struct goodix_dev *d);
/* Image baseline sampling: SetMode Image (0x20 [1,0]) captures one
 * no-finger image by direct exposure into d->img_base (it does not enter the
 * FDT decision loop).
 * Returns 0 on success with d->img_base_valid=true. */
int gx_capture_base_image(struct goodix_dev *d);
/* Fresh no-finger frame: direct exposure as above, stored in d->img_fresh
 * (img_base/goodix.dat untouched). Event callbacks are masked during the
 * capture. If its overall lowering relative to the last trusted fresh frame
 * (or the saved baseline if there is none) exceeds GOODIX_FRESH_MAXDC
 * (default 300 counts, a full finger press is ~1200) a finger is assumed to
 * be on the sensor and the frame is discarded, keeping the last trusted
 * fresh frame; with no fresh frame yet, a band-pass contrast below
 * GOODIX_FRESH_MAXRIDGE (default 12) means the saved baseline has drifted
 * and the frame is used. Returns 0 = used, 1 = discarded, < 0 on failure. */
int gx_capture_fresh_image(struct goodix_dev *d);
/* Host-side finger detection - per-block statistics of the new frame
 * compared with the no-finger baseline image.
 * img is the unpacked 16-bit LE frame (w*h*2 bytes).
 * Returns: 1 = finger down (accepted), 2 = void trigger, 3 = bad frame,
 *          0 = temperature drift (baseline must be rebuilt), -1 = no baseline
 *          to decide with. */
int gx_fdt_check_finger(struct goodix_dev *d, const uint8_t *img);
/* Image preprocessing (for host-side matching, not the device protocol
 * path): gx_imgproc_to8bit / gx_imgproc_params are in goodix_imgproc.h
 * (included at the end of this file). */
/* capture events (the event argument of gx_capture_set_event_cb) */
enum {
    GX_EV_FINGER_DOWN = 1,      /* FDT IrqStatus=0x2 push frame */
    GX_EV_FINGER_UP   = 2,      /* FDT IrqStatus=0x200 push frame */
};
void gx_capture_set_event_cb(struct goodix_dev *d,
                             void (*cb)(struct goodix_dev *d, int event,
                                        void *user),
                             void *user);
/* Cancels a running capture (gx_capture/gx_capture_base_image/
 * gx_wait_finger_up return -4 at the next receive cycle); cleared
 * automatically by the next gx_capture. */
void gx_capture_cancel(struct goodix_dev *d);
/* After arming FDT UP, waits for the device to push IrqStatus=0x200 (finger
 * lifted). Returns 0 = lifted; -2 = timeout; -4 = cancelled. */
int gx_wait_finger_up(struct goodix_dev *d, int timeout_ms);

/* ---- OTP parsing + config patching (goodix_otp.c) ----
 * Patches the 224B config table with the OTP factory calibration (tcode/fdt
 * delta/fdtOffset/exposure DAC); must be called before the config download.
 * delta_out returns the FDT delta (default 21). Returns 1 = OTP valid and
 * patched. */
int gx_otp_patch_config(struct goodix_dev *d, uint8_t *cfg, uint8_t *delta_out);

/* ---- FDT baseline persistence (goodix_base.c) ----
 * Saving the baseline on the Linux side: same file layout as goodix.dat
 *   [OTP 256B][fdt_base 12B][navbase 3200B][imagebase 10240B][CRC32/MPEG-2]
 * Check: CRC + byte-by-byte OTP match (invalidated automatically on another
 * device / after aging).
 * Learning: FDT IRQ frames carry the baseline measured by the device
 * (down/up threshold tables). */
/* State directory (where psk.bin / goodix.dat live):
 *   euid==0 -> /var/lib/fprint/goodix
 *     (fprintd's systemd sandbox, ProtectHome=true, hides /root and /home,
 *      while /var/lib/fprint is fprintd's own writable data directory; the
 *      CLI under sudo and fprintd therefore share the same PSK/baseline)
 *   others  -> $HOME/.config/goodix */
const char *gx_state_dir(void);
int  gx_state_dir_ensure(void);
int  gx_base_load(struct goodix_dev *d);
int  gx_base_save(struct goodix_dev *d);
/* validity check of a raw 12B baseline */
int  gx_fdt_base_check(const uint8_t raw[12]);
/* fdt-up IRQ raw data -> down threshold table */
int  gx_fdt_learn_down_base(struct goodix_dev *d, const uint8_t raw[12]);
/* fdt-down IRQ raw data -> up threshold table */
int  gx_fdt_learn_up_base(struct goodix_dev *d, const uint8_t raw[12],
                          uint16_t touchflag);

/* ---- firmware update (goodix_fwupdate.c) ---- */
int gx_fw_update(struct goodix_dev *d, uint8_t evk[64]);

#include "goodix_imgproc.h"

#endif /* GOODIX_H */

