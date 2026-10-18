/*
 * fldigi-fsq-core internals: constants and tables shared by the receiver and the transmitter
 * (fldigi 4.2.13 src/include/fsq.h, src/include/ifkp.h, src/fsq/fsq.cxx, src/ifkp/ifkp.cxx).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FSQ_INTERNAL_H
#define FSQ_INTERNAL_H

#include "fldigi_fsq.h"
#include "fldigi_dsp.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define FSQ_TWOPI (2.0 * M_PI)          /* TWOPI, modem.h:45 */

#define FSQ_SR 12000                    /* SR, fsq.h:41 */
#define IFKP_SR 16000                   /* IFKP_SR, ifkp.cxx:58 */
#define FSQ_FFTSIZE 4096                /* FFTSIZE, fsq.h:42; IFKP_FFTSIZE, ifkp.h:37 */
#define FSQ_SYMLEN 4096                 /* FSQ_SYMLEN, fsq.h:43; IFKP_SYMLEN, ifkp.h:38 */
#define FSQ_BLOCK_SIZE FSQ_FFTSIZE      /* BLOCK_SIZE, fsq.h:47 */
#define FSQ_SHIFT_SIZE (FSQ_SYMLEN / 16)    /* SHIFT_SIZE, fsq.h:48 */
#define FSQ_NUMBINS 144                 /* NUMBINS, fsq.h:45 */
#define IFKP_NUMBINS 151                /* IFKP_NUMBINS, ifkp.h:47 */
#define FSQ_MAXBINS IFKP_NUMBINS
#define FSQ_MOVAVGLIMIT 15              /* MOVAVGLIMIT, fsq.h:50 */
#define FSQ_SPACING 3                   /* spacing, fsq.cxx:185; IFKP_SPACING, ifkp.h:45 */
#define FSQ_SQLFILT_SIZE 64             /* SQLFILT_SIZE, fsq.cxx:66 */
#define IFKP_BPF_TAPS 129               /* ifkp.cxx:139, 277 */
#define FSQ_FLUSH 64                    /* flush_buffer, fsq.cxx:1335 */
#define FSQ_MAX_CALL 20                 /* valid_callsign length limit, fsq.cxx:434 */
/* fldigi's waterfall limits LowFreqCutoff 0 / HighFreqCutoff 3000 (configuration.h:204-209)
 * bound the FSQ carrier in modem::set_freq (modem.cxx:319-324). */
#define FSQ_LOW_CUTOFF 0
#define FSQ_HIGH_CUTOFF 3000

extern const signed char ffsq_fsq_varicode[256][2];
extern const short ffsq_fsq_varidecode[29 * 32];
extern const signed char ffsq_ifkp_varicode[256][2];
extern const short ffsq_ifkp_varidecode[29 * 32];

/* Tone difference (+99) -> symbol, -1 for the idle step (fsq.cxx:116-128 = ifkp.cxx:100-112). */
void ffsq_init_nibbles(int nibbles[199]);

/* 1 for the speeds fldigi offers (FSQ 1.5/2/3/4.5/6, IFKP 0/1/2). */
int ffsq_speed_ok(int mode, double speed);
/* fldigi's bandwidth member as a double (fsq.cxx:260, ifkp.cxx:270). */
double ffsq_bw(int mode);
double ffsq_clamp_carrier(int mode, double f);
/* valid_char, fsq.cxx:397-404 = ifkp.cxx:364-371 */
int ffsq_valid_char(int ch);

#endif
