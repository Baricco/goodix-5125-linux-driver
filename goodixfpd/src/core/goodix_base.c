/* goodix_base.c - FDT baseline persistence and learning (goodix.dat baseline
 * mechanism)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Baseline logic:
 *   - loading the baseline file (end of init): loads goodix.dat, CRC check +
 *     byte-by-byte OTP binding, used only if it matches.
 *   - saving the baseline: writes the file back after the baseline changes.
 *   - file baseline -> FDT-down threshold table.
 *   - validity check of baselines sent back by the device.
 *   - computing the down/up threshold tables from the raw data pushed with
 *     FDT IRQs.
 *
 * File layout (type 12 ChicagoHS):
 *   [OTP 64B][fdt_base 12B][navbase 3200B][imagebase 10240B][CRC32 4B LE]
 *   (the OTP section is 64 bytes, not the 256 of the read buffer)
 * CRC = CRC-32/MPEG-2 (poly 0x04C11DB7, initial value 0xFFFFFFFF, no final
 * XOR, table driven, seed = -1).
 *
 * Notes: navbase (3200 B) comes from the navigation baseline sampling of the
 * init-time baseline sampling flow and is used only for navigation events
 * (this driver does no navigation, the field is written as 0); imagebase
 * (10240 B) is the no-finger image baseline sampled by the same flow, used
 * after loading as the reference for host-side finger detection (FDT
 * decision), and persisted faithfully by this driver.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "goodix.h"

#define BASE_FDT_LEN   12                   /* FDT table length (type 12) */
#define BASE_NAV_LEN   3200                 /* navigation baseline length (type 12) */
#define BASE_IMG_LEN   10240                /* image baseline length (type 12) */
#define BASE_TAIL_LEN  (BASE_FDT_LEN + BASE_NAV_LEN + BASE_IMG_LEN + 4)

/* The table-driven CRC-32/MPEG-2 (poly 0x04C11DB7, initial value
 * 0xFFFFFFFF, no final XOR) is in goodix_crc.c, shared with the image line
 * CRC and the firmware CRC. */

const char *gx_state_dir(void)
{
    static char dir[512];
    const char *home;
    if (geteuid() == 0)
        return "/var/lib/fprint/goodix";
    home = getenv("HOME");
    if (!home || !*home)
        home = "/tmp";
    snprintf(dir, sizeof(dir), "%s/.config/goodix", home);
    return dir;
}

int gx_state_dir_ensure(void)
{
    const char *d;
    if (geteuid() == 0) {
        mkdir("/var/lib/fprint", 0755);
    } else {
        const char *home = getenv("HOME") ? getenv("HOME") : "/tmp";
        char parent[512];
        snprintf(parent, sizeof(parent), "%s/.config", home);
        mkdir(parent, 0700);
    }
    d = gx_state_dir();
    if (mkdir(d, 0700) == 0 || errno == EEXIST) {
        chmod(d, 0700);
        return 0;
    }
    return -1;
}

static const char *base_store_path(void)
{
    static char path[512];
    snprintf(path, sizeof(path), "%s/goodix.dat", gx_state_dir());
    return path;
}

/* baseline validity check: (v>>1)&0xFF of every u16 must not be 0 or 0xFF */
int gx_fdt_base_check(const uint8_t raw[12])
{
    for (int i = 0; i < 12; i += 2) {
        uint16_t v = (uint16_t)(raw[i] | (raw[i + 1] << 8));
        uint8_t b = (uint8_t)(v >> 1);
        if (b == 0 || b == 0xFF) {
            LOG("fdt base invalid (word%d = 0x%04x)", i / 2, v);
            return 0;
        }
    }
    return 1;
}

/* down threshold table learning: raw baseline of the fdt-up IRQ -> down
 * threshold table
 * table[i] = ((raw16[i] >> 1) << 8) | 0x80 */
int gx_fdt_learn_down_base(struct goodix_dev *d, const uint8_t raw[12])
{
    if (!gx_fdt_base_check(raw))
        return -1;
    for (int i = 0; i < 12; i += 2) {
        uint16_t v = (uint16_t)(raw[i] | (raw[i + 1] << 8));
        uint16_t w = (uint16_t)(((v >> 1) << 8) | 0x80);
        d->fdt_down_base[i] = (uint8_t)w;
        d->fdt_down_base[i + 1] = (uint8_t)(w >> 8);
    }
    d->base_dirty = true;
    LOG("learned fdt-down base from device IRQ");
    return 0;
}

/* up threshold table learning:
 * table[i] = ((raw16[i]>>1) + delta) << 8 | 0x80,
 * slots whose touchflag bit i is 0 are forced to ((delta-2)<<8)|0x80
 * (4992 with delta=21).
 * delta comes from OTP parsing (d->fdt_delta, default 21). */
int gx_fdt_learn_up_base(struct goodix_dev *d, const uint8_t raw[12],
                         uint16_t touchflag)
{
    uint8_t delta = d->fdt_delta ? d->fdt_delta : 21;
    if (!gx_fdt_base_check(raw))
        return -1;
    for (int i = 0; i < 6; i++) {
        uint16_t w;
        if (!((touchflag >> i) & 1)) {
            w = (uint16_t)(((delta - 2) << 8) | 0x80);
        } else {
            uint16_t v = (uint16_t)(raw[2 * i] | (raw[2 * i + 1] << 8));
            w = (uint16_t)(((delta + (v >> 1)) << 8) | 0x80);
        }
        d->fdt_up_base[2 * i] = (uint8_t)w;
        d->fdt_up_base[2 * i + 1] = (uint8_t)(w >> 8);
    }
    d->base_dirty = true;
    LOG("learned fdt-up base from device IRQ (touchflag 0x%x, delta %u)",
        touchflag, delta);
    return 0;
}

/* Baseline file loading:
 * read the file -> CRC check -> OTP binding -> take the 12 B FDT threshold
 * table. */
int gx_base_load(struct goodix_dev *d)
{
    uint8_t buf[256 + BASE_TAIL_LEN];
    const char *path = base_store_path();
    size_t want;
    FILE *f;
    size_t got;

    if (!d->otp_len)
        return -1;
    want = d->otp_len + BASE_TAIL_LEN;
    f = fopen(path, "rb");
    if (!f)
        return -1;                        /* no baseline file: sample instead */
    got = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (got != want) {
        LOG("base file %s size mismatch (%u, want %u), ignored",
            path, (unsigned)got, (unsigned)want);
        return -2;
    }
    uint32_t crc_file = (uint32_t)buf[got - 4] | ((uint32_t)buf[got - 3] << 8) |
                        ((uint32_t)buf[got - 2] << 16) | ((uint32_t)buf[got - 1] << 24);
    uint32_t crc_calc = gx_crc32_mpeg2(buf, got - 4, 0xFFFFFFFFu);
    if (crc_file != crc_calc) {
        LOG("base file CRC mismatch (file 0x%08x calc 0x%08x), ignored",
            crc_file, crc_calc);
        return -3;
    }
    if (memcmp(buf, d->otp, d->otp_len) != 0) {
        LOG("base file OTP mismatch (different device?), ignored");
        return -4;
    }
    memcpy(d->fdt_down_base, buf + d->otp_len, BASE_FDT_LEN);
    /* The table in the file must hold valid thresholds: the stored format is
     * "high byte = threshold, low byte = 0x80" (see gx_fdt_learn_down_base /
     * learn_up_base). An older comment claimed "the same validity check is
     * done before using the file", but it was missing - added here: a
     * threshold byte of 0/0xFF means a garbage table (all-zero file /
     * corruption / incompatible format), which is refused and resampled. */
    for (int i = 0; i < BASE_FDT_LEN; i += 2) {
        uint8_t hi = d->fdt_down_base[i + 1];
        if (hi == 0 || hi == 0xFF) {
            LOG("base file FDT table invalid (word%d hi=0x%02x), ignored",
                i / 2, hi);
            return -5;
        }
    }
    /* imagebase section (offset = otp + 12 + 3200): the no-finger baseline
     * image for host-side finger detection (loaded into the global baseline
     * image used for the decision).
     * Fixed 10240 B for type 12.
     * An all-zero section counts as invalid (older files stored only the
     * FDT table and the imagebase section was a placeholder of zeros; using
     * it would make the decision treat every frame as a finger). */
    if (d->img_size == BASE_IMG_LEN) {
        const uint8_t *ib = buf + d->otp_len + BASE_FDT_LEN + BASE_NAV_LEN;
        int nonzero = 0;
        for (int i = 0; i < BASE_IMG_LEN; i++)
            if (ib[i]) {
                nonzero = 1;
                break;
            }
        if (nonzero) {
            memcpy(d->img_base, ib, BASE_IMG_LEN);
            d->img_base_valid = true;
        }
    }
    /* (the FDT table validity check was done above) */
    d->base_valid = true;
    LOG("FDT base loaded from %s (CRC+OTP ok%s)", path,
        d->img_base_valid ? ", imagebase ok" : "");
    return 0;
}

/* Saving the baseline: fixed layout;
 * the navbase section is written as 0 (no navigation sampling), the
 * imagebase section with the real no-finger baseline image. */
int gx_base_save(struct goodix_dev *d)
{
    uint8_t buf[256 + BASE_TAIL_LEN];
    const char *path = base_store_path();
    size_t total;
    FILE *f;

    if (!d->otp_len)
        return -1;                        /* no file without OTP */
    total = d->otp_len + BASE_TAIL_LEN;
    memset(buf, 0, sizeof(buf));
    memcpy(buf, d->otp, d->otp_len);
    memcpy(buf + d->otp_len, d->fdt_down_base, BASE_FDT_LEN);
    /* imagebase section: real data when there is a baseline image, otherwise
     * left at 0 (without a sampled baseline the reference implementation
     * creates no file either - the caller only calls this when
     * img_base_valid). */
    if (d->img_base_valid && d->img_size == BASE_IMG_LEN)
        memcpy(buf + d->otp_len + BASE_FDT_LEN + BASE_NAV_LEN,
               d->img_base, BASE_IMG_LEN);
    uint32_t crc = gx_crc32_mpeg2(buf, total - 4, 0xFFFFFFFFu);
    buf[total - 4] = (uint8_t)crc;
    buf[total - 3] = (uint8_t)(crc >> 8);
    buf[total - 2] = (uint8_t)(crc >> 16);
    buf[total - 1] = (uint8_t)(crc >> 24);

    gx_state_dir_ensure();

    f = fopen(path, "wb");
    if (!f) {
        LOG("base store: cannot open %s", path);
        return -2;
    }
    if (fwrite(buf, 1, total, f) != total) {
        fclose(f);
        return -3;
    }
    fclose(f);
    chmod(path, 0600);
    d->base_dirty = false;
    LOG("FDT base stored to %s (%u B) - goodix.dat equivalent",
        path, (unsigned)total);
    return 0;
}
