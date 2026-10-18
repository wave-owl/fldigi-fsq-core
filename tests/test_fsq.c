/*
 * fldigi-fsq-core unit tests: parameters, CRC-8 (vectors from fldigi's own crc8.h), TX
 * framing, loopback TX -> RX for every speed of both modes, chunk invariance, reset,
 * squelch, image headers, carrier offsets, API edge cases.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _POSIX_C_SOURCE 200809L

#include "test_util.h"

static const char *golden = "tests/golden";

static const double fsq_speeds[5] = {1.5, 2.0, 3.0, 4.5, 6.0};
static const double ifkp_speeds[3] = {0.0, 1.0, 2.0};

/* Every printable ASCII byte both varicodes send and both receivers print, LF included. */
static void printable(char *s)
{
    int c, k = 0;
    for (c = 32; c < 127; c++)
        s[k++] = (char)c;
    s[k++] = '\n';
    s[k++] = 'k';
    s[k] = 0;
}

static void test_params(void)
{
    const char *v = ffsq_version();
    CHECK(strstr(v, "contract=fsq-c1") != NULL, "version %s", v);
    CHECK(strstr(v, "abi=1") != NULL && strstr(v, "tx=1") != NULL, "version %s", v);
    CHECK(strstr(v, "dsp=0.6.1") != NULL, "version %s", v);
    CHECK(ffsq_abi() == FFSQ_ABI && FFSQ_ABI == 1, "abi");
    CHECK(ffsq_sample_rate(FFSQ_MODE_FSQ) == 12000, "fsq rate");
    CHECK(ffsq_sample_rate(FFSQ_MODE_IFKP) == 16000, "ifkp rate");
    CHECK(ffsq_sample_rate(7) == 0, "bad mode rate");
    CHECK(ffsq_bandwidth(FFSQ_MODE_FSQ) == 290, "fsq bw %d", ffsq_bandwidth(FFSQ_MODE_FSQ));
    CHECK(ffsq_bandwidth(FFSQ_MODE_IFKP) == 386, "ifkp bw %d", ffsq_bandwidth(FFSQ_MODE_IFKP));
    CHECK(ffsq_symlen(FFSQ_MODE_FSQ, 1.5) == 8192 && ffsq_symlen(FFSQ_MODE_FSQ, 2) == 6144 &&
          ffsq_symlen(FFSQ_MODE_FSQ, 3) == 4096 && ffsq_symlen(FFSQ_MODE_FSQ, 4.5) == 3072 &&
          ffsq_symlen(FFSQ_MODE_FSQ, 6) == 2048, "fsq symlen");
    CHECK(ffsq_symlen(FFSQ_MODE_IFKP, 0) == 8192 && ffsq_symlen(FFSQ_MODE_IFKP, 1) == 4096 &&
          ffsq_symlen(FFSQ_MODE_IFKP, 2) == 2048, "ifkp symlen");
    CHECK(ffsq_symlen(FFSQ_MODE_FSQ, 5) == 0 && ffsq_symlen(FFSQ_MODE_IFKP, 3) == 0,
          "bad speed symlen");
}

static void test_crc(void)
{
    char path[512], *buf, *line, *save = NULL, out[3];
    size_t len;
    int n = 0;
    snprintf(path, sizeof path, "%s/crc8.txt", golden);
    buf = (char *)read_file(path, &len);
    CHECK(buf != NULL, "missing %s", path);
    for (line = buf ? strtok_r(buf, "\n", &save) : NULL; line;
         line = strtok_r(NULL, "\n", &save)) {
        char *tab = strchr(line, '\t');
        if (!tab)
            continue;
        *tab = 0;
        ffsq_crc8_hex(tab + 1, out);
        CHECK(strcmp(out, line) == 0, "crc8(%s) = %s, fldigi %s", tab + 1, out, line);
        n++;
    }
    CHECK(n == 16, "%d crc vectors", n);
    ffsq_crc8_hex("", out);
    CHECK(strcmp(out, "00") == 0, "crc8 of empty string");
    free(buf);
}

/* One over decoded by the core: the bytes of the events with flags `want`. */
static unsigned char *loop(const ffsq_tx_config *c, const char *text, double rx_carrier,
                           size_t chunk, int want, size_t *np)
{
    size_t n, total, i;
    double *audio = render_cfg(c, text, 0, &n), *x;
    int sr = ffsq_sample_rate(c->mode);
    ffsq_rx *rx = make_rx(c->mode, c->speed, rx_carrier);
    unsigned char *out;
    total = n + 4 * (size_t)sr;
    x = (double *)calloc(total, sizeof(double));
    for (i = 0; i < n; i++)
        x[(size_t)sr + i] = 0.25 * audio[i];
    out = decode_flags(rx, x, total, chunk, want, np);
    ffsq_rx_destroy(rx);
    free(x);
    free(audio);
    return out;
}

static void test_loopback(void)
{
    char text[200], text2x[200], *eq;
    int i;
    printable(text);
    strcpy(text2x, text);
    eq = strchr(text2x, '=');
    memmove(eq, eq + 1, strlen(eq));
    for (i = 0; i < 5; i++) {
        ffsq_tx_config c;
        size_t np;
        unsigned char *out;
        char want[256];
        tx_config(&c, FFSQ_MODE_FSQ, fsq_speeds[i], "dg6fl", 0);
        out = loop(&c, text, 1500.0, 512, FFSQ_EV_SHOWN, &np);
        /* BOT " \n", header "dg6fl:", the text, EOL "\n" (the last space of EOL needs a
         * following symbol to be decoded, as in fldigi) */
        snprintf(want, sizeof want, " \ndg6fl:%s\n", text);
        CHECK(contains(out, np, want), "FSQ %.1f baud loopback: %.*s", fsq_speeds[i], (int)np,
              out);
        free(out);
        /* directed: header with CRC, EOT */
        tx_config(&c, FFSQ_MODE_FSQ, fsq_speeds[i], "dl1abc", 1);
        out = loop(&c, "allcall hello", 1500.0, 512, FFSQ_EV_TEXT, &np);
        {
            char crc[3];
            ffsq_crc8_hex("dl1abc", crc);
            snprintf(want, sizeof want, " \ndl1abc:%sallcall hello  \b", crc);
        }
        CHECK(contains(out, np, want), "FSQ %.1f directed: %.*s", fsq_speeds[i], (int)np, out);
        free(out);
    }
    for (i = 0; i < 3; i++) {
        ffsq_tx_config c;
        size_t np;
        unsigned char *out;
        tx_config(&c, FFSQ_MODE_IFKP, ifkp_speeds[i], NULL, 0);
        /* At 2x fldigi's receiver loses a symbol whose tone returns to the tone before the
         * previous one within 2048 samples, as '=' = {0, 31} does (the fldigi bench drops
         * it too, quirk Q11); the 2x loopback leaves '=' out. */
        out = loop(&c, i == 2 ? text2x : text, 1500.0, 512, FFSQ_EV_SHOWN, &np);
        CHECK(contains(out, np, i == 2 ? text2x : text), "IFKP %g loopback: %.*s",
              ifkp_speeds[i], (int)np, out);
        free(out);
        /* IFKP at another carrier, received there */
        c.carrier_hz = 1000.0;
        out = loop(&c, "de dg6fl k", 1000.0, 512, FFSQ_EV_SHOWN, &np);
        CHECK(contains(out, np, "de dg6fl k"), "IFKP %g at 1000 Hz: %.*s", ifkp_speeds[i],
              (int)np, out);
        free(out);
    }
}

static void test_crc_header_matches(void)
{
    /* AC-880.2: the TX header carries exactly ffsq_crc8_hex of mycall, and the receiver
     * returns it in the text */
    static const char *calls[3] = {"zl1bpu", "dl1abc", "w1hkj"};
    int i;
    for (i = 0; i < 3; i++) {
        ffsq_tx_config c;
        size_t np;
        char crc[3], want[64];
        unsigned char *out;
        tx_config(&c, FFSQ_MODE_FSQ, 6.0, calls[i], 1);
        out = loop(&c, "dg6fl hi", 1500.0, 512, FFSQ_EV_TEXT, &np);
        ffsq_crc8_hex(calls[i], crc);
        snprintf(want, sizeof want, " \n%s:%sdg6fl hi  \b", calls[i], crc);
        CHECK(contains(out, np, want), "header %s in %.*s", want, (int)np, out);
        free(out);
    }
}

static void test_flags(void)
{
    ffsq_tx_config c;
    size_t n, total, i, k;
    double *audio, *x;
    ffsq_rx *rx;
    ffsq_event ev[2048];
    int bot = 0, eot = 0, eol = 0;
    tx_config(&c, FFSQ_MODE_FSQ, 6.0, "dg6fl", 1);
    audio = render_cfg(&c, "dg6xx test", 0, &n);
    total = n + 4 * 12000;
    x = (double *)calloc(total, sizeof(double));
    for (i = 0; i < n; i++)
        x[12000 + i] = 0.25 * audio[i];
    rx = make_rx(FFSQ_MODE_FSQ, 6.0, 1500.0);
    k = ffsq_rx_process(rx, x, total, ev, 2048);
    for (i = 0; i < k; i++) {
        bot += (ev[i].flags & FFSQ_EV_BOT) != 0;
        eol += (ev[i].flags & FFSQ_EV_EOL) != 0;
        if (ev[i].flags & FFSQ_EV_EOT) {
            eot++;
            CHECK(ev[i].ch == '\b' && (ev[i].flags & FFSQ_EV_TEXT), "EOT on the backspace");
        }
    }
    CHECK(bot == 1 && eot == 1 && eol == 0, "bot %d eol %d eot %d", bot, eol, eot);
    CHECK(ffsq_rx_dropped(rx) == 0, "no drops");
    /* too small an event buffer: drops are counted */
    ffsq_rx_reset(rx);
    k = ffsq_rx_process(rx, x, total, ev, 3);
    CHECK(k == 3 && ffsq_rx_dropped(rx) > 0, "drops counted: %zu %zu", k, ffsq_rx_dropped(rx));
    ffsq_rx_destroy(rx);
    free(x);
    free(audio);
}

static void test_chunks_and_reset(void)
{
    int mode;
    for (mode = 0; mode < 2; mode++) {
        size_t n, total, i, n1, n2, n3, n4;
        double *audio = render(mode, mode ? 2.0 : 6.0, "chunk test 123", &n), *x;
        int sr = ffsq_sample_rate(mode);
        ffsq_rx *rx = make_rx(mode, mode ? 2.0 : 6.0, 1500.0);
        unsigned char *a, *b, *c, *d;
        total = n + 3 * (size_t)sr;
        x = (double *)calloc(total, sizeof(double));
        for (i = 0; i < n; i++)
            x[(size_t)sr + i] = 0.25 * audio[i];
        a = decode(rx, x, total, 1, &n1);
        ffsq_rx_reset(rx);
        b = decode(rx, x, total, 333, &n2);
        ffsq_rx_reset(rx);
        c = decode(rx, x, total, total, &n3);
        ffsq_rx_destroy(rx);
        rx = make_rx(mode, mode ? 2.0 : 6.0, 1500.0);
        d = decode(rx, x, total, 4096, &n4);
        CHECK(n1 == n2 && n2 == n3 && n3 == n4 && memcmp(a, b, n1) == 0 &&
              memcmp(b, c, n1) == 0 && memcmp(c, d, n1) == 0, "mode %d chunking/reset", mode);
        CHECK(contains(a, n1, "chunk test 123"), "mode %d text: %.*s", mode, (int)n1, a);
        free(a);
        free(b);
        free(c);
        free(d);
        ffsq_rx_destroy(rx);
        free(x);
        free(audio);
    }
}

static uint32_t lcg = 12345;
static double gauss(void)
{
    double z = 0;
    int k;
    for (k = 0; k < 12; k++) {
        lcg = lcg * 1664525u + 1013904223u;
        z += lcg / 4294967296.0;
    }
    return z - 6.0;
}

static void test_squelch_and_noise(void)
{
    /* Squelch (level 30) above the noise metric: 20 s of noise print nothing, a signal at
     * -6 dB in 2500 Hz (FSQ; IFKP at +3 dB) still comes through. */
    int mode;
    for (mode = 0; mode < 2; mode++) {
        int sr = ffsq_sample_rate(mode);
        double level = 30.0;
        size_t total = 20 * (size_t)sr, i, np, n;
        double *x = (double *)malloc(total * sizeof(double)), *audio, ps = 0, sd;
        ffsq_rx_config c;
        ffsq_rx *rx;
        unsigned char *out;
        memset(&c, 0, sizeof c);
        c.mode = mode;
        c.speed = mode ? 1.0 : 3.0;
        c.squelch_on = 1;
        c.squelch = level;
        for (i = 0; i < total; i++)
            x[i] = 0.1 * gauss();
        rx = ffsq_rx_create(&c);
        out = decode(rx, x, total, 512, &np);
        CHECK(np == 0, "mode %d: %zu bytes from noise with squelch %.0f", mode, np, level);
        free(out);
        /* without squelch fldigi prints noise; so does the port */
        ffsq_rx_set_squelch(rx, 0, 0.0);
        ffsq_rx_reset(rx);
        out = decode(rx, x, total, 512, &np);
        CHECK(np > 0, "mode %d: noise without squelch printed nothing", mode);
        free(out);
        /* a signal through the same squelch */
        audio = render(mode, mode ? 1.0 : 3.0, "squelch open test", &n);
        for (i = 0; i < n; i++)
            ps += audio[i] * audio[i];
        ps /= (double)n;
        sd = sqrt(ps / pow(10, (mode ? 3.0 : -6.0) / 10.0) * (sr / 2.0) / 2500.0);
        for (i = 0; i < total; i++)
            x[i] = 0.05 * ((i >= (size_t)sr && i < (size_t)sr + n ? audio[i - sr] : 0.0) +
                           sd * gauss());
        ffsq_rx_set_squelch(rx, 1, level);
        ffsq_rx_reset(rx);
        out = decode(rx, x, total, 512, &np);
        /* the IFKP s/n average needs a few symbols to rise above the squelch */
        CHECK(contains(out, np, mode ? "open test" : "squelch open test"),
              "mode %d with squelch: %.*s", mode,
              (int)np, out);
        free(out);
        free(audio);
        ffsq_rx_destroy(rx);
        free(x);
    }
}

static void test_image_header(void)
{
    /* AC-880.3: an IFKP image header is flagged once, the text after it is decoded */
    ffsq_tx_config c;
    size_t n, total, i, k, np = 0;
    double *audio, *x;
    ffsq_rx *rx;
    ffsq_event ev[1024];
    unsigned char text[256];
    int images = 0;
    tx_config(&c, FFSQ_MODE_IFKP, 2.0, NULL, 0);
    audio = render_cfg(&c, "pic%L more text after", 0, &n);
    total = n + 3 * 16000;
    x = (double *)calloc(total, sizeof(double));
    for (i = 0; i < n; i++)
        x[16000 + i] = 0.25 * audio[i];
    rx = make_rx(FFSQ_MODE_IFKP, 2.0, 1500.0);
    k = ffsq_rx_process(rx, x, total, ev, 1024);
    for (i = 0; i < k; i++) {
        if (ev[i].flags & FFSQ_EV_IMAGE) {
            images++;
            CHECK(ev[i].ch == 'L', "image flag on %c", ev[i].ch);
        }
        if ((ev[i].flags & FFSQ_EV_SHOWN) && np < sizeof text)
            text[np++] = ev[i].ch;
    }
    CHECK(images == 1, "%d image flags", images);
    CHECK(contains(text, np, "more text after"), "text after the image header: %.*s",
          (int)np, text);
    ffsq_rx_destroy(rx);
    free(x);
    free(audio);
}

static void test_tx_api(void)
{
    ffsq_tx_config c;
    ffsq_tx *tx;
    double *buf;
    size_t bound, k;
    tx_config(&c, FFSQ_MODE_FSQ, 3.0, "dg6fl", 0);
    tx = ffsq_tx_create(&c);
    CHECK(tx != NULL, "tx create");
    bound = ffsq_tx_bound(tx);
    buf = (double *)malloc(bound * sizeof(double));
    CHECK(ffsq_tx_symlen(tx) == 4096, "symlen");
    CHECK(ffsq_tx_begin(tx, buf, 100) == 0, "begin all or nothing");
    k = ffsq_tx_begin(tx, buf, bound);
    /* " " 1 + " " 1 + "\n" 1 + dg6fl 6 ('6' takes two) + ":" 2 */
    CHECK(k == 11 * 4096, "begin %zu", k / 4096);
    CHECK(ffsq_tx_put(tx, 'A', buf, 4096) == 0, "two-symbol char needs two symbols");
    CHECK(ffsq_tx_put(tx, 'A', buf, bound) == 2 * 4096, "A");
    CHECK(ffsq_tx_put(tx, 'a', buf, bound) == 4096, "a");
    CHECK(ffsq_tx_symbols(tx, 'a') == 1 && ffsq_tx_symbols(tx, 'A') == 2 &&
          ffsq_tx_symbols(tx, 0) == 2, "symbol counts");
    CHECK(ffsq_tx_idle(tx, buf, bound) == 2 * 4096, "fsq idle 28,30");
    CHECK(ffsq_tx_end(tx, buf, bound) == 2 * 4096 + 64, "EOL + flush");
    CHECK(buf[2 * 4096] == 0.0 && buf[2 * 4096 + 63] == 0.0, "flush zeros");
    /* tone frequencies: centred tone set after a retune, whole Hz (quirk Q4) */
    CHECK(ffsq_tx_tone_hz(tx, 0) == 1353.0 && ffsq_tx_tone_hz(tx, 32) == 1634.0,
          "fsq tones %g %g", ffsq_tx_tone_hz(tx, 0), ffsq_tx_tone_hz(tx, 32));
    ffsq_tx_destroy(tx);
    c.startup_tonebase = 1;
    tx = ffsq_tx_create(&c);
    CHECK(ffsq_tx_tone_hz(tx, 0) == 1494.0, "fsq start-up tone base %g",
          ffsq_tx_tone_hz(tx, 0));
    ffsq_tx_destroy(tx);
    c.mycall = "abcdefghijklmnopqrstu";
    CHECK(ffsq_tx_create(&c) == NULL, "call over 20 bytes");
    c.mycall = "dg6fl";
    c.speed = 5.0;
    CHECK(ffsq_tx_create(&c) == NULL, "bad speed");
    tx_config(&c, FFSQ_MODE_IFKP, 0.0, NULL, 0);
    tx = ffsq_tx_create(&c);
    CHECK(ffsq_tx_symlen(tx) == 8192, "ifkp 0.5x symlen");
    CHECK(ffsq_tx_begin(tx, buf, bound) == 2 * 8192, "ifkp begin: two idles");
    CHECK(ffsq_tx_end(tx, buf, bound) == 8192, "ifkp end: one idle");
    CHECK(ffsq_tx_symbols(tx, ' ') == 1 && ffsq_tx_symbols(tx, '\n') == 2, "ifkp space/LF");
    CHECK(ffsq_tx_tone_hz(tx, 0) == 1308.0, "ifkp tone 0 %g", ffsq_tx_tone_hz(tx, 0));
    ffsq_tx_destroy(tx);
    free(buf);
}

static void test_rx_api(void)
{
    ffsq_rx_config c;
    ffsq_rx *rx;
    memset(&c, 0, sizeof c);
    c.mode = FFSQ_MODE_FSQ;
    c.speed = 4.0;
    CHECK(ffsq_rx_create(&c) == NULL, "bad FSQ speed");
    c.mode = 5;
    c.speed = 3.0;
    CHECK(ffsq_rx_create(&c) == NULL, "bad mode");
    CHECK(ffsq_rx_create(NULL) == NULL, "NULL config");
    c.mode = FFSQ_MODE_FSQ;
    rx = ffsq_rx_create(&c);
    CHECK(rx && ffsq_rx_carrier_hz(rx) == 1500.0, "default carrier");
    ffsq_rx_set_carrier(rx, 5000.0);
    CHECK(ffsq_rx_carrier_hz(rx) == 2855.0, "fsq carrier clamp high %g", ffsq_rx_carrier_hz(rx));
    ffsq_rx_set_carrier(rx, 100.0);
    CHECK(ffsq_rx_carrier_hz(rx) == 210.0, "fsq carrier clamp low %g", ffsq_rx_carrier_hz(rx));
    CHECK(ffsq_rx_footprint(rx) < 400000, "fsq footprint %zu", ffsq_rx_footprint(rx));
    CHECK(ffsq_rx_process(rx, NULL, 0, NULL, 0) == 0, "empty process");
    ffsq_rx_destroy(rx);
    ffsq_rx_destroy(NULL);
    c.mode = FFSQ_MODE_IFKP;
    c.speed = 1.0;
    c.carrier_hz = 50.0;
    rx = ffsq_rx_create(&c);
    CHECK(rx && ffsq_rx_carrier_hz(rx) == 293.0, "ifkp clamp low %g", ffsq_rx_carrier_hz(rx));
    ffsq_rx_set_carrier(rx, 4000.0);
    CHECK(ffsq_rx_carrier_hz(rx) == 3707.0, "ifkp clamp high %g", ffsq_rx_carrier_hz(rx));
    CHECK(ffsq_rx_footprint(rx) < 400000, "ifkp footprint %zu", ffsq_rx_footprint(rx));
    ffsq_rx_destroy(rx);
}

static void test_offset(void)
{
    /* a 10 Hz carrier offset between TX and RX decodes (no AFC in either modem; the bins
     * are 2.9 / 3.9 Hz wide and the tones 3 bins apart) */
    ffsq_tx_config c;
    size_t np;
    unsigned char *out;
    tx_config(&c, FFSQ_MODE_IFKP, 1.0, NULL, 0);
    c.carrier_hz = 1510.0;
    out = loop(&c, "offset test", 1500.0, 512, FFSQ_EV_SHOWN, &np);
    CHECK(contains(out, np, "offset test"), "IFKP +10 Hz: %.*s", (int)np, out);
    free(out);
    tx_config(&c, FFSQ_MODE_FSQ, 3.0, "dg6fl", 0);
    out = loop(&c, "offset test", 1490.0, 512, FFSQ_EV_SHOWN, &np);
    CHECK(contains(out, np, "offset test"), "FSQ -10 Hz: %.*s", (int)np, out);
    free(out);
}

int main(int argc, char **argv)
{
    if (argc > 1)
        golden = argv[1];
    test_params();
    test_crc();
    test_loopback();
    test_crc_header_matches();
    test_flags();
    test_chunks_and_reset();
    test_squelch_and_noise();
    test_image_header();
    test_tx_api();
    test_rx_api();
    test_offset();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("fldigi-fsq-core: all unit tests passed (%s)\n", ffsq_version());
    return 0;
}
