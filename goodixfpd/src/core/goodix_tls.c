/* goodix_tls.c - OpenSSL PSK TLS server (memory BIO)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The device connects as the TLS client, the host acts as the server:
 *   - PSK identity: "Client_identity"
 *   - cipher suite: TLS-PSK-WITH-AES-128-GCM-SHA256 (0x00A8, TLS 1.2 only)
 *   - no certificate, no identity hint (so no ServerKeyExchange is sent)
 *   - TLS records travel in 0xB0 USB frames of the frame layer
 *
 * Handshake:
 *   1. create the SSL server context + two memory BIOs (rbio: records coming
 *      from the device, wbio: records going to the device)
 *   2. TLS server init command (0xD1 {0,0}) -> the device sends ClientHello
 *   3. loop on SSL_do_handshake() until done; every output record goes in its
 *      own 0xB0 frame (same send granularity as the old mbedtls version)
 *   4. TLS handshake command (0xD4 {0,0})
 *   5. read the MCU state (0xAF) -> bit1=1 means TLS connected; bit3=1
 *      locked -> resend the command
 *   6. mark tls_inited
 *
 * In capture mode (gx_tls_capture_mode(1)) the channel is mixed: A0
 * plaintext frames go to the dispatch path of goodix_capture.c, B0 frames are
 * injected through gx_tls_feed() and the decrypted data is taken out with
 * gx_tls_recv().
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include "goodix.h"

#define GX_TLS_IDENTITY     "Client_identity"
#define GX_TLS_CIPHER       "PSK-AES128-GCM-SHA256"
#define GX_TLS_REC_HDR      5
#define GX_TLS_MAX_FRAME_REC 0x3FFF     /* stays within MAX_FRAME */

struct gx_tls_ctx {
    SSL_CTX *ctx;
    SSL *ssl;
    BIO *rbio;              /* records device -> OpenSSL */
    BIO *wbio;              /* records OpenSSL -> device */
    int rx_no_usb;          /* capture mode: the TLS layer does not read USB itself */
    uint8_t frame[MAX_FRAME];   /* USB frame buffer during the handshake */
    uint8_t out[MAX_FRAME];     /* records waiting to be sent */
};

static void log_ssl_errors(const char *what)
{
    unsigned long e;
    int any = 0;

    while ((e = ERR_get_error()) != 0) {
        char buf[256];
        ERR_error_string_n(e, buf, sizeof(buf));
        LOG("%s: %s", what, buf);
        any = 1;
    }
    if (!any)
        LOG("%s: (no OpenSSL error queued)", what);
}

static unsigned int psk_server_cb(SSL *ssl, const char *identity,
                                  unsigned char *psk, unsigned int max_psk_len)
{
    struct goodix_dev *d = SSL_get_app_data(ssl);

    if (!d || !identity || strcmp(identity, GX_TLS_IDENTITY) != 0) {
        LOG("TLS: unexpected PSK identity '%s'", identity ? identity : "(null)");
        return 0;
    }
    if (d->psk_len == 0 || d->psk_len > max_psk_len)
        return 0;
    memcpy(psk, d->psk, d->psk_len);
    return (unsigned int)d->psk_len;
}

/* Sends the data OpenSSL produced in wbio to the device: one 0xB0 frame per
 * TLS record, with a 10 ms pause after each frame to give the device time to
 * process the record. */
static int flush_out(struct goodix_dev *d)
{
    struct gx_tls_ctx *t = d->tls;

    while (BIO_ctrl_pending(t->wbio) > 0) {
        uint8_t *rec = t->out;
        int n = BIO_read(t->wbio, rec, GX_TLS_REC_HDR);
        size_t body;

        if (n != GX_TLS_REC_HDR) {
            LOG("TLS: short record header in output (%d)", n);
            return -1;
        }
        body = ((size_t)rec[3] << 8) | rec[4];
        if (GX_TLS_REC_HDR + body > GX_TLS_MAX_FRAME_REC) {
            LOG("TLS: record too big for one frame (%u)", (unsigned)body);
            return -1;
        }
        if (body > 0 && BIO_read(t->wbio, rec + GX_TLS_REC_HDR, (int)body) != (int)body) {
            LOG("TLS: short record body in output");
            return -1;
        }
        if (gx_debug)
            fprintf(stderr, "[goodix] TLS send %uB: type %02X len %u\n",
                    (unsigned)(GX_TLS_REC_HDR + body), rec[0], (unsigned)body);
        if (gx_send_tls_frame(d, rec, (uint16_t)(GX_TLS_REC_HDR + body)) < 0)
            return -1;
        usleep(10000);
    }
    return 0;
}

/* During the handshake, reads one USB frame and hands its TLS records to
 * OpenSSL. 0 = frame received (non-TLS frames ignored); -1 = no data within
 * the timeout. */
static int pull_in(struct goodix_dev *d)
{
    struct gx_tls_ctx *t = d->tls;
    uint16_t flen = 0;

    if (t->rx_no_usb)
        return -1;
    if (gx_read_frame(d, t->frame, &flen, 500) < 0)
        return -1;
    if (flen < 5 || t->frame[0] != GF_TYPE_TLS)
        return 0;
    if (gx_debug)
        fprintf(stderr, "[goodix] TLS recv +%uB: type %02X\n",
                (unsigned)(flen - 4), t->frame[4]);
    if (BIO_write(t->rbio, &t->frame[4], flen - 4) != flen - 4)
        return -1;
    return 0;
}

static int new_bios(struct gx_tls_ctx *t)
{
    t->rbio = BIO_new(BIO_s_mem());
    t->wbio = BIO_new(BIO_s_mem());
    if (!t->rbio || !t->wbio) {
        BIO_free(t->rbio);
        BIO_free(t->wbio);
        t->rbio = t->wbio = NULL;
        return -1;
    }
    /* an empty memory BIO returns "retry" instead of EOF */
    BIO_set_mem_eof_return(t->rbio, -1);
    BIO_set_mem_eof_return(t->wbio, -1);
    SSL_set_bio(t->ssl, t->rbio, t->wbio);   /* ownership handed to SSL */
    return 0;
}

void gx_tls_free(struct goodix_dev *d)
{
    struct gx_tls_ctx *t = d->tls;

    if (!t)
        return;
    SSL_free(t->ssl);           /* frees both BIOs */
    SSL_CTX_free(t->ctx);
    OPENSSL_cleanse(t, sizeof(*t));
    free(t);
    d->tls = NULL;
    d->tls_inited = false;
}

void gx_tls_capture_mode(struct goodix_dev *d, int on)
{
    struct gx_tls_ctx *t = d->tls;
    if (t)
        t->rx_no_usb = on;
}

int gx_tls_feed(struct goodix_dev *d, const uint8_t *record, size_t len)
{
    struct gx_tls_ctx *t = d->tls;

    if (!t || len > MAX_FRAME)
        return -1;
    return BIO_write(t->rbio, record, (int)len) == (int)len ? 0 : -1;
}

int gx_tls_server_init(struct goodix_dev *d)
{
    struct gx_tls_ctx *t;

    if (!d->have_psk) {
        LOG("no PSK, cannot init TLS server");
        return -1;
    }
    gx_tls_free(d);
    t = calloc(1, sizeof(*t));
    if (!t)
        return -1;
    d->tls = t;

    t->ctx = SSL_CTX_new(TLS_server_method());
    if (!t->ctx)
        goto fail;
    /* Pin the version to TLS 1.2: 0x00A8 exists only in TLS 1.2 (TLS 1.3
     * has no pure PSK suite), and the device sends a TLS 1.2 ClientHello. */
    if (!SSL_CTX_set_min_proto_version(t->ctx, TLS1_2_VERSION) ||
        !SSL_CTX_set_max_proto_version(t->ctx, TLS1_2_VERSION))
        goto fail;
    if (!SSL_CTX_set_cipher_list(t->ctx, GX_TLS_CIPHER))
        goto fail;
    SSL_CTX_set_options(t->ctx, SSL_OP_NO_TICKET | SSL_OP_NO_RENEGOTIATION |
                                SSL_OP_NO_COMPRESSION);
    SSL_CTX_set_session_cache_mode(t->ctx, SSL_SESS_CACHE_OFF);
    SSL_CTX_set_psk_server_callback(t->ctx, psk_server_cb);

    t->ssl = SSL_new(t->ctx);
    if (!t->ssl)
        goto fail;
    SSL_set_app_data(t->ssl, d);
    SSL_set_accept_state(t->ssl);
    if (new_bios(t) < 0)
        goto fail;

    LOG("TLS server configured (PSK %u bytes, TLS1.2 pinned)", (unsigned)d->psk_len);
    return 0;

fail:
    log_ssl_errors("TLS server init");
    gx_tls_free(d);
    return -1;
}

int gx_tls_handshake(struct goodix_dev *d)
{
    struct gx_tls_ctx *t = d->tls;
    uint8_t state[16] = { 0 };
    int r = -1, tried = 0, done = 0;
    time_t t0;

    if (!t)
        return -1;
    if (d->tls_inited)
        return 0;

    r = gx_tls_server_cmd(d);
    LOG("TLSServerInit cmd ret=%d", r);

    /* After the command the device sends ClientHello. Bounded to about 30
     * seconds of wall-clock time so it never hangs forever. */
    t0 = time(NULL);
    while (time(NULL) - t0 < 30) {
        int hr = SSL_do_handshake(t->ssl);
        int err = hr == 1 ? SSL_ERROR_NONE : SSL_get_error(t->ssl, hr);

        if (flush_out(d) < 0) {
            err = SSL_ERROR_SSL;
        } else if (hr == 1) {
            done = 1;
            break;
        }
        if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
            log_ssl_errors("TLS handshake");
            /* Wrap-up: resend the TLS handshake command to give the MCU a chance
             * to unlock (the state bit3 lock blocks the following evk/init).
             * Root causes such as a PSK mismatch are handled by the PSK
             * self-healing path of init. */
            usleep(10000);
            gx_tls_handshake_cmd(d);
            return -1;
        }
        if (tried < 3)
            LOG("handshake step: waiting for device (try %d)", tried);
        tried++;
        if (pull_in(d) < 0 && t->rx_no_usb)
            break;
    }

    if (!done) {
        LOG("handshake not complete after %d tries (30s)", tried);
        usleep(10000);
        gx_tls_handshake_cmd(d);
        return -2;
    }
    LOG("TLS handshake done (server side), cipher %s",
        SSL_get_cipher_name(t->ssl));

    r = gx_tls_handshake_cmd(d);
    LOG("SendTlsHandshakeCmd ret=%d", r);

    for (int i = 0; i < 3; i++) {
        usleep(20000);
        if (gx_get_mcu_state(d, state) == 0) {
            if (state[1] & 0x02) {
                LOG("TLS connected confirmed (mcu state 0x%02x)", state[1]);
                d->tls_inited = true;
                return 0;
            }
            if (state[1] & 0x08) {
                LOG("MCU still locked, unlocking...");
                gx_tls_handshake_cmd(d);
            }
        }
    }
    LOG("TLS connect not confirmed in MCU (state 0x%02x)", state[1]);
    usleep(10000);
    gx_tls_handshake_cmd(d);
    return -3;
}

/* TLS reconnection: the device asks to rebuild TLS with a 0xDA plaintext
 * frame, or the host starts it when the MCU state shows
 * bit1 (isTlsConnected) = 0. The session and BIOs are cleared and the full
 * handshake runs again. */
int gx_tls_reconnect(struct goodix_dev *d)
{
    struct gx_tls_ctx *t = d->tls;

    if (!t)
        return -1;
    LOG("TLS reconnect requested, resetting session");
    if (!SSL_clear(t->ssl) || new_bios(t) < 0) {
        log_ssl_errors("TLS reconnect");
        return -1;
    }
    SSL_set_accept_state(t->ssl);
    d->tls_inited = false;
    return gx_tls_handshake(d);
}

/* Reads decrypted application data: > 0 = number of bytes; < 0 = no data
 * yet or error. */
int gx_tls_recv(struct goodix_dev *d, uint8_t *data, size_t cap, int tmo)
{
    struct gx_tls_ctx *t = d->tls;
    int n;

    (void)tmo;
    if (!t || !d->tls_inited)
        return -1;
    n = SSL_read(t->ssl, data, (int)cap);
    if (n > 0)
        return n;
    if (SSL_get_error(t->ssl, n) != SSL_ERROR_WANT_READ)
        log_ssl_errors("TLS read");
    flush_out(d);       /* there may be an alert to send to the device */
    return -1;
}
