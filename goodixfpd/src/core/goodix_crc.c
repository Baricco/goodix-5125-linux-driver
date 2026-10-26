/* goodix_crc.c - shared CRC-32/MPEG-2 implementation
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Three places in the project (image line CRC, goodix.dat baseline CRC,
 * firmware CRC) use the same algorithm: CRC-32/MPEG-2 (poly 0x04C11DB7,
 * initial value 0xFFFFFFFF, MSB first, no final XOR). Each used to have its
 * own implementation (bitwise / table x2); they are merged here into a single
 * table-driven implementation shared by the three modules.
 *
 * Note: this is the MPEG-2 variant, not zlib/ISO CRC-32 (which has the
 * opposite bit order and a final XOR); do not replace it with zlib's crc32().
 */
#include <stdint.h>
#include <stddef.h>
#include "goodix.h"     /* gx_crc32_mpeg2 prototype */

static uint32_t crc_tab[256];
static int crc_tab_ready = 0;

static void crc32_init_table(void)
{
    if (crc_tab_ready)
        return;
    for (int i = 0; i < 256; i++) {
        uint32_t c = (uint32_t)i << 24;
        for (int k = 0; k < 8; k++)
            c = (c & 0x80000000u) ? ((c << 1) ^ 0x04C11DB7u) : (c << 1);
        crc_tab[i] = c;
    }
    crc_tab_ready = 1;
}

/* Computes CRC-32/MPEG-2 of data starting from crc (usually 0xFFFFFFFFu) and
 * returns the current value. No final XOR: the result can be compared with
 * the stored value directly. */
uint32_t gx_crc32_mpeg2(const uint8_t *data, size_t len, uint32_t crc)
{
    crc32_init_table();
    for (size_t i = 0; i < len; i++)
        crc = (crc << 8) ^ crc_tab[(uint8_t)(data[i] ^ (crc >> 24))];
    return crc;
}
