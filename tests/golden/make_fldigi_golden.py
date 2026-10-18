#!/usr/bin/env python3
"""Golden data from fldigi 4.2.13 itself for fldigi-fsq-core (stdlib only).

Drives the headless fldigi bench build (configure --enable-benchmark plus the TX extension
of wave-owl's archived change fix-psk-rtty-vs-fldigi, bench/BUILD.md, and the FSQ extension
of wave-owl's change add-fldigi-fsq-ifkp-core, tests/golden/fldigi-bench-fsq.patch) for every
FSQ and IFKP speed of fldigi 4.2.13:

* TX: fldigi renders the text (bench-tx, txlevel -12 dB, PCM16, 12000 Hz FSQ / 16000 Hz
  IFKP) with the case's settings (fldigi_def.xml FSQBAUD, FSQDIRECTED, MYCALL, IFKPBAUD).
  FSQ takes the text through get_fsq_tx_char with fldigi's "^r" end marker, IFKP through
  get_tx_char. The FNV-1a 64 hash of every vector goes to tx_hashes.txt; the CQ overs are
  kept as raw little-endian int16 in tx/<stem>.s16.
* RX: fldigi receives 1 s silence + a TX vector + 3 s silence, clean and buried in
  deterministic noise at -3, -6, -9 and -12 dB (2500 Hz reference bandwidth, see noisy()),
  squelch off. Every byte fldigi prints (FSQ write_rx_mon_char, IFKP put_rx_char) goes to
  rx/<stem>_<snr>.txt. Pure noise (10 s) goes to rx/<mode>_noise.txt.

FLDIGI_BENCH_RETUNE re-applies the carrier after init, as a click on the waterfall does
(FSQ computes its TX tone base there); the case fsq_3_startup leaves it out.

    FLDIGI_BENCH_BIN=/path/to/src-bench/src/fldigi python3 tests/golden/make_fldigi_golden.py

The C tests (tests/test_fsq_fldigi.c) rebuild the same noisy inputs bit for bit.
SPDX-License-Identifier: GPL-3.0-or-later
"""

from __future__ import annotations

import array
import math
import os
import shutil
import subprocess
import sys
import tempfile
import wave
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
FLDIGI = os.environ.get("FLDIGI_BENCH_BIN", "fldigi")
TAIL_S = 3
MODE_ID = {"FSQ": 162, "IFKP": 163}   # trx_mode ids of fldigi 4.2.13 (globals.h)
RATE = {"FSQ": 12000, "IFKP": 16000}
TEXTS = {"cq": "cq cq de dg6fl dg6fl k",
         "pangram": "the quick brown fox jumps over the lazy dog THE QUICK BROWN FOX "
                    "0123456789 !\"#$%&'()*+,-./:;<=>?@[\\]_`{|}~ 73",
         "directed": "dg6xx hello from the bench",
         "allcall": "allcall net check in please"}
# 10^(-snr/10) as literals, so C uses the very same double
SNRS = (("clean", 0.0), ("m03", 0.50118723362727224), ("m06", 0.25118864315095796),
        ("m09", 0.12589254117941673), ("m12", 0.063095734448019331))
FSQ_SPEEDS = (("15", 1.5), ("2", 2.0), ("3", 3.0), ("45", 4.5), ("6", 6.0))
IFKP_SPEEDS = (("0", 0), ("1", 1), ("2", 2))


def tx_cases():
    """(stem, mode, speed, text key, directed, carrier, retune, stored)"""
    out = [(f"fsq_{s}_cq", "FSQ", v, "cq", 0, 1500, 1, True) for s, v in FSQ_SPEEDS]
    out += [(f"fsq_{s}_pangram", "FSQ", v, "pangram", 0, 1500, 1, False) for s, v in FSQ_SPEEDS]
    out += [("fsq_3_directed", "FSQ", 3.0, "directed", 1, 1500, 1, True),
            ("fsq_3_allcall", "FSQ", 3.0, "allcall", 1, 1500, 1, True),
            ("fsq_3_startup", "FSQ", 3.0, "cq", 0, 1500, 0, True)]
    out += [(f"ifkp_{s}_cq", "IFKP", v, "cq", 0, 1500, 1, True) for s, v in IFKP_SPEEDS]
    out += [(f"ifkp_{s}_pangram", "IFKP", v, "pangram", 0, 1500, 1, False)
            for s, v in IFKP_SPEEDS]
    out += [("ifkp_1_tx1510", "IFKP", 1, "cq", 0, 1510, 1, True)]
    return out


def rx_cases():
    """(stem, source tx stem, mode, speed, rx carrier, snrs)"""
    allsnr = tuple(n for n, _ in SNRS)
    out = [(c[0], c[0], c[1], c[2], 1500, allsnr) for c in tx_cases() if c[7]]
    out += [("fsq_3_cq_rx1510", "fsq_3_cq", "FSQ", 3.0, 1510, ("clean", "m06")),
            ("fsq_3_cq_rx1490", "fsq_3_cq", "FSQ", 3.0, 1490, ("clean", "m06"))]
    return out


def config_xml(**tags):
    body = "".join(f"<{k}>{v}</{k}>\n" for k, v in tags.items())
    return "<FLDIGI_DEFS>\n" + body + "</FLDIGI_DEFS>\n"


def mode_xml(mode, speed, directed=0):
    if mode == "FSQ":
        return config_xml(FSQSOUNDER=0, FSQ_ENABLE_HEARD_LOG=0, FSQ_ENABLE_AUDIT_LOG=0,
                          MYCALL="dg6fl", FSQBAUD=speed, FSQDIRECTED=directed,
                          FSQMOVAVG=4, FSQHITS=3, FSQLOWERCASE=0)
    return config_xml(IFKPBAUD=speed, IFKPFREQLOCK=0, IFKP_ENABLE_HEARD_LOG=0,
                      IFKP_ENABLE_AUDIT_LOG=0, MYCALL="dg6fl")


def run_fldigi(mode, carrier, args, xml, env_extra=None, retune=1):
    cfg = tempfile.mkdtemp(prefix="fldigi-fsq-golden-")
    Path(cfg, "fldigi_def.xml").write_text(xml)
    env = {k: v for k, v in os.environ.items() if not k.startswith("FLDIGI_BENCH_")}
    env["FLDIGI_BENCH_FSQ"] = "1"
    if retune:
        env["FLDIGI_BENCH_RETUNE"] = "1"
    env.update(env_extra or {})
    try:
        p = subprocess.run([FLDIGI, "--config-dir", cfg, "--benchmark-modem",
                            str(MODE_ID[mode]), "--benchmark-frequency", str(carrier),
                            *args], env=env, capture_output=True, text=True, timeout=3600)
    finally:
        shutil.rmtree(cfg, ignore_errors=True)
    if f"mode={mode} " not in p.stderr:
        raise RuntimeError(f"fldigi did not run {mode}: {p.stderr[-400:]}")
    return p.stderr


def read_wav(path):
    with wave.open(str(path)) as w:
        assert w.getnchannels() == 1 and w.getsampwidth() == 2
        sr = w.getframerate()
        a = array.array("h", w.readframes(w.getnframes()))
    if sys.byteorder == "big":
        a.byteswap()
    return sr, a


def write_wav(path, sr, pcm):
    a = array.array("h", pcm)
    if sys.byteorder == "big":
        a.byteswap()
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(a.tobytes())


def fnv1a64(pcm):
    h = 0xCBF29CE484222325
    for v in pcm:
        u = v & 0xFFFF
        for byte in (u & 0xFF, u >> 8):
            h ^= byte
            h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


class Lcg:
    """32-bit LCG (Numerical Recipes); the same in tests/test_fsq_fldigi.c."""

    def __init__(self, seed):
        self.s = seed & 0xFFFFFFFF

    def uniform(self):
        self.s = (self.s * 1664525 + 1013904223) & 0xFFFFFFFF
        return self.s / 4294967296.0


def noisy(pcm, sr, k, seed):
    """1 s + pcm + TAIL_S s, plus Irwin-Hall Gaussian noise; k = 10^(-SNR/10) (0: clean).

    Only +, -, *, / and sqrt (all correctly rounded), so C reproduces it bit for bit.
    SNR = mean square of the burst / (noise variance * 2500 / (sr / 2)).
    """
    padded = [0.0] * sr + [float(v) for v in pcm] + [0.0] * (TAIL_S * sr)
    if k == 0.0:
        return [int(v) for v in padded]
    ps = 0.0
    for v in pcm:
        ps += float(v) * float(v)
    ps = ps / len(pcm)
    s2 = ps * k * (sr / 2.0) / 2500.0
    sd = math.sqrt(s2)
    gain = 3276.7 / math.sqrt(ps * len(pcm) / len(padded) + s2)
    rng = Lcg(seed)
    out = []
    for v in padded:
        z = 0.0
        for _ in range(12):
            z += rng.uniform()
        z -= 6.0
        y = (v + sd * z) * gain
        q = math.floor(y + 0.5)
        out.append(max(-32768, min(32767, int(q))))
    return out


def noise_only(sr, seconds, seed):
    """seconds of Irwin-Hall noise at 0.1 RMS (3276.7 int16 units)."""
    rng = Lcg(seed)
    out = []
    for _ in range(seconds * sr):
        z = 0.0
        for _ in range(12):
            z += rng.uniform()
        z -= 6.0
        out.append(max(-32768, min(32767, int(math.floor(z * 3276.7 + 0.5)))))
    return out


def one_tx(idx, case):
    stem, mode, speed, key, directed, carrier, retune, stored = case
    xml = mode_xml(mode, speed, directed)
    with tempfile.TemporaryDirectory() as td:
        wav = Path(td, stem + ".wav")
        run_fldigi(mode, carrier, ["--benchmark-input", "1"], xml,
                   {"FLDIGI_BENCH_TX_TEXT": TEXTS[key], "FLDIGI_BENCH_TX_WAV": str(wav)},
                   retune)
        sr, pcm = read_wav(wav)
    if stored:
        raw = array.array("h", pcm)
        if sys.byteorder == "big":
            raw.byteswap()
        Path(HERE, "tx", stem + ".s16").write_bytes(raw.tobytes())
    line = (f"{stem} {mode} {speed:g} {key} {idx} {sr} {len(pcm)} {directed} {carrier} "
            f"{1 - retune} {fnv1a64(pcm):016x}")
    print(line, flush=True)
    return line


def one_rx(idx, case):
    stem, src, mode, speed, carrier, snrs = case
    raw = array.array("h", Path(HERE, "tx", src + ".s16").read_bytes())
    if sys.byteorder == "big":
        raw.byteswap()
    sr = RATE[mode]
    xml = mode_xml(mode, speed)
    with tempfile.TemporaryDirectory() as td:
        for i, (snr, k) in enumerate(SNRS):
            if snr not in snrs:
                continue
            inp = Path(td, f"{stem}_{snr}.wav")
            write_wav(inp, sr, noisy(raw, sr, k, 1000 * idx + i))
            out = Path(td, f"{stem}_{snr}.txt")
            run_fldigi(mode, carrier, ["--benchmark-input", str(inp), "--benchmark-output",
                                       str(out)], xml)
            data = out.read_bytes() if out.exists() else b""
            Path(HERE, "rx", f"{stem}_{snr}.txt").write_bytes(data)
    line = f"{stem} {src} {mode} {speed:g} {idx} {carrier} {','.join(snrs)}"
    print(line, flush=True)
    return line


def one_noise(mode, seed):
    sr = RATE[mode]
    with tempfile.TemporaryDirectory() as td:
        inp = Path(td, "noise.wav")
        write_wav(inp, sr, noise_only(sr, 10, seed))
        out = Path(td, "noise.txt")
        run_fldigi(mode, 1500, ["--benchmark-input", str(inp), "--benchmark-output", str(out)],
                   mode_xml(mode, 3.0 if mode == "FSQ" else 1))
        data = out.read_bytes() if out.exists() else b""
    Path(HERE, "rx", f"{mode.lower()}_noise.txt").write_bytes(data)
    return f"{mode.lower()}_noise {mode} {seed} {len(data)}"


def main():
    (HERE / "tx").mkdir(exist_ok=True)
    (HERE / "rx").mkdir(exist_ok=True)
    jobs = int(os.environ.get("JOBS", "8"))
    with ThreadPoolExecutor(max_workers=jobs) as ex:
        tx_lines = list(ex.map(lambda a: one_tx(*a), enumerate(tx_cases())))
    with ThreadPoolExecutor(max_workers=jobs) as ex:
        rx_lines = list(ex.map(lambda a: one_rx(*a), enumerate(rx_cases())))
        noise_lines = list(ex.map(lambda a: one_noise(*a), (("FSQ", 77), ("IFKP", 78))))
    (HERE / "tx_hashes.txt").write_text(
        "# stem mode speed text index samplerate samples directed carrier startup "
        "fnv1a64(int16 LE) -- fldigi 4.2.13 bench TX, txlevel -12 dB\n"
        + "\n".join(tx_lines) + "\n")
    (HERE / "rx_cases.txt").write_text(
        "# stem source mode speed index rx-carrier snrs -- fldigi 4.2.13 bench RX of 1 s + "
        f"source + {TAIL_S} s, squelch off; noise seed = 1000 * index + snr step\n"
        + "\n".join(rx_lines) + "\n")
    (HERE / "noise_cases.txt").write_text(
        "# stem mode seed bytes -- fldigi 4.2.13 bench RX of 10 s noise at 0.1 RMS\n"
        + "\n".join(noise_lines) + "\n")
    (HERE / "texts.txt").write_text("".join(f"{k} {v}\n" for k, v in TEXTS.items()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
