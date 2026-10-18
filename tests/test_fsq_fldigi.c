/*
 * fldigi-fsq-core against fldigi 4.2.13 itself (goldens from tests/golden/make_fldigi_golden.py,
 * the headless fldigi bench build).
 *
 * TX, every FSQ speed and IFKP speed, CQ and pangram overs, directed and allcall FSQ frames,
 * the FSQ start-up tone base and an IFKP carrier offset: the port renders fldigi's TX vector
 * (FNV-1a hash of the int16 output identical on the reference platform, at most 1 LSB from
 * the stored vectors everywhere).
 * RX, the stored overs clean and at -3 ... -12 dB (2500 Hz), FSQ received 10 Hz off, and
 * 10 s of pure noise: the printed bytes equal fldigi's own on the reference platform;
 * elsewhere (different libm / FMA use) the sent text must be recovered with at most
 * 1 + 5 % more errors than fldigi's own decode, and wherever fldigi decoded the text
 * completely, so must the port.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _POSIX_C_SOURCE 200809L

#include "test_util.h"

static const char *golden = "tests/golden";

static uint64_t fnv1a64(const int16_t *pcm, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ULL;
    size_t i;
    for (i = 0; i < n; i++) {
        uint16_t u = (uint16_t)pcm[i];
        h ^= (uint8_t)(u & 0xFF);
        h *= 0x100000001B3ULL;
        h ^= (uint8_t)(u >> 8);
        h *= 0x100000001B3ULL;
    }
    return h;
}

/* Same as noisy() in make_fldigi_golden.py; this file is compiled without FP contraction,
 * so every step rounds like Python. */
static uint32_t lcg;
static double lcg_uniform(void)
{
    lcg = lcg * 1664525u + 1013904223u;
    return lcg / 4294967296.0;
}

static const char *snr_names[5] = {"clean", "m03", "m06", "m09", "m12"};
static const double snr_k[5] = {0.0, 0.50118723362727224, 0.25118864315095796,
                                0.12589254117941673, 0.063095734448019331};

static double *noisy(const int16_t *pcm, size_t n, int sr, int snr_step, uint32_t seed,
                     size_t *out_n)
{
    size_t lead = (size_t)sr, tail = 3 * (size_t)sr, total = lead + n + tail, i;
    double *x = (double *)calloc(total, sizeof(double));
    double ps = 0, s2, sd, gain, k = snr_k[snr_step];
    int j;
    for (i = 0; i < n; i++)
        x[lead + i] = (double)pcm[i];
    *out_n = total;
    if (snr_step == 0) {
        for (i = 0; i < total; i++)
            x[i] /= 32768.0;
        return x;
    }
    for (i = 0; i < n; i++)
        ps += (double)pcm[i] * (double)pcm[i];
    ps = ps / (double)n;
    s2 = ps * k * (sr / 2.0) / 2500.0;
    sd = sqrt(s2);
    gain = 3276.7 / sqrt(ps * (double)n / (double)total + s2);
    lcg = seed;
    for (i = 0; i < total; i++) {
        double z = 0.0, y, q;
        for (j = 0; j < 12; j++)
            z += lcg_uniform();
        z -= 6.0;
        y = (x[i] + sd * z) * gain;
        q = floor(y + 0.5);
        if (q > 32767)
            q = 32767;
        if (q < -32768)
            q = -32768;
        x[i] = q / 32768.0;  /* libsndfile sf_readf_double: int16 / 32768 */
    }
    return x;
}

static double *noise_only(int sr, int seconds, uint32_t seed, size_t *out_n)
{
    size_t total = (size_t)seconds * (size_t)sr, i;
    double *x = (double *)malloc(total * sizeof(double));
    int j;
    lcg = seed;
    for (i = 0; i < total; i++) {
        double z = 0.0, q;
        for (j = 0; j < 12; j++)
            z += lcg_uniform();
        z -= 6.0;
        q = floor(z * 3276.7 + 0.5);
        if (q > 32767)
            q = 32767;
        if (q < -32768)
            q = -32768;
        x[i] = q / 32768.0;
    }
    *out_n = total;
    return x;
}

static char texts[4][160];
static const char *keys[4] = {"cq", "pangram", "directed", "allcall"};

static const char *text_for(const char *key)
{
    int i;
    for (i = 0; i < 4; i++)
        if (strcmp(key, keys[i]) == 0)
            return texts[i];
    return "";
}

static void load_texts(void)
{
    char path[512], *buf, *line;
    size_t len;
    int i;
    snprintf(path, sizeof path, "%s/texts.txt", golden);
    buf = (char *)read_file(path, &len);
    if (!buf) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(2);
    }
    for (line = strtok(buf, "\n"); line; line = strtok(NULL, "\n"))
        for (i = 0; i < 4; i++) {
            size_t k = strlen(keys[i]);
            if (strncmp(line, keys[i], k) == 0 && line[k] == ' ')
                snprintf(texts[i], sizeof texts[i], "%s", line + k + 1);
        }
    free(buf);
}

static int mode_of(const char *s)
{
    return strcmp(s, "FSQ") == 0 ? FFSQ_MODE_FSQ : FFSQ_MODE_IFKP;
}

static int tx_done, rx_exact, rx_total;
static size_t noisy_port, noisy_fldigi;
static int full_port, full_fldigi;

static void check_tx(const char *stem, const char *mode, double speed, const char *key, int sr,
                     size_t samples, int directed, int carrier, int startup, uint64_t hash)
{
    ffsq_tx_config c;
    double *audio;
    int16_t *pcm, *ref;
    size_t n, i, reflen = 0, diff1 = 0, diffbig = 0;
    char path[512];

    tx_config(&c, mode_of(mode), speed, "dg6fl", directed);
    c.carrier_hz = carrier;
    c.startup_tonebase = startup;
    CHECK(sr == ffsq_sample_rate(c.mode), "%s: rate %d", stem, sr);
    audio = render_cfg(&c, text_for(key), 0, &n);
    CHECK(audio != NULL, "%s: render", stem);
    if (!audio)
        return;
    pcm = (int16_t *)malloc((n + 1) * sizeof(int16_t));
    quantize(audio, n, pcm);
    CHECK(n == samples, "%s: %zu samples, fldigi %zu", stem, n, samples);
    if (REFERENCE_PLATFORM)
        CHECK(fnv1a64(pcm, n) == hash, "%s: TX hash %016llx, fldigi %016llx", stem,
              (unsigned long long)fnv1a64(pcm, n), (unsigned long long)hash);
    snprintf(path, sizeof path, "%s/tx/%s.s16", golden, stem);
    ref = (int16_t *)read_file(path, &reflen);
    if (ref) {
        reflen /= 2;
        CHECK(reflen == n, "%s: stored vector %zu samples, port %zu", stem, reflen, n);
        for (i = 0; i < n && i < reflen; i++) {
            int d = abs((int)pcm[i] - (int)ref[i]);
            diff1 += d == 1;
            diffbig += d > 1;
        }
        CHECK(diffbig == 0, "%s: %zu samples differ by more than 1 LSB from fldigi", stem,
              diffbig);
        if (diff1)
            printf("  %s: %zu samples differ by 1 LSB from fldigi\n", stem, diff1);
    }
    tx_done++;
    free(ref);
    free(pcm);
    free(audio);
}

static void compare_rx(const char *what, const unsigned char *out, size_t got,
                       const unsigned char *want, size_t glen, const char *text, int clean)
{
    int exact = got == glen && memcmp(out, want, glen) == 0;
    size_t ns = strlen(text);
    rx_total++;
    rx_exact += exact;
    if (REFERENCE_PLATFORM) {
        CHECK(exact, "%s: RX differs from fldigi", what);
        if (exact)
            return;
        fprintf(stderr, "  port  : ");
        fwrite(out, 1, got, stderr);
        fprintf(stderr, "\n  fldigi: ");
        fwrite(want, 1, glen, stderr);
        fputc('\n', stderr);
        return;
    }
    if (!ns)
        return;
    {
        size_t dp = window_distance((const unsigned char *)text, ns, out, got);
        size_t df = window_distance((const unsigned char *)text, ns, want, glen);
        if (!exact)
            printf("  %s: not identical; text errors port %zu, fldigi %zu\n", what, dp, df);
        if (clean) {
            CHECK(dp <= df + 1 + ns / 20, "%s: %zu text errors, fldigi %zu", what, dp, df);
        } else {
            noisy_port += dp;
            noisy_fldigi += df;
            full_port += contains(out, got, text);
            full_fldigi += contains(want, glen, text);
        }
    }
}

static void check_rx(const char *stem, const char *src, const char *mode, double speed, int idx,
                     int carrier, const char *snrs, const char *text)
{
    int16_t *ref;
    size_t reflen = 0;
    char path[512];
    int s, m = mode_of(mode);

    snprintf(path, sizeof path, "%s/tx/%s.s16", golden, src);
    ref = (int16_t *)read_file(path, &reflen);
    CHECK(ref != NULL, "%s: missing %s", stem, path);
    if (!ref)
        return;
    reflen /= 2;
    for (s = 0; s < 5; s++) {
        ffsq_rx *rx;
        size_t nx, got, glen;
        double *x;
        unsigned char *out, *want;
        char what[128];
        if (!strstr(snrs, snr_names[s]))
            continue;
        x = noisy(ref, reflen, ffsq_sample_rate(m), s, (uint32_t)(1000 * idx + s), &nx);
        rx = make_rx(m, speed, carrier);
        CHECK(rx != NULL, "%s: rx_create", stem);
        if (!rx) {
            free(x);
            continue;
        }
        out = decode(rx, x, nx, 512, &got);
        snprintf(path, sizeof path, "%s/rx/%s_%s.txt", golden, stem, snr_names[s]);
        want = (unsigned char *)read_file(path, &glen);
        CHECK(want != NULL, "%s: missing %s", stem, path);
        snprintf(what, sizeof what, "%s %s", stem, snr_names[s]);
        if (want)
            compare_rx(what, out, got, want, glen, text, s == 0);
        free(want);
        free(out);
        free(x);
        ffsq_rx_destroy(rx);
    }
    free(ref);
}

static void check_noise(const char *stem, const char *mode, unsigned seed)
{
    int m = mode_of(mode);
    size_t nx, got, glen;
    double *x = noise_only(ffsq_sample_rate(m), 10, seed, &nx);
    ffsq_rx *rx = make_rx(m, m == FFSQ_MODE_FSQ ? 3.0 : 1.0, 1500.0);
    unsigned char *out = decode(rx, x, nx, 512, &got), *want;
    char path[512];
    snprintf(path, sizeof path, "%s/rx/%s.txt", golden, stem);
    want = (unsigned char *)read_file(path, &glen);
    CHECK(want != NULL, "missing %s", path);
    if (want && REFERENCE_PLATFORM)
        compare_rx(stem, out, got, want, glen, "", 0);
    free(want);
    free(out);
    free(x);
    ffsq_rx_destroy(rx);
}

int main(int argc, char **argv)
{
    char path[512], *buf, *line, *save = NULL;
    size_t len;
    int rx_cases = 0;

    if (argc > 1)
        golden = argv[1];
    load_texts();
    snprintf(path, sizeof path, "%s/tx_hashes.txt", golden);
    buf = (char *)read_file(path, &len);
    if (!buf) {
        fprintf(stderr, "missing goldens in %s\n", golden);
        return 2;
    }
    for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char stem[64], mode[8], key[16];
        int idx, sr, directed, carrier, startup;
        double speed;
        size_t samples;
        unsigned long long hash;
        if (line[0] == '#')
            continue;
        if (sscanf(line, "%63s %7s %lf %15s %d %d %zu %d %d %d %llx", stem, mode, &speed, key,
                   &idx, &sr, &samples, &directed, &carrier, &startup, &hash) != 11) {
            CHECK(0, "bad line: %s", line);
            continue;
        }
        check_tx(stem, mode, speed, key, sr, samples, directed, carrier, startup,
                 (uint64_t)hash);
    }
    free(buf);

    snprintf(path, sizeof path, "%s/rx_cases.txt", golden);
    buf = (char *)read_file(path, &len);
    CHECK(buf != NULL, "missing %s", path);
    save = NULL;
    for (line = buf ? strtok_r(buf, "\n", &save) : NULL; line;
         line = strtok_r(NULL, "\n", &save)) {
        char stem[64], src[64], mode[8], snrs[64], key[16];
        int idx, carrier;
        double speed;
        const char *p;
        if (line[0] == '#')
            continue;
        if (sscanf(line, "%63s %63s %7s %lf %d %d %63s", stem, src, mode, &speed, &idx,
                   &carrier, snrs) != 7) {
            CHECK(0, "bad line: %s", line);
            continue;
        }
        /* the text key is the last part of the source stem */
        p = strrchr(src, '_');
        snprintf(key, sizeof key, "%s", p ? p + 1 : "cq");
        if (strcmp(key, "startup") == 0 || strncmp(key, "tx", 2) == 0)
            snprintf(key, sizeof key, "cq");
        check_rx(stem, src, mode, speed, idx, carrier, snrs,
                 strcmp(key, "startup") == 0 ? "" : text_for(key));
        rx_cases++;
    }
    free(buf);

    check_noise("fsq_noise", "FSQ", 77);
    check_noise("ifkp_noise", "IFKP", 78);

    CHECK(tx_done == 20, "%d TX vectors", tx_done);
    CHECK(rx_cases == 14, "%d RX cases", rx_cases);
    if (!REFERENCE_PLATFORM) {
        printf("noisy decodes: text errors port %zu, fldigi %zu; complete texts port %d, "
               "fldigi %d\n", noisy_port, noisy_fldigi, full_port, full_fldigi);
        CHECK(noisy_port <= noisy_fldigi + noisy_fldigi / 5 + 10,
              "noisy decodes: %zu text errors, fldigi %zu", noisy_port, noisy_fldigi);
        CHECK(full_port + 3 >= full_fldigi, "complete texts: port %d, fldigi %d", full_port,
              full_fldigi);
    }
    printf("fldigi-fsq-core vs fldigi 4.2.13: %d TX vectors, RX identical in %d of %d decodes%s\n",
           tx_done, rx_exact, rx_total, REFERENCE_PLATFORM ? " (reference platform)" : "");
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    return 0;
}
