/*
 * fldigi-fsq-core: version, mode parameters, nibble table and CRC-8 (fldigi 4.2.13
 * src/fsq/fsq.cxx, src/ifkp/ifkp.cxx, src/include/crc8.h).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "fsq_internal.h"

#include <math.h>
#include <string.h>

const char *ffsq_version(void)
{
    return "fldigi-fsq-core " FFSQ_VERSION_STRING " contract=fsq-c1 abi=1 tx=1 dsp="
        FDSP_VERSION_STRING;
}

int ffsq_abi(void)
{
    return FFSQ_ABI;
}

int ffsq_sample_rate(int mode)
{
    return mode == FFSQ_MODE_FSQ ? FSQ_SR : mode == FFSQ_MODE_IFKP ? IFKP_SR : 0;
}

int ffsq_bandwidth(int mode)
{
    /* bandwidth = 33 * spacing * samplerate / FSQ_SYMLEN (fsq.cxx:260) and
     * set_bandwidth(33 * IFKP_SPACING * samplerate / symlen) (ifkp.cxx:270): integer
     * arithmetic, 290 and 386 */
    int sr = ffsq_sample_rate(mode);
    return sr ? 33 * FSQ_SPACING * sr / FSQ_SYMLEN : 0;
}

double ffsq_bw(int mode)
{
    return (double)ffsq_bandwidth(mode);
}

int ffsq_speed_ok(int mode, double speed)
{
    if (mode == FFSQ_MODE_FSQ)
        return speed == 1.5 || speed == 2.0 || speed == 3.0 || speed == 4.5 || speed == 6.0;
    if (mode == FFSQ_MODE_IFKP)
        return speed == 0.0 || speed == 1.0 || speed == 2.0;
    return 0;
}

int ffsq_symlen(int mode, double speed)
{
    if (!ffsq_speed_ok(mode, speed))
        return 0;
    if (mode == FFSQ_MODE_FSQ) {
        /* adjust_for_speed, fsq.cxx:322-338 */
        if (speed == 1.5)
            return 8192;
        if (speed == 2.0)
            return 6144;
        if (speed == 3.0)
            return 4096;
        if (speed == 4.5)
            return 3072;
        return 2048;
    }
    /* send_tone, ifkp.cxx:699-701: symlen * (2x: 0.5, 0.5x: 2.0, else 1.0) */
    return (int)(FSQ_SYMLEN * (speed == 2.0 ? 0.5 : speed == 0.0 ? 2.0 : 1.0));
}

double ffsq_clamp_carrier(int mode, double f)
{
    double bw = ffsq_bw(mode);
    if (f <= 0)
        f = 1500.0;
    if (mode == FFSQ_MODE_FSQ) {
        /* modem::set_freq, modem.cxx:321-324, CLAMP with the waterfall limits */
        double lo = FSQ_LOW_CUTOFF + bw / 2, hi = FSQ_HIGH_CUTOFF - bw / 2;
        return f < lo ? lo : f > hi ? hi : f;
    }
    /* ifkp::set_freq, ifkp.cxx:263-264 */
    if (f < 100 + 0.5 * bw)
        f = 100 + 0.5 * bw;
    if (f > 3900 - 0.5 * bw)
        f = 3900 - 0.5 * bw;
    return f;
}

int ffsq_valid_char(int ch)
{
    /* fsq.cxx:397-404, ifkp.cxx:364-371 */
    return ch == 10 || ch == 163 || ch == 176 || ch == 177 || ch == 215 || ch == 247 ||
           (ch > 31 && ch < 128);
}

/* init_nibbles, fsq.cxx:116-128 (= ifkp.cxx:100-112): nibble = floor(0.5 + (i - 99) / 3.0),
 * wrapped into 0 .. 32, minus 1 for the +1 symbol offset of the transmitter. */
void ffsq_init_nibbles(int nibbles[199])
{
    int i;
    for (i = 0; i < 199; i++) {
        int nibble = (int)floor(0.5 + (i - 99) / 3.0);
        if (nibble < 0)
            nibble += 33;
        if (nibble > 32)
            nibble -= 33;
        nibble--;
        nibbles[i] = nibble;
    }
}

/* CRC8, crc8.h:10-35: table of polynomial 0x07, val = table[val ^ s[i]], "%02x". */
void ffsq_crc8_hex(const char *s, char out[3])
{
    static const char hex[] = "0123456789abcdef";
    unsigned char val = 0;
    size_t i;
    for (i = 0; s && s[i]; i++) {
        unsigned char crc = (unsigned char)(val ^ (unsigned char)s[i]);
        int j;
        for (j = 0; j < 8; j++)
            crc = (unsigned char)((crc << 1) ^ ((crc & 0x80) ? 0x07 : 0));
        val = crc;
    }
    out[0] = hex[val >> 4];
    out[1] = hex[val & 0x0F];
    out[2] = 0;
}
