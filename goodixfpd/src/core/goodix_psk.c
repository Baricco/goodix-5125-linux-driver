/* goodix_psk.c - PSK management
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * PSK life cycle:
 *   first init: generate a random 32 B PSK -> white-box AES-GCM encryption ->
 *               write it to the MCU
 *   later inits: read the MCU data (0xBB010003) -> decrypt the block with the
 *                white-box AES -> use it as TLS-PSK
 *
 * The white-box encryption is AES-256-GCM with a fixed derived key plus an
 * HMAC-SHA256 envelope. Everything is deterministic (no random component), so
 * host_hash = SHA256(blob) always equals the MCU's hash of the stored blob.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include "goodix.h"

/* ======================================================================
 * PSK white-box AES-GCM. Built as AES-256-GCM with a fixed derived key plus
 * an HMAC-SHA256 envelope, corrected to the constants used by the MCU
 * firmware (st411sec_app.bin):
 *   KDF key = the "Goodix" variant (firmware default global: variant=0,
 *             string selector=1),
 *   GCM AAD = a firmware flash constant.
 * The firmware is the final decryptor - it decides the TLS PSK.
 *
 * Blob layout (inlen + 70 bytes; multi-byte fields are LE):
 *   [0..31]           HMAC-SHA256(key=v43[16..48],
 *                                   msg=blob[32..38] || ct || tag)
 *   [32..33]          02 FF   (WORD 0xFF02, stored LE)
 *   [34..37]          plaintext length (u32)
 *   [38..53]          iv = v42[0..15]
 *   [54..53+len]      ct  = AES-256-GCM ciphertext
 *   [54+len..69+len]  tag = GCM tag (16 B)
 *
 * Key schedule:
 *   v43 = T1 || T2[0..15]  (48 bytes)
 *   T_i = HMAC-SHA256(key=WB_KDF_KEY,
 *                     msg=BE32(i) || "kgoodwixg\0" ||
 *                         "kaelrgnoerlithm" || 00 00 01 80), i = 1,2
 *   AES-256-GCM key = v43[0..32], block HMAC key = v43[16..48]
 *   v42 = SHA256(02 FF || len32le || plain[0..len/4] || 16 x LE32(3))
 *   GCM iv = v42[0..15], GCM aad = WB_GCM_AAD (16 bytes)
 *
 * WB_GCM_AAD comes from the MCU firmware (loaded from flash by the 0xFF02
 * decryption path in st411sec_app.bin), not from the immediate AAD of the
 * driver's internal decryption (52 2A C3 F0 ...). The firmware is
 * authoritative: a blob that does not authenticate under the firmware AAD is
 * rejected by the MCU, which then derives a different TLS PSK -> the
 * handshake fails with bad_record_mac.
 * Verified through the firmware's WB decryption: key=WB_KDF_KEY + this AAD
 * reproduces the PSK exactly; the other AAD fails the firmware's GCM tag
 * check.
 * ====================================================================== */

static const uint8_t WB_KDF_KEY[32] = {
    /* firmware derive1 ("Goodix" selector) */
    0x5C, 0xBA, 0x6E, 0x25, 0x81, 0x95, 0x18, 0xDE,
    0x2D, 0x53, 0xE9, 0x6D, 0xC0, 0x34, 0x7A, 0xB0,
    /* firmware derive2 */
    0xD4, 0x27, 0xD4, 0x08, 0x4B, 0xDA, 0x4F, 0xAE,
    0x1B, 0xFF, 0x2B, 0x09, 0x11, 0x2A, 0x57, 0xE5
};

/* firmware GCM AAD (0xFF02 decryption path of st411sec_app.bin) */
static const uint8_t WB_GCM_AAD[16] = {
    0x52, 0x2D, 0xC1, 0xF0, 0x99, 0x56, 0x7D, 0x07,
    0xF4, 0x7F, 0x37, 0xA3, 0x2A, 0x84, 0x42, 0x7D
};

/* KDF: v43 = T1 || T2[0..15], 48 bytes in total */
static void psk_kdf(uint8_t out[48])
{
    /* "kgoodwixg\0" || "kaelrgnoerlithm" || 00 00 01 80 */
    static const uint8_t salt[29] = {
        'k','g','o','o','d','w','i','x','g', 0x00,
        'k','a','e','l','r','g','n','o','e','r','l','i','t','h','m',
        0x00, 0x00, 0x01, 0x80
    };
    uint8_t msg[4 + sizeof(salt)];
    uint8_t mac[32];
    unsigned int mlen = 0;
    for (int i = 1; i <= 2; i++) {
        msg[0] = 0; msg[1] = 0; msg[2] = 0; msg[3] = (uint8_t)i; /* BE32(i) */
        memcpy(msg + 4, salt, sizeof(salt));
        HMAC(EVP_sha256(), WB_KDF_KEY, sizeof(WB_KDF_KEY),
             msg, sizeof(msg), mac, &mlen);
        memcpy(out + (i - 1) * 32, mac, (i == 1) ? 32 : 16);
    }
}

/* v42: SHA256(02 FF || len32le || plain[0..len/4] || 16 x LE32(3)) */
static int psk_derive_v42(const uint8_t *plain, uint32_t len, uint8_t out[32])
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    uint8_t hdr[6] = { 0x02, 0xFF, (uint8_t)len, (uint8_t)(len >> 8),
                       (uint8_t)(len >> 16), (uint8_t)(len >> 24) };
    uint8_t d3[4] = { 3, 0, 0, 0 };
    unsigned int olen = 0;
    uint32_t head = len >> 2;   /* number of plaintext bytes hashed = len >> 2 */
    int ok;
    if (!ctx)
        return -1;
    ok = EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1 &&
         EVP_DigestUpdate(ctx, hdr, sizeof(hdr)) == 1 &&
         (head == 0 || EVP_DigestUpdate(ctx, plain, head) == 1);
    for (int i = 0; ok && i < 16; i++)
        ok = EVP_DigestUpdate(ctx, d3, 4) == 1;
    if (ok)
        ok = EVP_DigestFinal_ex(ctx, out, &olen) == 1 && olen == 32;
    EVP_MD_CTX_free(ctx);
    return ok ? 0 : -1;
}

/* AES-256-GCM encryption, 16-byte iv + 16-byte tag */
static int aes_256_gcm_encrypt(const uint8_t key[32], const uint8_t iv[16],
                               const uint8_t *plain, uint32_t len,
                               uint8_t *ct, uint8_t tag[16])
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int n = 0, n2 = 0;
    int ok;
    if (!ctx)
        return -1;
    ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1 &&
         EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 16, NULL) == 1 &&
         EVP_EncryptInit_ex(ctx, NULL, NULL, key, iv) == 1 &&
         EVP_EncryptUpdate(ctx, NULL, &n, WB_GCM_AAD, sizeof(WB_GCM_AAD)) == 1 &&
         EVP_EncryptUpdate(ctx, ct, &n, plain, (int)len) == 1 &&
         EVP_EncryptFinal_ex(ctx, ct + n, &n2) == 1 &&
         EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag) == 1;
    EVP_CIPHER_CTX_free(ctx);
    return ok ? 0 : -1;
}

/* AES-256-GCM decryption + tag check; returns -2 on authentication failure */
static int aes_256_gcm_decrypt(const uint8_t key[32], const uint8_t iv[16],
                               const uint8_t *ct, uint32_t len,
                               const uint8_t tag[16], uint8_t *plain)
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int n = 0, n2 = 0;
    int ok;
    if (!ctx)
        return -1;
    ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1 &&
         EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 16, NULL) == 1 &&
         EVP_DecryptInit_ex(ctx, NULL, NULL, key, iv) == 1 &&
         EVP_DecryptUpdate(ctx, NULL, &n, WB_GCM_AAD, sizeof(WB_GCM_AAD)) == 1 &&
         EVP_DecryptUpdate(ctx, plain, &n, ct, (int)len) == 1 &&
         EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, (void *)tag) == 1;
    if (ok && EVP_DecryptFinal_ex(ctx, plain + n, &n2) != 1)
        ok = 0;   /* GCM tag mismatch: WB key and blob do not agree */
    EVP_CIPHER_CTX_free(ctx);
    return ok ? 0 : -2;
}

/* White-box encryption. Output layout above; wb_len = len + 70 */
int gx_wb_encrypt(const uint8_t *plain, uint32_t len, uint8_t *out, uint32_t *out_len)
{
    uint8_t k48[48];
    uint8_t v42[32];
    uint8_t h[32];
    unsigned int hlen = 0;
    uint32_t total = 54 + len + 16;
    uint8_t msg[6 + 4096 + 16];
    size_t n = 0;

    if (len == 0 || len > 4096 || total > MAX_FRAME)
        return -1;
    psk_kdf(k48);
    if (psk_derive_v42(plain, len, v42) != 0)
        return -1;

    memset(out, 0, total);
    out[32] = 0x02;                     /* WORD 0xFF02, LE */
    out[33] = 0xFF;
    out[34] = (uint8_t)len;
    out[35] = (uint8_t)(len >> 8);
    out[36] = (uint8_t)(len >> 16);
    out[37] = (uint8_t)(len >> 24);
    memcpy(out + 38, v42, 16);          /* iv in clear */

    if (aes_256_gcm_encrypt(k48, v42, plain, len,
                            out + 54, out + 54 + len) != 0)
        return -1;

    /* block HMAC: HMAC-SHA256(key=v43[16..48],
     *                         msg=blob[32..38] || ct || tag) */
    memcpy(msg + n, out + 32, 6); n += 6;
    memcpy(msg + n, out + 54, (size_t)(len + 16)); n += len + 16;
    HMAC(EVP_sha256(), k48 + 16, 32, msg, n, h, &hlen);
    memcpy(out, h, 32);

    *out_len = total;
    return 0;
}

int gx_wb_decrypt(const uint8_t *wb, uint32_t wb_len, uint8_t *plain, uint32_t *plain_len)
{
    uint8_t k48[48];
    uint8_t h[32];
    unsigned int hlen = 0;
    uint32_t len;
    uint8_t msg[6 + 4096 + 16];
    size_t n = 0;

    if (wb_len < 70)
        return -1;
    psk_kdf(k48);
    len = (uint32_t)wb[34] | ((uint32_t)wb[35] << 8) |
          ((uint32_t)wb[36] << 16) | ((uint32_t)wb[37] << 24);
    if (len == 0 || len > 4096)
        return -3;
    if (54 + len + 16 > wb_len)
        return -2;

    /* check the block HMAC first, then handle the ciphertext */
    memcpy(msg + n, wb + 32, 6); n += 6;
    memcpy(msg + n, wb + 54, (size_t)(len + 16)); n += len + 16;
    HMAC(EVP_sha256(), k48 + 16, 32, msg, n, h, &hlen);
    if (memcmp(h, wb, 32) != 0)
        return -4;

    if (aes_256_gcm_decrypt(k48, wb + 38, wb + 54, len,
                            wb + 54 + len, plain) != 0)
        return -5;
    *plain_len = len;
    return 0;
}



/* ============ MCU write / read (PSK life cycle) =================
 *
 * Write TLVs:
 *   [0xBB010002 u32 LE] [len u32 LE] [dummy seal 64B] [entropy 8B]
 *   [0xBB010003 u32 LE] [wb_len u32 LE] [wb block 102B]
 * The MCU ignores the host-side DPAPI seal (an ARM chip cannot run
 * CryptProtectData); a fixed 64 B padding + 8 B of entropy are written here.
 *
 * Read: gx_mcu_read(GF_DT_PSK_DATA) returns the wb block; then WB decryption.
 */
int gx_psk_write_to_mcu(struct goodix_dev *d, const uint8_t *psk)
{
    uint8_t wb[MAX_FRAME];
    uint32_t wb_len = 0;
    uint8_t tlv[32 + 64 + 8 + 4 + MAX_FRAME];
    uint32_t o = 0;
    int r;

    r = gx_wb_encrypt(psk, 32, wb, &wb_len);
    if (r < 0) {
        LOG("WB encrypt failed (%d)", r);
        return r;
    }

    /* [0xBB010002][len][seal 64][entropy 8] */
    uint32_t dt1 = 0xBB010002;
    uint32_t len1 = 64 + 8;
    memcpy(tlv + o, &dt1, 4); o += 4;
    memcpy(tlv + o, &len1, 4); o += 4;
    memset(tlv + o, 0x5A, 64); o += 64;      /* dummy DPAPI seal */
    for (int i = 0; i < 8; i++)              /* entropy (random-ish) */
        tlv[o++] = (uint8_t)(i * 7 + 1);

    /* [0xBB010003][wb_len][wb] */
    uint32_t dt2 = 0xBB010003;
    memcpy(tlv + o, &dt2, 4); o += 4;
    memcpy(tlv + o, &wb_len, 4); o += 4;
    memcpy(tlv + o, wb, wb_len); o += wb_len;

    /* pads the data length to a 4-byte boundary. 190 -> 192. */
    while (o & 3)
        tlv[o++] = 0;
    r = gx_mcu_write(d, tlv, (uint16_t)o);
    if (r < 0) {
        LOG("gx_mcu_write failed (%d)", r);
        return r;
    }
    LOG("PSK written to MCU (wb %u B, tlv %u B)", wb_len, o);
    return 0;
}

int gx_psk_read_from_mcu(struct goodix_dev *d)
{
    uint8_t buf[1024];
    uint16_t bl = sizeof(buf);
    uint8_t wb[102];
    uint32_t plen = 0;
    int r;

    r = gx_mcu_read(d, GF_DT_PSK_WB, buf, &bl);   /* 0xBB010003 = WB storage segment */
    if (r < 0) {
        LOG("gx_mcu_read PSK failed (%d)", r);
        return r;
    }
    /* gx_mcu_read has already stripped the status byte; the payload is
     * [data_type 4B][len 4B][wb 102B] (data after the 8-byte header). Skip
     * the header. */
    if (bl < 8 + 102) {
        LOG("PSK read response too short (%u B, want >=110)", bl);
        return -2;
    }
    memcpy(wb, buf + 8, 102);
    r = gx_wb_decrypt(wb, 102, d->psk, &plen);
    if (r < 0 || plen != 32) {
        LOG("WB decrypt failed (%d, plen=%u)", r, plen);
        return -3;
    }
    d->psk_len = 32;
    d->have_psk = true;
    LOG("PSK recovered from MCU (WB-decrypt ok)");
    return 0;
}

/* ================= host-side PSK storage + MCU hash check =================
 *
 * Persistence: the host stores the equivalent of Goodix_Cache.bin, holding
 * [DPAPI-seal][entropy]; the seal lets the host recover the plaintext PSK
 * later without rereading the MCU. The MCU never checks the seal (it is an
 * ARM chip; CryptProtectData exists only on the host). On Linux the plaintext
 * PSK is stored in a 0600 file, the equivalent single-user protection. A
 * Secret Service (libsecret) backend could be used instead - whatever host
 * storage is chosen, the MCU protocol is unaffected.
 */

static const char *psk_store_path(void)
{
    static char path[512];
    snprintf(path, sizeof(path), "%s/psk.bin", gx_state_dir());
    return path;
}

int gx_psk_store_save(const uint8_t *psk, uint32_t len)
{
    const char *path = psk_store_path();

    gx_state_dir_ensure();

    FILE *f = fopen(path, "wb");
    if (!f) {
        LOG("PSK store: cannot open %s", path);
        return -1;
    }
    if (fwrite(psk, 1, len, f) != len) {
        fclose(f);
        return -1;
    }
    fclose(f);
    chmod(path, 0600);
    LOG("PSK stored to %s (0600, %u B) - Goodix_Cache.bin equivalent", path, len);
    return 0;
}

int gx_psk_store_load(uint8_t *psk, uint32_t *len)
{
    const char *path = psk_store_path();
    FILE *f = fopen(path, "rb");
    size_t got;
    if (!f)
        return -1;
    got = fread(psk, 1, *len, f);
    /* More content than the expected length = damaged/tampered cache: refuse
     * it and let the caller take the "read MCU / rewrite" self-healing path. */
    if (fgetc(f) != EOF) {
        LOG("PSK store %s oversized (cap %u), rejected", path, *len);
        fclose(f);
        return -1;
    }
    fclose(f);
    if (got < 1)
        return -1;
    *len = (uint32_t)got;
    LOG("PSK loaded from %s (%u B)", path, *len);
    return 0;
}

/* host hash = SHA256(WB block) */
int gx_psk_host_hash(const uint8_t *wb, uint32_t wb_len, uint8_t out[32])
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned int olen = 0;
    int ok;
    if (!ctx)
        return -1;
    ok = EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1 &&
         EVP_DigestUpdate(ctx, wb, wb_len) == 1 &&
         EVP_DigestFinal_ex(ctx, out, &olen) == 1 && olen == 32;
    EVP_MD_CTX_free(ctx);
    return ok ? 0 : -1;
}

/* Checks whether the MCU holds the same PSK we wrote.
 *
 * MCU hash response format:
 *   [status 1B][data_type 4B LE][len 4B LE][hash 32B]
 * gx_mcu_read has already stripped the status byte, so out =
 *   [data_type 4B][len 4B][hash 32B] (40 bytes), hash at out[8..39].
 * The hash is compared with SHA256(our WB block). Equal means the MCU
 * successfully decrypted our WB with its white box -> the encryption is
 * correct. */
int gx_psk_verify_mcu_hash(struct goodix_dev *d)
{
    uint8_t mcu_buf[64];
    uint8_t wb[MAX_FRAME];
    uint32_t wb_len = 0;
    uint8_t host_hash[32];
    uint16_t rl = sizeof(mcu_buf);
    int r;

    /* rebuild the WB block from the plaintext PSK we hold */
    r = gx_wb_encrypt(d->psk, 32, wb, &wb_len);
    if (r < 0)
        return r;

    /* read MCU hash (0xBB020003) */
    r = gx_mcu_read(d, GF_DT_PSK_HASH, mcu_buf, &rl);
    if (r < 0) {
        LOG("read MCU hash failed (%d)", r);
        return r;
    }
    /* response: [data_type 4B][len 4B][hash 32B] -> 40 bytes, hash @8 */
    if (rl < 40) {
        LOG("MCU hash response too short (%u B, want 40)", rl);
        return -4;
    }
    uint32_t dt, ln;
    memcpy(&dt, mcu_buf, 4);
    memcpy(&ln, mcu_buf + 4, 4);
    if (dt != GF_DT_PSK_HASH || ln != 32) {
        LOG("unexpected MCU hash header dt=0x%08X len=%u", dt, ln);
        return -5;
    }
    const uint8_t *mcu_hash = mcu_buf + 8;

    r = gx_psk_host_hash(wb, wb_len, host_hash);
    if (r < 0)
        return r;

    if (memcmp(mcu_hash, host_hash, 32) == 0) {
        LOG("*** MCU hash MATCHES SHA256(WB) - white-box crypto VERIFIED ***");
        return 0;
    }
    LOG("*** MCU hash MISMATCH - WB crypto wrong! ***");
    if (gx_debug) {
        fprintf(stderr, "[goodix] host: ");
        for (int i = 0; i < 32; i++) fprintf(stderr, "%02X", host_hash[i]);
        fprintf(stderr, "\n[goodix] mcu : ");
        for (int i = 0; i < 32; i++) fprintf(stderr, "%02X", mcu_hash[i]);
        fprintf(stderr, "\n");
    }
    return -6;
}
