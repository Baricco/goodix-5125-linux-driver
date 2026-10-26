/* goodix_otp.c - OTP parsing + 224 B configuration table patching.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Background:
 *   The init order is: read OTP -> parse OTP -> FDT init -> download config.
 *   OTP parsing uses the factory calibration values in the OTP to modify a
 *   few register entries of the 224 B configuration table on the spot, then
 *   the config download sends the modified table to the MCU.
 *   Sending the static table without the patch = FDT thresholds and exposure
 *   parameters all at generic defaults, and finger detection never fires
 *   (one of the root causes of no FDT event after arming with --capture).
 *
 *   Patched entries (type 12 ChicagoHS, chipid 0x2503/0x2504):
 *     reg 0x220 <- (16*dac0)|8     section 0, full write  (OTP DAC)
 *     reg 0x236 <- dac1            section 0, full write
 *     reg 0x238 <- dac2            section 0, full write
 *     reg 0x23A <- dac3            section 0, full write
 *     reg 0x5C  <- tcode           section 0, full write  (OTP tcode)
 *     reg 0x82  <- delta<<8        section 2, high byte only (FDT delta)
 *     reg 0x56  <- fdtOffset+4     section 2, low byte only (only if offset != 0)
 *
 * Configuration table layout:
 *   header: offset/size of the entry area of section s = cfg[2s+1], cfg[2s+2]
 *   entries: [reg16 LE][val16 LE], stride 4; on a hit the value is changed
 *            and the 16-bit checksum rewritten
 *   checksum: u16 cks = -(0xA5A5 + sum(u16[0..110])),
 *   stored in word[111] (bytes 222..223, LE).
 */
#include <string.h>
#include "goodix.h"

/* ---------------- CRC-8 ----------------
 * CRC-8/SMBUS (poly 0x07, initial value 0, MSB first), bitwise inverted on
 * return (return ~i). Computed bit by bit, no table. */
static uint8_t gf_crc8(const uint8_t *data, int len)
{
    uint8_t crc = 0;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07)
                               : (uint8_t)(crc << 1);
    }
    return (uint8_t)~crc;
}

/* ---------------- 16-bit checksum of the configuration table ---------------- */
static void gf_config_fix_checksum(uint8_t *cfg)
{
    int16_t v = (int16_t)0xA5A5;            /* -23131 */
    for (int i = 0; i < 111; i++)
        v = (int16_t)(v + (int16_t)(cfg[2 * i] | (cfg[2 * i + 1] << 8)));
    uint16_t cks = (uint16_t)(-v);
    cfg[222] = (uint8_t)cks;
    cfg[223] = (uint8_t)(cks >> 8);
}

/* ---------------- modify the configuration table ----------------
 * cfg: 224 B configuration table; reg: target register; val: new value;
 * section: 0..7 (selects the entry area in the header); mode: 0 = full write,
 * 1 = low byte only, 2 = high byte only.
 * Returns 1 = found and modified. */
static int gf_modify_config(uint8_t *cfg, uint16_t reg, uint16_t val,
                            int section, int mode)
{
    if (section < 0 || section > 7)
        return 0;
    uint8_t off = cfg[2 * section + 1];
    uint8_t cnt = cfg[2 * section + 2];
    /* Defensive: the entry area must lie inside the 224 B table with a 4 B
     * stride. The 4 current static tables are all verified as valid; this
     * check prevents out-of-bounds reads if a future table is wrong. */
    if (cnt % 4 != 0 || (uint16_t)off + cnt > 224)
        return 0;
    for (int i = 0; i < cnt; i += 4) {
        uint16_t r = (uint16_t)(cfg[off + i] | (cfg[off + i + 1] << 8));
        if (r != reg)
            continue;
        if (mode == 1) {
            cfg[off + i + 2] = (uint8_t)val;
        } else if (mode == 2) {
            cfg[off + i + 3] = (uint8_t)(val >> 8);
        } else {
            cfg[off + i + 2] = (uint8_t)val;
            cfg[off + i + 3] = (uint8_t)(val >> 8);
        }
        LOG("config patch: reg 0x%04x -> 0x%04x (section %d mode %d)",
            reg, val, section, mode);
        gf_config_fix_checksum(cfg);
        return 1;
    }
    LOG("config patch: reg 0x%04x not found in section %d", reg, section);
    return 0;
}

/* ---------------- OTP CRC group checks ----------------
 * cp: OTP[0..10] + OTP[36..39]        -> OTP[60]
 * ft: OTP[11..19] + OTP[28] + OTP[50..53] + OTP[56..59] + OTP[62] -> OTP[61]
 * mt: OTP[20..27] + OTP[29..35] + OTP[40..49] + OTP[54..55]       -> OTP[63]
 * In the reference implementation a CRC failure is only logged, it does not
 * block (the DAC check is the real gate of the parsing). */
static void gf_otp_crc_groups(const uint8_t *otp, int *cp_ok, int *ft_ok,
                              int *mt_ok)
{
    uint8_t buf[32];

    memset(buf, 0, sizeof(buf));
    memcpy(buf, otp, 11);
    memcpy(buf + 11, otp + 36, 4);
    *cp_ok = (gf_crc8(buf, 15) == otp[60]);

    memset(buf, 0, sizeof(buf));
    memcpy(buf, otp + 11, 9);
    buf[9] = otp[28];
    memcpy(buf + 10, otp + 50, 4);
    memcpy(buf + 14, otp + 56, 4);
    buf[18] = otp[62];
    *ft_ok = (gf_crc8(buf, 19) == otp[61]);

    memset(buf, 0, sizeof(buf));
    memcpy(buf, otp + 20, 8);
    memcpy(buf + 8, otp + 29, 7);
    memcpy(buf + 15, otp + 40, 10);
    memcpy(buf + 25, otp + 54, 2);
    *mt_ok = (gf_crc8(buf, 27) == otp[63]);

    LOG("OTP CRC: cp=%d ft=%d mt=%d", *cp_ok, *ft_ok, *mt_ok);
}

/* ---------------- OTP DAC extraction ----------------
 * ft dac = OTP[50..53] (own CRC8 -> OTP[62]), mt dac = OTP[46..49]
 * (CRC8 -> OTP[22]). Same selection order as the reference implementation:
 *   ft group valid (group CRC or own CRC) -> use ft;
 *   mt valid -> use mt;
 *   otherwise a per-byte majority vote (needs >= 3 agreeing bytes; with 3 in
 *   agreement the disagreeing byte takes the mean of the other three).
 * Returns 1 = dac[4] valid (this is the overall gate of OTP parsing). */
static int gf_otp_dac_extract(const uint8_t *otp, int ft_grp, int mt_grp,
                              uint8_t dac[4])
{
    int ft_ok = 0, mt_ok = 0;

    if (otp[50] && otp[51] && otp[52] && otp[53])
        ft_ok = (gf_crc8(otp + 50, 4) == otp[62]);
    if (ft_grp || ft_ok) {
        memcpy(dac, otp + 50, 4);
        return 1;
    }

    if (otp[46] && otp[47] && otp[48] && otp[49])
        mt_ok = (gf_crc8(otp + 46, 4) == otp[22]);
    if (mt_grp || mt_ok) {
        memcpy(dac, otp + 46, 4);
        return 1;
    }

    int same = 0;
    for (int i = 0; i < 4; i++)
        if (otp[46 + i] == otp[50 + i] && otp[50 + i])
            same++;
    if (same < 3) {
        LOG("OTP ft/mt dac match num check failed (%d)", same);
        return 0;
    }
    if (same == 4) {
        memcpy(dac, otp + 50, 4);
        return 1;
    }
    for (int i = 0; i < 4; i++) {
        if (otp[46 + i] == otp[50 + i]) {
            dac[i] = otp[50 + i];
        } else {
            dac[i] = (uint8_t)((otp[46 + (i + 2) % 4] +
                                otp[46 + (i + 3) % 4] +
                                otp[46 + (i + 1) % 4]) / 3);
        }
    }
    return 1;
}

/* ---------------- main OTP parsing entry point ----------------
 * Merges the OTP check and OTP parsing steps: parses the OTP and patches the
 * 224 B configuration table on the spot. Also outputs the FDT delta (needed
 * to learn the fdt-up baseline; default 21).
 * Returns 1 = OTP valid and patch done; 0 = OTP invalid (the reference
 * implementation then applies no patch, keeps the static table and uses the
 * default delta/tcode 21/128). */
int gx_otp_patch_config(struct goodix_dev *d, uint8_t *cfg, uint8_t *delta_out)
{
    const uint8_t *otp = d->otp;
    uint8_t dac[4];
    int cp_ok, ft_ok, mt_ok;
    uint8_t tcode_diff = 0;
    uint16_t tcode = 128;
    uint8_t delta = 21;

    *delta_out = 21;                 /* default fdt delta */
    d->fdt_tcode = 128;              /* default initial tcode */

    if (d->otp_len < 64) {
        LOG("OTP too short (%u), skip config patch", d->otp_len);
        return 0;
    }

    gf_otp_crc_groups(otp, &cp_ok, &ft_ok, &mt_ok);
    (void)cp_ok;

    if (!gf_otp_dac_extract(otp, ft_ok, mt_ok, dac)) {
        LOG("OTP DAC check failed - keep static config");
        return 0;
    }
    LOG("OTP dac 0x%02x 0x%02x 0x%02x 0x%02x", dac[0], dac[1], dac[2], dac[3]);

    /* DAC -> exposure registers (section 0, full write) */
    gf_modify_config(cfg, 0x220, (uint16_t)((16 * dac[0]) | 8), 0, 0);
    gf_modify_config(cfg, 0x236, dac[1], 0, 0);
    gf_modify_config(cfg, 0x238, dac[2], 0, 0);
    gf_modify_config(cfg, 0x23A, dac[3], 0, 0);

    /* tcode_diff: one of OTP[42]/[43]/[45] */
    if (otp[42] && otp[42] == (uint8_t)~otp[43])
        tcode_diff = otp[42];
    else if (otp[45] && otp[45] == (uint8_t)~otp[43])
        tcode_diff = otp[45];
    else if (otp[42] && otp[45] == otp[42])
        tcode_diff = otp[42];
    else
        LOG("wrong OTP tcode_diff - keep static tcode/delta");

    if (tcode_diff) {
        tcode = (uint16_t)(16 * (((tcode_diff >> 4) & 0xF) + 1) + 64);
        gf_modify_config(cfg, 0x5C, tcode, 0, 0);
        /* delta = ((100*((diff&0xF)+2) << 8) / tcode / 3) >> 4 */
        delta = (uint8_t)((((100u * ((tcode_diff & 0xF) + 2)) << 8)
                           / tcode / 3) >> 4);
        *delta_out = delta;
        gf_modify_config(cfg, 0x82, (uint16_t)(delta << 8), 2, 2);
        LOG("OTP tcode %u, fdt delta %u", tcode, delta);
    }
    d->fdt_tcode = tcode;   /* needed for the decision threshold T=tcode*delta/8 */

    /* fdtOffset: OTP[27], only chipid 0x2503/0x2504 (9475/9476) */
    if (otp[27] && (d->chipid == 0x2504 || d->chipid == 0x2503)) {
        uint8_t b = otp[27], off;
        if ((b & 3) == ((b >> 4) & 3))
            off = b & 3;
        else if ((b & 3) == ((~b >> 2) & 3))
            off = b & 3;
        else if (((b >> 4) & 3) == ((~b >> 2) & 3))
            off = (b >> 4) & 3;
        else
            off = 0;
        LOG("fdtOffset: 0x%x", off);
        if (off)
            gf_modify_config(cfg, 0x56, (uint16_t)(off + 4), 2, 1);
    }
    return 1;
}
