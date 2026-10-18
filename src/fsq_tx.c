/*
 * fldigi-fsq-core transmitter: fsq::tx_process and ifkp::tx_process of fldigi 4.2.13 with
 * their tone generators (src/fsq/fsq.cxx:1333-1536, src/ifkp/ifkp.cxx:687-733, 1056-1104).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "fsq_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* " " + FSQBOL " \n" + mycall + ":" + CRC: at most 4 + 2 * 20 + 2 + 2 * 2 symbols */
#define FSQ_MAX_HEADER_SYMBOLS (4 + 2 * FSQ_MAX_CALL + 2 + 4)

struct ffsq_tx {
    int mode;
    double speed;
    int samplerate;
    int symlen;                 /* samples per symbol sent */
    int basetone;               /* FSQ: tx_basetone (fsq.cxx:302-305); IFKP: basetone */
    int tone;
    int prevtone;
    double txphase;
    char mycall[FSQ_MAX_CALL + 1];
    int directed;
};

/* fsq::set_freq (fsq.cxx:297-306) / ifkp::set_freq (ifkp.cxx:256-272): the tone base. */
static void set_tonebase(ffsq_tx *tx, double carrier, int startup)
{
    int sr = tx->samplerate;
    if (tx->mode == FFSQ_MODE_FSQ) {
        /* bandwidth is still 0 when the constructor and rx_init call set_freq, 290 after
         * (fsq.cxx:258-260, quirk Q3); the TX frequency is always 1500 (modem.cxx:349) */
        double bandwidth = startup ? 0.0 : ffsq_bw(FFSQ_MODE_FSQ);
        double frequency = ffsq_clamp_carrier(FFSQ_MODE_FSQ, carrier);
        int basetone = (int)ceil(1.0 * (frequency - bandwidth / 2) * FSQ_SYMLEN / sr);
        int tx_basetone = (int)ceil((1500.0 - bandwidth / 2) * FSQ_SYMLEN / sr);
        int incr = basetone % FSQ_SPACING;
        tx->basetone = tx_basetone - incr;
    } else {
        double bandwidth = ffsq_bw(FFSQ_MODE_IFKP);
        double frequency = ffsq_clamp_carrier(FFSQ_MODE_IFKP, carrier);
        tx->basetone = (int)ceil((frequency - bandwidth / 2.0) * FSQ_SYMLEN / sr);
    }
}

ffsq_tx *ffsq_tx_create(const ffsq_tx_config *cfg)
{
    ffsq_tx *tx;
    if (!cfg || !ffsq_speed_ok(cfg->mode, cfg->speed))
        return NULL;
    if (cfg->mycall && strlen(cfg->mycall) > FSQ_MAX_CALL)
        return NULL;
    tx = (ffsq_tx *)calloc(1, sizeof *tx);
    if (!tx)
        return NULL;
    tx->mode = cfg->mode;
    tx->speed = cfg->speed;
    tx->samplerate = ffsq_sample_rate(cfg->mode);
    tx->symlen = ffsq_symlen(cfg->mode, cfg->speed);
    if (cfg->mycall)
        strcpy(tx->mycall, cfg->mycall);
    tx->directed = cfg->directed;
    set_tonebase(tx, cfg->carrier_hz, cfg->startup_tonebase);
    /* tx_init, fsq.cxx:246-255 / ifkp.cxx:196-205 */
    tx->tone = tx->prevtone = 0;
    tx->txphase = 0;
    return tx;
}

void ffsq_tx_destroy(ffsq_tx *tx)
{
    free(tx);
}

size_t ffsq_tx_symlen(const ffsq_tx *tx)
{
    return tx ? (size_t)tx->symlen : 0;
}

size_t ffsq_tx_bound(const ffsq_tx *tx)
{
    if (!tx)
        return 0;
    if (tx->mode == FFSQ_MODE_FSQ)
        return (size_t)FSQ_MAX_HEADER_SYMBOLS * (size_t)tx->symlen + FSQ_FLUSH;
    return 2 * (size_t)tx->symlen;
}

double ffsq_tx_tone_hz(const ffsq_tx *tx, int tone)
{
    /* (tx_basetone + tone * spacing) * samplerate / FSQ_SYMLEN in int (fsq.cxx:1349,
     * ifkp.cxx:692): whole Hz, quirk Q4 */
    if (!tx)
        return 0.0;
    return (double)((tx->basetone + tone * FSQ_SPACING) * tx->samplerate / FSQ_SYMLEN);
}

/* send_tone, fsq.cxx:1342-1365 / ifkp.cxx:687-709 */
static size_t send_tone(ffsq_tx *tx, int tone, double *out)
{
    double phaseincr, freq;
    int i;
    freq = ffsq_tx_tone_hz(tx, tone);
    phaseincr = 2.0 * M_PI * freq / tx->samplerate;
    tx->prevtone = tone;
    for (i = 0; i < tx->symlen; i++) {
        out[i] = cos(tx->txphase);
        tx->txphase -= phaseincr;
        if (tx->txphase < 0)
            tx->txphase += FSQ_TWOPI;
    }
    return (size_t)tx->symlen;
}

/* send_symbol, fsq.cxx:1367-1372 / ifkp.cxx:711-715: tone = (prevtone + sym + 1) % 33 */
static size_t send_symbol(ffsq_tx *tx, int sym, double *out)
{
    tx->tone = (tx->prevtone + sym + 1) % 33;
    return send_tone(tx, tx->tone, out);
}

/* send_idle, fsq.cxx:1374-1378 (28, 30) / ifkp.cxx:717-720 (0) */
static size_t send_idle(ffsq_tx *tx, double *out)
{
    size_t k;
    if (tx->mode == FFSQ_MODE_IFKP)
        return send_symbol(tx, 0, out);
    k = send_symbol(tx, 28, out);
    return k + send_symbol(tx, 30, out + k);
}

int ffsq_tx_symbols(const ffsq_tx *tx, unsigned char c)
{
    const signed char (*code)[2];
    if (!tx)
        return 0;
    if (c == 0)
        return tx->mode == FFSQ_MODE_FSQ ? 2 : 1;
    code = tx->mode == FFSQ_MODE_FSQ ? ffsq_fsq_varicode : ffsq_ifkp_varicode;
    return code[c][1] > 28 ? 2 : 1;
}

/* send_char, fsq.cxx:1382-1397 / ifkp.cxx:722-733 */
static size_t send_char(ffsq_tx *tx, unsigned char ch, double *out)
{
    const signed char (*code)[2];
    int sym1, sym2;
    size_t k;
    if (!ch)
        return send_idle(tx, out);
    code = tx->mode == FFSQ_MODE_FSQ ? ffsq_fsq_varicode : ffsq_ifkp_varicode;
    sym1 = code[ch][0];
    sym2 = code[ch][1];
    k = send_symbol(tx, sym1, out);
    if (sym2 > 28)
        k += send_symbol(tx, sym2, out + k);
    return k;
}

static size_t send_string(ffsq_tx *tx, const char *s, double *out)
{
    size_t k = 0;
    for (; *s; s++)
        k += send_char(tx, (unsigned char)*s, out + k);
    return k;
}

static size_t string_symbols(const ffsq_tx *tx, const char *s)
{
    size_t n = 0;
    for (; *s; s++)
        n += (size_t)ffsq_tx_symbols(tx, (unsigned char)*s);
    return n;
}

size_t ffsq_tx_begin(ffsq_tx *tx, double *out, size_t max)
{
    if (!tx || !out)
        return 0;
    if (tx->mode == FFSQ_MODE_FSQ) {
        /* fsq.cxx:1501-1508: " " + FSQBOL + mycall + ":" (+ crc.sval(mycall) if directed) */
        char send[4 + FSQ_MAX_CALL + 4];
        strcpy(send, "  \n");
        strcat(send, tx->mycall);
        strcat(send, ":");
        if (tx->directed) {
            char crc[3];
            ffsq_crc8_hex(tx->mycall, crc);
            strcat(send, crc);
        }
        if (string_symbols(tx, send) * (size_t)tx->symlen > max)
            return 0;
        return send_string(tx, send, out);
    }
    /* ifkp.cxx:1060-1064: send_char(0) twice */
    if (2 * (size_t)tx->symlen > max)
        return 0;
    return send_char(tx, 0, out) + send_char(tx, 0, out + tx->symlen);
}

size_t ffsq_tx_put(ffsq_tx *tx, unsigned char c, double *out, size_t max)
{
    if (!tx || !out || (size_t)ffsq_tx_symbols(tx, c) * (size_t)tx->symlen > max)
        return 0;
    return send_char(tx, c, out);
}

size_t ffsq_tx_idle(ffsq_tx *tx, double *out, size_t max)
{
    return ffsq_tx_put(tx, 0, out, max);
}

size_t ffsq_tx_end(ffsq_tx *tx, double *out, size_t max)
{
    if (!tx || !out)
        return 0;
    if (tx->mode == FFSQ_MODE_FSQ) {
        /* fsq.cxx:1511-1523: FSQEOT "  \b  " when directed, else FSQEOL "\n ", then
         * flush_buffer: 64 zero samples (fsq.cxx:1333-1338) */
        const char *tail = tx->directed ? "  \b  " : "\n ";
        size_t k;
        if (string_symbols(tx, tail) * (size_t)tx->symlen + FSQ_FLUSH > max)
            return 0;
        k = send_string(tx, tail, out);
        memset(out + k, 0, FSQ_FLUSH * sizeof(double));
        return k + FSQ_FLUSH;
    }
    /* ifkp.cxx:1095-1101: send_char(0) */
    return ffsq_tx_put(tx, 0, out, max);
}
