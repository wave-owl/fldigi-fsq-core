/*
 * fldigi-fsq-core receiver: fsq::rx_process and ifkp::rx_process of fldigi 4.2.13 with their
 * tone and symbol processing (src/fsq/fsq.cxx, src/ifkp/ifkp.cxx).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "fsq_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TAP_LEN 1024
/* Lowest FSQ carrier whose first examined bin is not below the FFT buffer: fldigi reads
 * fft_data[i + firstbin] with firstbin = frequency * 4096 / 12000 - 72 (fsq.cxx:1137), which
 * is negative below 208 Hz although modem::set_freq allows 145 Hz (quirk Q10). */
#define FSQ_MIN_CARRIER 210.0

struct ffsq_rx {
    int mode;
    double speed;
    int samplerate;
    int numbins;
    double frequency;
    double bandwidth;
    int movavg_size;
    int peak_hits;
    int squelch_on;
    double squelch;

    double rx_stream[FSQ_BLOCK_SIZE + FSQ_SHIFT_SIZE];
    fdsp_cmpx fft_data[FSQ_FFTSIZE];
    double a_blackman[FSQ_BLOCK_SIZE];
    double tones[FSQ_MAXBINS];
    fdsp_movavgh binfilt[FSQ_MAXBINS];
    fdsp_gfft fft;
    fdsp_movavgh snfilt;
    fdsp_movavgh sigfilt;           /* FSQ: reset at EOL/EOT, otherwise unused (fsq.cxx:178) */
    fdsp_movavgh noisefilt;         /* FSQ: likewise (fsq.cxx:177) */
    fdsp_fir rxfilter;              /* IFKP: 129-tap band pass (ifkp.cxx:138-139, 277) */
    int bkptr;
    double max;
    int peak;
    int prev_peak;
    int last_peak;
    int peak_counter;
    int symbol;
    int prev_symbol;
    int curr_nibble;
    int prev_nibble;
    int nibbles[199];
    double s2n;
    double metric;
    int ch_sqlch_open;
    int no_signal;                  /* IFKP: a global of ifkp.cxx:482 (quirk Q8) */
    char lfpair[3];                 /* FSQ lf_check statics (fsq.cxx:1006-1007, quirk Q8) */
    char bstrng[4];
    char pic_str[6];                /* IFKP image header window (ifkp.cxx:228) */

    ffsq_event *ev;
    size_t ev_max;
    size_t ev_n;
    size_t dropped;
    int tap[TAP_LEN];
    size_t tap_n;
    size_t footprint;
};

static void emit(ffsq_rx *rx, int ch, int flags)
{
    if (rx->ev_n < rx->ev_max) {
        rx->ev[rx->ev_n].ch = (unsigned char)ch;
        rx->ev[rx->ev_n].flags = (unsigned char)flags;
        rx->ev_n++;
    } else {
        rx->dropped++;
    }
}

/* ---------------------------------------------------------------------------------- */
/* FSQ                                                                                */
/* ---------------------------------------------------------------------------------- */

static int fsq_squelch_open(const ffsq_rx *rx)
{
    /* fsq.cxx:410-415 (state == IMAGE never holds: images are not received, D-886) */
    return rx->ch_sqlch_open || rx->metric >= rx->squelch;
}

/* lf_check, fsq.cxx:1004-1027: BOT " \n", EOL "\n ", EOT "  \b" over the last bytes. */
static int fsq_lf_check(ffsq_rx *rx, int ch)
{
    rx->lfpair[0] = rx->lfpair[1];
    rx->lfpair[1] = (char)(0xFF & ch);
    rx->bstrng[0] = rx->bstrng[1];
    rx->bstrng[1] = rx->bstrng[2];
    rx->bstrng[2] = (char)(0xFF & ch);
    if (rx->bstrng[0] == ' ' && rx->bstrng[1] == ' ' && rx->bstrng[2] == '\b')
        return FFSQ_EV_EOT;
    if (rx->lfpair[0] == ' ' && rx->lfpair[1] == '\n')
        return FFSQ_EV_BOT;
    if (rx->lfpair[0] == '\n' && rx->lfpair[1] == ' ')
        return FFSQ_EV_EOL;
    return 0;
}

/* process_symbol, fsq.cxx:1029-1125, without the FSQCALL parse (parse_rx_text stays with the
 * caller, wave-owl D-881/D-883) and without the GUI and log output. */
static void fsq_process_symbol(ffsq_rx *rx, int sym)
{
    int nibble, curr_ch = -1;

    rx->symbol = sym;
    nibble = rx->symbol - rx->prev_symbol;
    if (nibble < -99 || nibble > 99) {
        rx->prev_symbol = rx->symbol;
        return;
    }
    nibble = rx->nibbles[nibble + 99];

    /* -1 is the idle symbol */
    if (nibble >= 0) {
        rx->curr_nibble = nibble;
        if ((rx->prev_nibble < 29) & (rx->curr_nibble < 29))
            curr_ch = ffsq_fsq_varidecode[rx->prev_nibble];
        else if (rx->prev_nibble < 29 && rx->curr_nibble > 28 && rx->curr_nibble < 32)
            curr_ch = ffsq_fsq_varidecode[rx->prev_nibble * 32 + rx->curr_nibble];
        if (curr_ch > 0) {
            int lf = fsq_lf_check(rx, curr_ch);
            int flags = lf;
            int b_bot = lf == FFSQ_EV_BOT, b_eol = lf == FFSQ_EV_EOL, b_eot = lf == FFSQ_EV_EOT;

            if (b_bot)
                rx->ch_sqlch_open = 1;      /* and rx_text.clear() */

            if (fsq_squelch_open(rx)) {
                flags |= FFSQ_EV_SHOWN;     /* write_rx_mon_char */
                if (b_eol || b_eot) {
                    fdsp_movavgh_reset(&rx->noisefilt);
                    fdsp_movavgh_run(&rx->noisefilt, 1);
                    fdsp_movavgh_reset(&rx->sigfilt);
                    fdsp_movavgh_run(&rx->sigfilt, 1);
                }
            }

            if (ffsq_valid_char(curr_ch) || b_eol || b_eot) {
                if (fsq_squelch_open(rx) || !rx->squelch_on) {
                    flags |= FFSQ_EV_TEXT;  /* rx_text += curr_ch */
                    if (b_eot) {
                        /* parse_rx_text() here; the state stays TEXT */
                        rx->ch_sqlch_open = 0;
                        rx->metric = 0;
                    }
                }
            }
            if (fsq_squelch_open(rx) && (b_eot || b_eol)) {
                rx->ch_sqlch_open = 0;
                rx->metric = 0;
            }
            emit(rx, curr_ch, flags);
        }
        rx->prev_nibble = rx->curr_nibble;
    }
    rx->prev_symbol = rx->symbol;
}

static double fsq_norm(fdsp_cmpx z)
{
    /* std::norm, re * re + im * im, with the multiply-add the reference build fuses */
    return fma(z.re, z.re, z.im * z.im);
}

static void tap_symbol(ffsq_rx *rx, int peak)
{
    rx->tap[rx->tap_n % TAP_LEN] = peak;
    rx->tap_n++;
}

/* process_tones, fsq.cxx:1131-1196 */
static void fsq_process_tones(ffsq_rx *rx)
{
    int i, firstbin, minbin;
    double sigval, min, val;
    const int nb = FSQ_NUMBINS;

    rx->max = 0;
    rx->peak = nb / 2;
    firstbin = (int)(rx->frequency * FSQ_SYMLEN / rx->samplerate - nb / 2);
    min = 3.0e8;
    minbin = nb / 2;

    for (i = 0; i < nb; ++i) {
        val = fsq_norm(rx->fft_data[i + firstbin]);
        rx->tones[i] = fdsp_movavgh_run(&rx->binfilt[i], val);
        if (rx->tones[i] > rx->max) {
            rx->max = rx->tones[i];
            rx->peak = i;
        }
        if (rx->tones[i] < min) {
            min = rx->tones[i];
            minbin = i;
        }
    }

    sigval = rx->tones[(rx->peak - 1) < 0 ? (nb - 1) : (rx->peak - 1)] +
             rx->tones[rx->peak] +
             rx->tones[(rx->peak + 1) == nb ? 0 : (rx->peak + 1)];

    min = rx->tones[(minbin - 1) < 0 ? (nb - 1) : (minbin - 1)] +
          rx->tones[minbin] +
          rx->tones[(minbin + 1) == nb ? 0 : (minbin + 1)];

    if (min == 0)
        min = 1e-10;

    /* 10 * log10(snfilt->run(sigval / min)) - 34.0 + movavg_size / 4 */
    rx->s2n = fma(10, log10(fdsp_movavgh_run(&rx->snfilt, sigval / min)), -34.0) +
              rx->movavg_size / 4;

    if (rx->s2n <= 0)
        rx->metric = 2 * (25 + rx->s2n);
    if (rx->s2n > 0)
        rx->metric = 50 * (1 + rx->s2n / 45);
    rx->metric = rx->metric < 0 ? 0 : rx->metric > 100 ? 100 : rx->metric;

    if (rx->metric < rx->squelch && rx->ch_sqlch_open)
        rx->ch_sqlch_open = 0;

    /* requires consecutive hits */
    if (rx->peak == rx->prev_peak)
        rx->peak_counter++;
    else
        rx->peak_counter = 0;

    if ((rx->peak_counter >= rx->peak_hits) && (rx->peak != rx->last_peak) &&
        (fsq_squelch_open(rx) || !rx->squelch_on)) {
        tap_symbol(rx, rx->peak);
        fsq_process_symbol(rx, rx->peak);
        rx->peak_counter = 0;
        rx->last_peak = rx->peak;
    }
    rx->prev_peak = rx->peak;
}

/* ---------------------------------------------------------------------------------- */
/* IFKP                                                                               */
/* ---------------------------------------------------------------------------------- */

/* parse_pic, ifkp.cxx:377-421: 1 when the window now holds an image header. */
static int ifkp_parse_pic(ffsq_rx *rx, int ch)
{
    memmove(rx->pic_str, rx->pic_str + 1, 4);
    rx->pic_str[4] = (char)ch;
    rx->pic_str[5] = 0;
    if (memcmp(rx->pic_str, "pic%", 4) != 0)
        return 0;
    return strchr("ATtSsLlVvFPpMm", rx->pic_str[4]) != NULL && rx->pic_str[4] != 0;
}

/* process_symbol, ifkp.cxx:423-480, without the GUI, the logs and the heard list
 * (valid_callsign stays with the caller). */
static void ifkp_process_symbol(ffsq_rx *rx, int sym)
{
    int nibble, curr_ch = -1;

    rx->symbol = sym;
    nibble = rx->symbol - rx->prev_symbol;
    if (nibble < -99 || nibble > 99) {
        rx->prev_symbol = rx->symbol;
        return;
    }
    nibble = rx->nibbles[nibble + 99];

    if (nibble >= 0) {
        rx->curr_nibble = nibble;
        if ((rx->prev_nibble < 29) & (rx->curr_nibble < 29))
            curr_ch = ffsq_ifkp_varidecode[rx->prev_nibble];
        else if (rx->prev_nibble < 29 && rx->curr_nibble > 28 && rx->curr_nibble < 32)
            curr_ch = ffsq_ifkp_varidecode[rx->prev_nibble * 32 + rx->curr_nibble];
        if (curr_ch > 0) {
            int flags = 0;
            /* ch_sqlch_open is never set by ifkp (uninitialised member, quirk Q8) */
            if (rx->ch_sqlch_open || rx->metric >= rx->squelch) {
                flags = FFSQ_EV_SHOWN | FFSQ_EV_TEXT;   /* put_rx_char */
                if (ifkp_parse_pic(rx, curr_ch))
                    flags |= FFSQ_EV_IMAGE;
            }
            emit(rx, curr_ch, flags);
        }
        rx->prev_nibble = rx->curr_nibble;
    }
    rx->prev_symbol = rx->symbol;
}

/* process_tones, ifkp.cxx:484-551 */
static void ifkp_process_tones(ffsq_rx *rx)
{
    int i, firstbin;
    double sigval, min, val;
    const int nb = IFKP_NUMBINS;

    rx->max = 0;
    rx->peak = 0;
    rx->max = 0;
    rx->peak = nb / 2;

    firstbin = (int)(rx->frequency * FSQ_SYMLEN / rx->samplerate - nb / 2);
    min = 3.0e8;

    for (i = 0; i < nb; ++i) {
        val = fsq_norm(rx->fft_data[i + firstbin]);
        rx->tones[i] = fdsp_movavgh_run(&rx->binfilt[i], val);
        if (rx->tones[i] > rx->max) {
            rx->max = rx->tones[i];
            rx->peak = i;
        }
        if (rx->tones[i] < min)
            min = rx->tones[i];
    }

    if (rx->peak == rx->prev_peak)
        rx->peak_counter++;
    else
        rx->peak_counter = 0;

    if ((rx->peak_counter >= rx->peak_hits) && (rx->peak != rx->last_peak)) {
        /* tones[peak - 1] and tones[peak + 1] without wrap: fldigi reads the neighbouring
         * class members at the edges (a pointer as a denormal); 0.0 here (quirk Q7) */
        double lo = rx->peak > 0 ? rx->tones[rx->peak - 1] : 0.0;
        double hi = rx->peak < nb - 1 ? rx->tones[rx->peak + 1] : 0.0;
        sigval = (lo + rx->tones[rx->peak] + hi);
        if (min == 0)
            min = rx->max / 1000;
        if (min > 0 && rx->max > 0) {
            rx->s2n = 20 * log10(fdsp_movavgh_run(&rx->snfilt, sigval / min));
            rx->s2n -= 64;
            rx->s2n *= 40;
            rx->s2n /= 75;
            rx->s2n -= 20;
        } else {
            rx->s2n = -25;
        }

        if (rx->s2n <= 0)
            rx->metric = 2 * (25 + rx->s2n);
        if (rx->s2n > 0)
            rx->metric = 50 * (1 + rx->s2n / 45);
        rx->metric = rx->metric < 0 ? 0 : rx->metric > 100 ? 100 : rx->metric;

        if (rx->metric >= rx->squelch || !rx->squelch_on) {
            tap_symbol(rx, rx->peak);
            ifkp_process_symbol(rx, rx->peak);
        }
        rx->peak_counter = 0;
        rx->last_peak = rx->peak;
        rx->no_signal = 0;
    } else if (++rx->no_signal > 128) {
        /* reset the filter and zero the s/n display */
        fdsp_movavgh_reset(&rx->snfilt);
    }
    rx->prev_peak = rx->peak;
}

/* The IFKP receive band pass, ifkp.cxx:274-277: float corners, float division. */
static void ifkp_make_bpf(ffsq_rx *rx)
{
    fdsp_fir *f = &rx->rxfilter;
    float lo = (float)fma(-0.75, rx->bandwidth, rx->frequency);
    float hi = (float)fma(0.75, rx->bandwidth, rx->frequency);
    float sr = (float)rx->samplerate;
    fdsp_fir_bp_taps(f->ifilter, IFKP_BPF_TAPS, 0, (double)(lo / sr), (double)(hi / sr));
    memcpy(f->qfilter, f->ifilter, IFKP_BPF_TAPS * sizeof(double));
    memset(f->ibuffer, 0, sizeof f->ibuffer);
    memset(f->qbuffer, 0, sizeof f->qbuffer);
    f->pointer = IFKP_BPF_TAPS;
    f->counter = 0;
}

static void set_freq(ffsq_rx *rx, double f)
{
    rx->frequency = ffsq_clamp_carrier(rx->mode, f);
    if (rx->mode == FFSQ_MODE_FSQ) {
        if (rx->frequency < FSQ_MIN_CARRIER)
            rx->frequency = FSQ_MIN_CARRIER;
    } else {
        ifkp_make_bpf(rx);
    }
}

/* rx_init, fsq.cxx:257-284 / ifkp.cxx:207-229, plus the state the constructors set. */
static void rx_init(ffsq_rx *rx)
{
    int i;
    rx->bkptr = 0;
    rx->peak_counter = 0;
    rx->peak = rx->last_peak = 0;
    rx->prev_peak = 0;
    rx->symbol = rx->prev_symbol = 0;     /* uninitialised in fldigi (quirk Q8) */
    rx->max = 0;
    rx->curr_nibble = rx->prev_nibble = 0;
    rx->s2n = 0;
    rx->ch_sqlch_open = 0;
    rx->metric = 0;
    rx->no_signal = 0;
    memset(rx->rx_stream, 0, sizeof rx->rx_stream);
    memset(rx->fft_data, 0, sizeof rx->fft_data);
    for (i = 0; i < rx->numbins; i++) {
        rx->tones[i] = 0.0;
        fdsp_movavgh_reset(&rx->binfilt[i]);
    }
    fdsp_movavgh_reset(&rx->snfilt);
    fdsp_movavgh_reset(&rx->sigfilt);
    fdsp_movavgh_reset(&rx->noisefilt);
    memcpy(rx->lfpair, "01", 3);
    memcpy(rx->bstrng, "012", 4);
    memcpy(rx->pic_str, "     ", 6);
    rx->tap_n = 0;
}

ffsq_rx *ffsq_rx_create(const ffsq_rx_config *cfg)
{
    ffsq_rx *rx;
    int i, ok = 1;
    if (!cfg || !ffsq_speed_ok(cfg->mode, cfg->speed))
        return NULL;
    rx = (ffsq_rx *)calloc(1, sizeof *rx);
    if (!rx)
        return NULL;
    rx->footprint = sizeof *rx;
    rx->mode = cfg->mode;
    rx->speed = cfg->speed;
    rx->samplerate = ffsq_sample_rate(cfg->mode);
    rx->bandwidth = ffsq_bw(cfg->mode);
    rx->squelch_on = cfg->squelch_on;
    rx->squelch = cfg->squelch;
    if (rx->mode == FFSQ_MODE_FSQ) {
        /* fsq.cxx:181-184, 380-391: fsq_movavg clamped to 1 .. MOVAVGLIMIT, fsqhits */
        rx->numbins = FSQ_NUMBINS;
        rx->movavg_size = cfg->movavg > 0 ? cfg->movavg : 4;
        if (rx->movavg_size < 1)
            rx->movavg_size = 1;
        if (rx->movavg_size > FSQ_MOVAVGLIMIT)
            rx->movavg_size = FSQ_MOVAVGLIMIT;
        rx->peak_hits = cfg->peak_hits > 0 ? cfg->peak_hits : 3;
    } else {
        /* ifkp::init, ifkp.cxx:241-254: peak_hits 4, movavg 3 (after restart's 3 or 4) */
        rx->numbins = IFKP_NUMBINS;
        rx->movavg_size = cfg->movavg > 0 ? cfg->movavg : 3;
        if (rx->movavg_size > FSQ_MOVAVGLIMIT)
            rx->movavg_size = FSQ_MOVAVGLIMIT;
        rx->peak_hits = cfg->peak_hits > 0 ? cfg->peak_hits : 4;
    }
    ok &= fdsp_gfft_init(&rx->fft, FSQ_FFTSIZE) == 0;
    rx->footprint += (FSQ_FFTSIZE / 4 + 1) * sizeof(double) + FSQ_FFTSIZE * sizeof(short);
    for (i = 0; i < rx->numbins; i++) {
        ok &= fdsp_movavgh_init(&rx->binfilt[i], rx->movavg_size) == 0;
        rx->footprint += (size_t)rx->movavg_size * sizeof(double);
    }
    /* snfilt 64 (FSQ, fsq.cxx:175) or 32 (IFKP, ifkp.cxx:129); noisefilt 32, sigfilt 8 */
    ok &= fdsp_movavgh_init(&rx->snfilt, rx->mode == FFSQ_MODE_FSQ ? FSQ_SQLFILT_SIZE : 32) == 0;
    ok &= fdsp_movavgh_init(&rx->noisefilt, 32) == 0;
    ok &= fdsp_movavgh_init(&rx->sigfilt, 8) == 0;
    rx->footprint += (64 + 32 + 8) * sizeof(double);
    if (rx->mode == FFSQ_MODE_IFKP) {
        double *taps = (double *)calloc(IFKP_BPF_TAPS, sizeof(double));
        ok &= taps != NULL && fdsp_fir_init(&rx->rxfilter, IFKP_BPF_TAPS, 1, taps, taps) == 0;
        free(taps);
        rx->footprint += 2 * IFKP_BPF_TAPS * sizeof(double);
    }
    if (!ok) {
        ffsq_rx_destroy(rx);
        return NULL;
    }
    /* fsq.cxx:206-207, ifkp.cxx:162-163 */
    for (i = 0; i < FSQ_BLOCK_SIZE; i++)
        rx->a_blackman[i] = fdsp_blackman(1.0 * i / FSQ_BLOCK_SIZE);
    ffsq_init_nibbles(rx->nibbles);
    set_freq(rx, cfg->carrier_hz);
    rx_init(rx);
    return rx;
}

void ffsq_rx_destroy(ffsq_rx *rx)
{
    int i;
    if (!rx)
        return;
    fdsp_gfft_free(&rx->fft);
    for (i = 0; i < FSQ_MAXBINS; i++)
        fdsp_movavgh_free(&rx->binfilt[i]);
    fdsp_movavgh_free(&rx->snfilt);
    fdsp_movavgh_free(&rx->noisefilt);
    fdsp_movavgh_free(&rx->sigfilt);
    if (rx->mode == FFSQ_MODE_IFKP)
        fdsp_fir_free(&rx->rxfilter);
    free(rx);
}

void ffsq_rx_reset(ffsq_rx *rx)
{
    if (!rx)
        return;
    if (rx->mode == FFSQ_MODE_IFKP)
        ifkp_make_bpf(rx);
    rx_init(rx);
    rx->dropped = 0;
}

/* One block of SHIFT_SIZE new samples: window, FFT, tone processing (fsq.cxx:1302-1316,
 * ifkp.cxx:651-666). */
static void block(ffsq_rx *rx)
{
    int i;
    memmove(rx->rx_stream, &rx->rx_stream[FSQ_SHIFT_SIZE],
            FSQ_BLOCK_SIZE * sizeof(*rx->rx_stream));
    for (i = 0; i < FSQ_BLOCK_SIZE; i++) {
        double d = rx->rx_stream[i] * rx->a_blackman[i];
        rx->fft_data[i].re = d;
        rx->fft_data[i].im = d;
    }
    fdsp_gfft_forward(&rx->fft, rx->fft_data);
    if (rx->mode == FFSQ_MODE_FSQ)
        fsq_process_tones(rx);
    else
        ifkp_process_tones(rx);
}

size_t ffsq_rx_process(ffsq_rx *rx, const double *x, size_t n, ffsq_event *ev, size_t max_ev)
{
    size_t k;
    if (!rx)
        return 0;
    rx->ev = ev;
    rx->ev_max = ev ? max_ev : 0;
    rx->ev_n = 0;
    if (rx->bkptr < 0 || rx->bkptr >= FSQ_SHIFT_SIZE)
        rx->bkptr = 0;
    for (k = 0; k < n; k++) {
        double val = x[k];
        if (rx->mode == FFSQ_MODE_IFKP)
            fdsp_fir_irun(&rx->rxfilter, x[k], &val);
        rx->rx_stream[FSQ_BLOCK_SIZE + rx->bkptr] = val;
        rx->bkptr++;
        if (rx->bkptr == FSQ_SHIFT_SIZE) {
            rx->bkptr = 0;
            block(rx);
        }
    }
    rx->ev = NULL;
    return rx->ev_n;
}

double ffsq_rx_snr_db(const ffsq_rx *rx)
{
    return rx ? rx->s2n : 0.0;
}

double ffsq_rx_metric(const ffsq_rx *rx)
{
    return rx ? rx->metric : 0.0;
}

double ffsq_rx_carrier_hz(const ffsq_rx *rx)
{
    return rx ? rx->frequency : 0.0;
}

void ffsq_rx_set_carrier(ffsq_rx *rx, double carrier_hz)
{
    if (rx)
        set_freq(rx, carrier_hz);
}

void ffsq_rx_set_squelch(ffsq_rx *rx, int on, double level)
{
    if (!rx)
        return;
    rx->squelch_on = on;
    rx->squelch = level;
}

size_t ffsq_rx_dropped(const ffsq_rx *rx)
{
    return rx ? rx->dropped : 0;
}

size_t ffsq_rx_footprint(const ffsq_rx *rx)
{
    return rx ? rx->footprint : 0;
}

size_t ffsq_rx_take_symbols(ffsq_rx *rx, int *out, size_t max)
{
    size_t have, first, i;
    if (!rx || !out)
        return 0;
    have = rx->tap_n < TAP_LEN ? rx->tap_n : TAP_LEN;
    if (have > max)
        have = max;
    first = rx->tap_n - have;
    for (i = 0; i < have; i++)
        out[i] = rx->tap[(first + i) % TAP_LEN];
    rx->tap_n = 0;
    return have;
}
