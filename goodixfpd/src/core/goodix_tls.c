/* goodix_tls.c - OpenSSL PSK TLS server（memory BIO）
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * 设备以 TLS 客户端身份连接，主机作服务端：
 *   - PSK identity："Client_identity"
 *   - 密码套件：TLS-PSK-WITH-AES-128-GCM-SHA256（0x00A8，仅 TLS1.2）
 *   - 无证书、无 identity hint（因此不发 ServerKeyExchange）
 *   - TLS 记录经帧层 0xB0 USB 帧收发
 *
 * 握手流程：
 *   1. 建立 SSL server 上下文 + 两个 memory BIO（rbio：设备发来的记录，
 *      wbio：发往设备的记录）
 *   2. TLS server 初始化命令（0xD1 {0,0}）-> 设备发送 ClientHello
 *   3. 循环 SSL_do_handshake() 直到完成；每条输出记录单独装一个 0xB0 帧
 *      （与原 mbedtls 实现的发送粒度一致）
 *   4. TLS 握手命令（0xD4 {0,0}）
 *   5. 读 MCU 状态（0xAF）-> bit1=1 表示 TLS 已连接；bit3=1 锁定 -> 重发命令
 *   6. 标记 tls_inited
 *
 * 采集模式（gx_tls_capture_mode(1)）下为混合通道：A0 明文帧进入
 * goodix_capture.c 的分流路径，B0 帧经 gx_tls_feed() 注入、
 * gx_tls_recv() 取出解密数据。
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
#define GX_TLS_MAX_FRAME_REC 0x3FFF     /* 保持在 MAX_FRAME 内 */

struct gx_tls_ctx {
    SSL_CTX *ctx;
    SSL *ssl;
    BIO *rbio;              /* 设备 -> OpenSSL 的记录 */
    BIO *wbio;              /* OpenSSL -> 设备的记录 */
    int rx_no_usb;          /* 采集模式：TLS 层不自己读 USB */
    uint8_t frame[MAX_FRAME];   /* 握手期的 USB 帧缓冲 */
    uint8_t out[MAX_FRAME];     /* 待发送记录 */
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

/* 把 wbio 中 OpenSSL 产生的数据发给设备：每条 TLS 记录一个 0xB0 帧，
 * 每帧后延时 10ms，给设备时间处理 TLS 记录。 */
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

/* 握手期读一帧 USB，把其中的 TLS 记录交给 OpenSSL。
 * 0 = 收到帧（非 TLS 帧忽略）；-1 = 超时内无数据。 */
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
    /* memory BIO 为空时返回"重试"而非 EOF */
    BIO_set_mem_eof_return(t->rbio, -1);
    BIO_set_mem_eof_return(t->wbio, -1);
    SSL_set_bio(t->ssl, t->rbio, t->wbio);   /* 所有权移交给 SSL */
    return 0;
}

void gx_tls_free(struct goodix_dev *d)
{
    struct gx_tls_ctx *t = d->tls;

    if (!t)
        return;
    SSL_free(t->ssl);           /* 同时释放两个 BIO */
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
    /* 将版本锁定为 TLS1.2：0x00A8 仅存在于 TLS1.2（TLS1.3 无纯 PSK
     * 套件），设备发送的是 TLS1.2 ClientHello。 */
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

    /* 命令后设备发送 ClientHello。限制在约 30 秒墙钟时间内，
     * 避免永远挂起。 */
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
            /* 收尾：重发 TLS 握手命令，给 MCU 一次解锁机会（state bit3
             * 锁定态会阻塞后续 evk/init）。PSK 不匹配等根因由 init 的
             * PSK 自愈路径处理。 */
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

/* TLS 重连：设备通过 0xDA 明文帧主动要求重建 TLS，或读取 MCU 状态发现
 * bit1(isTlsConnected)=0 时由 host 发起。清空会话与 BIO 后重跑完整握手。 */
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

/* 读取解密后的应用数据：>0 = 字节数；<0 = 暂无数据或出错。 */
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
    flush_out(d);       /* 可能有发往设备的 alert */
    return -1;
}
