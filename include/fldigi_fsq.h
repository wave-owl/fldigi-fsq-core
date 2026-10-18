/*
 * fldigi-fsq-core -- the FSQ and IFKP modems of fldigi 4.2.13 (FSQ 1.5, 2, 3, 4.5 and 6 baud;
 * IFKP 0.5x, 1x and 2x speed), receiver and transmitter, ported to C99.
 *
 * Copyright (C) 2026 Gerolf Ziegenhain
 * Ported from fldigi 4.2.13 (https://github.com/wave-owl/fldigi, tag v4.2.13, commit
 * e7c3ab3709ad62a7d91829364eb48a05fa325c0d): src/fsq/fsq.cxx, src/fsq/fsq_varicode.cxx,
 * src/include/fsq.h, src/ifkp/ifkp.cxx, src/ifkp/ifkp_varicode.cxx, src/include/ifkp.h,
 * src/include/crc8.h and the carrier clamp of src/trx/modem.cxx; (c) 2015 Dave Freese
 * W1HKJ and the fldigi authors. FSQ and IFKP were designed by Murray Greenman ZL1BPU.
 *
 * This program is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this
 * program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef FLDIGI_FSQ_H
#define FLDIGI_FSQ_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FFSQ_VERSION_STRING "0.1.0"
#define FFSQ_ABI 1

/* "fldigi-fsq-core 0.1.0 contract=fsq-c1 abi=1 tx=1 dsp=0.6.1". Static string. The contract
 * tag changes only with an incompatible API change; tx=1 announces the transmitter. */
const char *ffsq_version(void);
int ffsq_abi(void);                         /* FFSQ_ABI */

/* The two modems of one core (wave-owl D-880). */
#define FFSQ_MODE_FSQ 0
#define FFSQ_MODE_IFKP 1

/* Modem rate: FSQ 12000 Hz (fsq.h:41), IFKP 16000 Hz (ifkp.cxx:58); 0 for another mode. */
int ffsq_sample_rate(int mode);
/* Occupied span of the 33 tones as fldigi computes its bandwidth, in whole Hz
 * (33 * 3 * rate / 4096 in integer arithmetic): FSQ 290 (fsq.cxx:260), IFKP 386
 * (ifkp.cxx:270); 0 for another mode. */
int ffsq_bandwidth(int mode);
/* Samples per transmitted symbol: FSQ 1.5/2/3/4.5/6 baud -> 8192/6144/4096/3072/2048
 * (fsq.cxx:322-338); IFKP speed 0/1/2 (0.5x/1x/2x) -> 8192/4096/2048 (ifkp.cxx:699-701).
 * 0 for an unknown mode or speed. */
int ffsq_symlen(int mode, double speed);

/* CRC-8 (polynomial 0x07, initial value 0) of s as two lower-case hex digits plus NUL,
 * crc8.h:10-35. fldigi indexes its table with a signed char; bytes above 127 are taken as
 * unsigned here (quirk Q9). FSQ directed headers carry it after "mycall:". */
void ffsq_crc8_hex(const char *s, char out[3]);

/* ------------------------------------------------------------------------------------
 * Receiver: fsq::rx_process / ifkp::rx_process (fsq.cxx:1263-1320, ifkp.cxx:627-674): a
 * 4096-point Blackman-windowed FFT every 256 samples, a moving average per bin, the loudest
 * bin held for peak_hits blocks is a symbol, the tone difference gives a nibble, two nibbles
 * a character of the varicode.
 * ------------------------------------------------------------------------------------ */
typedef struct ffsq_rx_config {
    int mode;               /* FFSQ_MODE_FSQ or FFSQ_MODE_IFKP */
    double speed;           /* FSQ: 1.5, 2, 3, 4.5 or 6 (only checked; the FSQ receiver is
                             * speed independent, fsq.cxx:1131-1196); IFKP: 0, 1 or 2 (not
                             * used by the receiver after init, quirk Q5) */
    double carrier_hz;      /* audio centre (fldigi "frequency"), 0 = 1500 */
    int movavg;             /* bin moving average: 0 = fldigi's default (FSQ fsq_movavg 4,
                             * clamped 1..15; IFKP 3, ifkp::init, quirk Q5) */
    int peak_hits;          /* blocks a peak must hold: 0 = default (FSQ fsqhits 3, IFKP 4) */
    int squelch_on;         /* fldigi's progStatus.sqlonoff (0 in the bench) */
    double squelch;         /* fldigi's squelch level in metric units 0..100 (0 in the bench) */
} ffsq_rx_config;

/* Event flags of one decoded character. */
#define FFSQ_EV_BOT 0x01    /* FSQ: " \n" seen (lf_check, fsq.cxx:1004-1027) */
#define FFSQ_EV_EOL 0x02    /* FSQ: "\n " seen */
#define FFSQ_EV_EOT 0x04    /* FSQ: "  \b" seen */
#define FFSQ_EV_SHOWN 0x08  /* fldigi prints it: FSQ write_rx_mon_char, IFKP put_rx_char */
#define FFSQ_EV_TEXT 0x10   /* FSQ: appended to fldigi's rx_text, the FSQCALL parser input
                             * (fsq.cxx:1103-1115); IFKP: same as SHOWN */
#define FFSQ_EV_IMAGE 0x20  /* IFKP: this byte completes an image header "pic%X"
                             * (ifkp.cxx:377-421); images are not supported, text decoding
                             * goes on (wave-owl D-886) */

typedef struct ffsq_event {
    unsigned char ch;       /* the decoded byte (fldigi's varicode value, 1..255) */
    unsigned char flags;    /* FFSQ_EV_* */
} ffsq_event;

typedef struct ffsq_rx ffsq_rx;

/* NULL for a bad mode or speed, or out of memory. The carrier is clamped as fldigi does
 * (FSQ: modem::set_freq to 145 .. 2855 Hz; IFKP: 293 .. 3707 Hz, ifkp.cxx:263-264). */
ffsq_rx *ffsq_rx_create(const ffsq_rx_config *cfg);
void ffsq_rx_destroy(ffsq_rx *rx);
/* Back to the state after create. */
void ffsq_rx_reset(ffsq_rx *rx);
/* Feed n samples at the modem rate (fldigi's sound card scale: int16 / 32768). One event per
 * decoded character (fldigi's curr_ch > 0) goes to ev; returns their number. Events beyond
 * max_ev are dropped and counted (ffsq_rx_dropped). ev may be NULL with max_ev 0. */
size_t ffsq_rx_process(ffsq_rx *rx, const double *x, size_t n, ffsq_event *ev,
                       size_t max_ev);
/* fldigi's s/n estimate in dB (FSQ fsq.cxx:1169, every block; IFKP ifkp.cxx:519-526, at
 * every symbol) and the squelch metric 0..100 derived from it. */
double ffsq_rx_snr_db(const ffsq_rx *rx);
double ffsq_rx_metric(const ffsq_rx *rx);
double ffsq_rx_carrier_hz(const ffsq_rx *rx);
/* Retune (clamped as at create, filters re-made for IFKP); the bin averages keep running. */
void ffsq_rx_set_carrier(ffsq_rx *rx, double carrier_hz);
void ffsq_rx_set_squelch(ffsq_rx *rx, int on, double level);
size_t ffsq_rx_dropped(const ffsq_rx *rx);
/* Bytes the receiver allocated (all at create). */
size_t ffsq_rx_footprint(const ffsq_rx *rx);
/* Test tap: the peak bin of every symbol handed to process_symbol since the last call,
 * oldest first, at most the last 1024. Returns the number copied. */
size_t ffsq_rx_take_symbols(ffsq_rx *rx, int *out, size_t max);

/* ------------------------------------------------------------------------------------
 * Transmitter: fsq::tx_process (fsq.cxx:1497-1536) / ifkp::tx_process (ifkp.cxx:1056-1104),
 * one call per state step. Output: the samples fldigi hands to modem::ModulateXmtr
 * (cos of a phase accumulator, peak 1.0, no shaping; fsq.cxx:1342-1365, ifkp.cxx:687-709),
 * before the TX level (multiply by 10^(txlevel/20), fldigi's -12 dB, to reproduce fldigi's
 * WAV). Every call writes all of its samples or none (0 when max is too small).
 * ------------------------------------------------------------------------------------ */
typedef struct ffsq_tx_config {
    int mode;
    double speed;           /* FSQ: 1.5 .. 6 baud; IFKP: 0, 1 or 2 */
    double carrier_hz;      /* 0 = 1500. IFKP transmits on it. FSQ always transmits around
                             * 1500 Hz (modem::get_txfreq, modem.cxx:349); the value only
                             * enters the tone-base rounding (fsq.cxx:301-305) */
    const char *mycall;     /* FSQ: the header call, at most 20 bytes (sent as given) */
    int directed;           /* FSQ: 1 appends the CRC-8 of mycall to the header
                             * (progdefaults.fsq_directed) */
    int startup_tonebase;   /* FSQ: 1 = fldigi's tone base before the first retune, computed
                             * with bandwidth 0 (+48 bins above centre, quirk Q3); 0 = after a
                             * retune (the centred tone set) */
} ffsq_tx_config;

typedef struct ffsq_tx ffsq_tx;

ffsq_tx *ffsq_tx_create(const ffsq_tx_config *cfg);   /* NULL as rx_create, or mycall > 20 */
void ffsq_tx_destroy(ffsq_tx *tx);
size_t ffsq_tx_symlen(const ffsq_tx *tx);            /* samples per symbol */
size_t ffsq_tx_bound(const ffsq_tx *tx);             /* largest output of one call */
/* Start of an over: FSQ " " + " \n" + mycall + ":" (+ CRC when directed), fsq.cxx:1501-1508;
 * IFKP two idle symbols, ifkp.cxx:1060-1064. */
size_t ffsq_tx_begin(ffsq_tx *tx, double *out, size_t max);
/* One character (send_char, fsq.cxx:1382-1397 / ifkp.cxx:722-733): one or two symbols;
 * NUL sends the idle (FSQ symbols 28, 30; IFKP symbol 0); bytes without a code send the
 * symbols of table entry {0, 0} (FSQ: space; IFKP: idle). */
size_t ffsq_tx_put(ffsq_tx *tx, unsigned char c, double *out, size_t max);
/* The idle: FSQ send_idle (fsq.cxx:1374-1378, two symbols); IFKP send_idle (ifkp.cxx:
 * 717-720, symbol 0, what fldigi sends with an empty TX buffer). */
size_t ffsq_tx_idle(ffsq_tx *tx, double *out, size_t max);
/* End of an over: FSQ "  \b  " (directed) or "\n " and 64 zero samples (fsq.cxx:1511-1523,
 * 1333-1338); IFKP one idle symbol (ifkp.cxx:1095-1101). */
size_t ffsq_tx_end(ffsq_tx *tx, double *out, size_t max);
/* Symbols a character takes (1 or 2) without sending it. */
int ffsq_tx_symbols(const ffsq_tx *tx, unsigned char c);
/* Audio frequency of tone 0 .. 32 in Hz (integer, as fldigi computes it, quirk Q4). */
double ffsq_tx_tone_hz(const ffsq_tx *tx, int tone);

#ifdef __cplusplus
}
#endif

#endif
