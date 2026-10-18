/*
 * fldigi-fsq-core CPU and memory benchmark: receiver and transmitter per mode and speed,
 * CPU time per second of audio, and the receiver's allocation.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _POSIX_C_SOURCE 200809L
#include <time.h>

#include "test_util.h"

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void bench(int mode, double speed, const char *name)
{
    const char *text = "cq cq de dg6fl dg6fl k the quick brown fox 0123456789";
    int sr = ffsq_sample_rate(mode);
    size_t n, i, rounds = 0;
    double *audio, t0, t_tx, t_rx, secs;
    ffsq_rx *rx;
    ffsq_event ev[1024];

    t0 = now();
    audio = render(mode, speed, text, &n);
    t_tx = now() - t0;
    secs = (double)n / sr;
    for (i = 0; i < n; i++)
        audio[i] *= 0.25;
    rx = make_rx(mode, speed, 1500.0);
    t0 = now();
    while (now() - t0 < 0.5 || rounds < 2) {
        size_t off;
        for (off = 0; off < n; off += 512)
            ffsq_rx_process(rx, audio + off, n - off < 512 ? n - off : 512, ev, 1024);
        rounds++;
    }
    t_rx = (now() - t0) / (double)rounds;
    printf("%-9s RX %6.3f %% of one core (%7.4f s per %5.1f s audio), TX %6.3f %%, "
           "receiver %zu bytes\n", name, 100.0 * t_rx / secs, t_rx, secs, 100.0 * t_tx / secs,
           ffsq_rx_footprint(rx));
    ffsq_rx_destroy(rx);
    free(audio);
}

int main(void)
{
    printf("%s\n", ffsq_version());
    bench(FFSQ_MODE_FSQ, 1.5, "FSQ-1.5");
    bench(FFSQ_MODE_FSQ, 2.0, "FSQ-2");
    bench(FFSQ_MODE_FSQ, 3.0, "FSQ-3");
    bench(FFSQ_MODE_FSQ, 4.5, "FSQ-4.5");
    bench(FFSQ_MODE_FSQ, 6.0, "FSQ-6");
    bench(FFSQ_MODE_IFKP, 0.0, "IFKP-0.5");
    bench(FFSQ_MODE_IFKP, 1.0, "IFKP-1");
    bench(FFSQ_MODE_IFKP, 2.0, "IFKP-2");
    return 0;
}
