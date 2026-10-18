/*
 * fldigi-fsq-core test helpers (header-only).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FSQ_TEST_UTIL_H
#define FSQ_TEST_UTIL_H

#include "fldigi_fsq.h"

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures __attribute__((unused));

#define CHECK(cond, ...)                                                                 \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            failures++;                                                                  \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                         \
            fprintf(stderr, __VA_ARGS__);                                                \
            fputc('\n', stderr);                                                         \
        }                                                                                \
    } while (0)

/* The platform the goldens were made on (fldigi 4.2.13 bench, macOS arm64, Apple clang -O2
 * with its default FP contraction): there the port must reproduce fldigi bit for bit. */
#if defined(__APPLE__) && defined(__aarch64__) && !defined(FSQ_NOT_REFERENCE)
#define REFERENCE_PLATFORM 1
#else
#define REFERENCE_PLATFORM 0
#endif

static inline void *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *buf;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)n + 1);
    if (!buf || (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n)) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    buf[n] = 0;
    *len = (size_t)n;
    return buf;
}

typedef struct {
    double *x;
    size_t n, cap;
} sbuf;

static inline void sbuf_add(sbuf *b, const double *x, size_t n)
{
    if (n == 0)
        return;
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 1024;
        b->x = (double *)realloc(b->x, b->cap * sizeof(double));
    }
    memcpy(b->x + b->n, x, n * sizeof(double));
    b->n += n;
}

/* A whole over as fldigi's bench sends it: begin, the text, idles, end. Returns a malloc'ed
 * buffer of *n samples (peak 1.0, before fldigi's TX level). */
static inline double *render_cfg(const ffsq_tx_config *c, const char *text, int idle,
                                 size_t *n)
{
    ffsq_tx *tx = ffsq_tx_create(c);
    sbuf b = {NULL, 0, 0};
    double *tmp;
    size_t bound, k, i;
    if (!tx) {
        *n = 0;
        return NULL;
    }
    bound = ffsq_tx_bound(tx);
    tmp = (double *)malloc(bound * sizeof(double));
    k = ffsq_tx_begin(tx, tmp, bound);
    sbuf_add(&b, tmp, k);
    for (i = 0; text[i]; i++) {
        k = ffsq_tx_put(tx, (unsigned char)text[i], tmp, bound);
        sbuf_add(&b, tmp, k);
    }
    for (i = 0; i < (size_t)idle; i++) {
        k = ffsq_tx_idle(tx, tmp, bound);
        sbuf_add(&b, tmp, k);
    }
    k = ffsq_tx_end(tx, tmp, bound);
    sbuf_add(&b, tmp, k);
    free(tmp);
    ffsq_tx_destroy(tx);
    *n = b.n;
    return b.x;
}

static inline void tx_config(ffsq_tx_config *c, int mode, double speed, const char *mycall,
                             int directed)
{
    memset(c, 0, sizeof *c);
    c->mode = mode;
    c->speed = speed;
    c->carrier_hz = 1500.0;
    c->mycall = mycall;
    c->directed = directed;
}

static inline double *render(int mode, double speed, const char *text, size_t *n)
{
    ffsq_tx_config c;
    tx_config(&c, mode, speed, "dg6fl", 0);
    return render_cfg(&c, text, 0, n);
}

/* fldigi's sound path: modem::ModulateXmtr scales by 10^(txlevel/20), txlevel -12 dB, and
 * clips at +-0.95 (trx/modem.cxx:652-658); libsndfile writes PCM16 as lrint(x * 0x7FFF). */
static inline void quantize(const double *x, size_t n, int16_t *pcm)
{
    double mult = pow(10, -12.0 / 20.0);
    size_t i;
    for (i = 0; i < n; i++) {
        double v = x[i] * mult;
        if (v < -0.95)
            v = -0.95;
        if (v > 0.95)
            v = 0.95;
        pcm[i] = (int16_t)lrint(v * 32767.0);
    }
}

static inline ffsq_rx *make_rx(int mode, double speed, double carrier)
{
    ffsq_rx_config c;
    memset(&c, 0, sizeof c);
    c.mode = mode;
    c.speed = speed;
    c.carrier_hz = carrier;
    return ffsq_rx_create(&c);
}

/* Decode x in chunks of `chunk` samples. Returns the malloc'ed bytes of the events whose
 * flags contain `want` (*np). */
static inline unsigned char *decode_flags(ffsq_rx *rx, const double *x, size_t n, size_t chunk,
                                          int want, size_t *np)
{
    unsigned char *p = (unsigned char *)malloc(n / 64 + 4096);
    ffsq_event ev[512];
    size_t off, gp = 0, k, i;
    for (off = 0; off < n; off += chunk) {
        size_t m = n - off < chunk ? n - off : chunk;
        k = ffsq_rx_process(rx, x + off, m, ev, 512);
        for (i = 0; i < k; i++)
            if (ev[i].flags & want)
                p[gp++] = ev[i].ch;
    }
    p[gp] = 0;
    *np = gp;
    return p;
}

static inline unsigned char *decode(ffsq_rx *rx, const double *x, size_t n, size_t chunk,
                                    size_t *np)
{
    return decode_flags(rx, x, n, chunk, FFSQ_EV_SHOWN, np);
}

/* Minimum edit distance between want and any substring of got. */
static inline size_t window_distance(const unsigned char *want, size_t nw,
                                     const unsigned char *got, size_t ng)
{
    size_t *prev = (size_t *)malloc((nw + 1) * sizeof(size_t));
    size_t *cur = (size_t *)malloc((nw + 1) * sizeof(size_t));
    size_t i, j, best;
    for (i = 0; i <= nw; i++)
        prev[i] = i;
    best = prev[nw];
    for (j = 1; j <= ng; j++) {
        cur[0] = 0;
        for (i = 1; i <= nw; i++) {
            size_t a = prev[i - 1] + (want[i - 1] != got[j - 1]);
            size_t b = prev[i] + 1, c = cur[i - 1] + 1;
            cur[i] = a < b ? (a < c ? a : c) : (b < c ? b : c);
        }
        if (cur[nw] < best)
            best = cur[nw];
        memcpy(prev, cur, (nw + 1) * sizeof(size_t));
    }
    free(prev);
    free(cur);
    return best;
}

static inline int contains(const unsigned char *hay, size_t n, const char *needle)
{
    size_t m = strlen(needle), i;
    for (i = 0; i + m <= n; i++)
        if (memcmp(hay + i, needle, m) == 0)
            return 1;
    return 0;
}

#endif
