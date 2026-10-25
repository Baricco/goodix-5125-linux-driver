/* test-tls.c - il server TLS-PSK del core contro un client TLS 1.2 PSK che
 * fa la parte del sensore: handshake, dati applicativi in acquisizione,
 * riconnessione, PSK sbagliata.
 *
 * Il livello USB e' sostituito da code in memoria: quello che il server
 * manda con gx_send_tls_frame() arriva al client, quello che il client
 * produce torna al server come trame 0xB0 da gx_read_frame().
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include "goodix.h"

int gx_debug;

/* ---- sensore finto ---- */

static SSL_CTX *cli_ctx;
static SSL *cli;
static BIO *cli_in, *cli_out;           /* dal server / verso il server */
static uint8_t client_psk[32];
static int connected;

#define QMAX 64
static uint8_t *q_buf[QMAX];
static uint16_t q_len[QMAX];
static int q_head, q_tail;

static void q_push_records(void)
{
    uint8_t buf[MAX_FRAME];
    int n;

    while ((n = BIO_read(cli_out, buf + 4, sizeof(buf) - 4)) > 0) {
        assert(q_tail < QMAX);
        buf[0] = GF_TYPE_TLS;
        q_buf[q_tail] = malloc(n + 4);
        memcpy(q_buf[q_tail], buf, n + 4);
        q_len[q_tail++] = (uint16_t)(n + 4);
    }
}

static void client_step(void)
{
    if (!SSL_is_init_finished(cli)) {
        int r = SSL_do_handshake(cli);
        if (r == 1)
            connected = 1;
    }
    q_push_records();
}

static unsigned int cli_psk_cb(SSL *ssl, const char *hint, char *identity,
                               unsigned int max_identity_len, unsigned char *psk,
                               unsigned int max_psk_len)
{
    (void)ssl; (void)hint; (void)max_psk_len;
    snprintf(identity, max_identity_len, "Client_identity");
    memcpy(psk, client_psk, 32);
    return 32;
}

static void client_new(void)
{
    if (cli)
        SSL_free(cli);
    cli = SSL_new(cli_ctx);
    cli_in = BIO_new(BIO_s_mem());
    cli_out = BIO_new(BIO_s_mem());
    BIO_set_mem_eof_return(cli_in, -1);
    BIO_set_mem_eof_return(cli_out, -1);
    SSL_set_bio(cli, cli_in, cli_out);
    SSL_set_connect_state(cli);
    connected = 0;
    q_head = q_tail = 0;
}

/* ---- stub del livello frame/comandi usati da goodix_tls.c ---- */

int gx_send_tls_frame(struct goodix_dev *d, const uint8_t *record, uint16_t rlen)
{
    (void)d;
    /* un record per trama, come per il sensore vero */
    assert(rlen >= 5 && (size_t)(((record[3] << 8) | record[4]) + 5) == rlen);
    BIO_write(cli_in, record, rlen);
    client_step();
    return 0;
}

int gx_read_frame(struct goodix_dev *d, uint8_t *frame, uint16_t *len, int tmo)
{
    (void)d; (void)tmo;
    if (q_head == q_tail)
        return -1;
    memcpy(frame, q_buf[q_head], q_len[q_head]);
    *len = q_len[q_head];
    free(q_buf[q_head++]);
    return 0;
}

int gx_tls_server_cmd(struct goodix_dev *d)
{
    (void)d;
    client_step();              /* ClientHello */
    return 0;
}

int gx_tls_handshake_cmd(struct goodix_dev *d)
{
    (void)d;
    return 0;
}

int gx_get_mcu_state(struct goodix_dev *d, uint8_t out[16])
{
    (void)d;
    memset(out, 0, 16);
    out[1] = connected ? 0x02 : 0x00;
    return 0;
}

/* ---- test ---- */

static void setup_dev(struct goodix_dev *d, uint8_t fill)
{
    memset(d, 0, sizeof(*d));
    memset(d->psk, fill, 32);
    d->psk_len = 32;
    d->have_psk = true;
}

static void test_handshake_and_data(void)
{
    static struct goodix_dev d;
    uint8_t out[4096];
    const char msg[] = "image payload from the sensor";

    setup_dev(&d, 0x5a);
    memset(client_psk, 0x5a, 32);
    client_new();

    assert(gx_tls_server_init(&d) == 0);
    assert(gx_tls_handshake(&d) == 0);
    assert(d.tls_inited);
    assert(connected);
    assert(strcmp(SSL_get_cipher_name(cli), "PSK-AES128-GCM-SHA256") == 0);
    assert(SSL_version(cli) == TLS1_2_VERSION);

    /* acquisizione: il chiamante legge le trame e inietta i record */
    gx_tls_capture_mode(&d, 1);
    assert(SSL_write(cli, msg, sizeof(msg)) == (int)sizeof(msg));
    q_push_records();
    while (q_head != q_tail) {
        uint8_t fr[MAX_FRAME];
        uint16_t fl;
        assert(gx_read_frame(&d, fr, &fl, 0) == 0);
        assert(gx_tls_feed(&d, &fr[4], fl - 4) == 0);
    }
    assert(gx_tls_recv(&d, out, sizeof(out), 0) == (int)sizeof(msg));
    assert(memcmp(out, msg, sizeof(msg)) == 0);
    assert(gx_tls_recv(&d, out, sizeof(out), 0) < 0);   /* niente altro */

    /* riconnessione chiesta dal sensore: sessione nuova */
    gx_tls_capture_mode(&d, 0);
    client_new();
    assert(gx_tls_reconnect(&d) == 0);
    assert(d.tls_inited && connected);

    gx_tls_free(&d);
    assert(d.tls == NULL && !d.tls_inited);
    gx_tls_free(&d);            /* idempotente */
    printf("ok - handshake, dati in acquisizione, riconnessione\n");
}

static void test_wrong_psk(void)
{
    static struct goodix_dev d;

    setup_dev(&d, 0x11);
    memset(client_psk, 0x22, 32);
    client_new();

    assert(gx_tls_server_init(&d) == 0);
    assert(gx_tls_handshake(&d) < 0);
    assert(!d.tls_inited);
    gx_tls_free(&d);
    ERR_clear_error();
    printf("ok - PSK diversa rifiutata\n");
}

static void test_no_psk(void)
{
    static struct goodix_dev d;

    memset(&d, 0, sizeof(d));
    assert(gx_tls_server_init(&d) < 0);
    assert(d.tls == NULL);
    printf("ok - niente PSK, niente server\n");
}

int main(void)
{
    cli_ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_min_proto_version(cli_ctx, TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(cli_ctx, TLS1_2_VERSION);
    SSL_CTX_set_cipher_list(cli_ctx, "PSK-AES128-GCM-SHA256");
    SSL_CTX_set_psk_client_callback(cli_ctx, cli_psk_cb);

    test_handshake_and_data();
    test_wrong_psk();
    test_no_psk();

    SSL_free(cli);
    SSL_CTX_free(cli_ctx);
    return 0;
}
