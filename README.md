# fldigi-fsq-core

[![CI](https://github.com/wave-owl/fldigi-fsq-core/actions/workflows/ci.yml/badge.svg)](https://github.com/wave-owl/fldigi-fsq-core/actions/workflows/ci.yml)

A C99 library that receives and transmits **FSQ** and **IFKP** exactly the way fldigi 4.2.13
does: audio in, characters out, and characters in, audio out, for every speed fldigi offers
(FSQ 1.5, 2, 3, 4.5 and 6 baud; IFKP 0.5x, 1x and 2x). It is a line-by-line port of
`src/fsq/fsq.cxx`, `src/ifkp/ifkp.cxx`, their varicode tables and `src/include/crc8.h`
(plus the carrier clamp of `src/trx/modem.cxx`), built on the shared
[fldigi-dsp-core](https://github.com/wave-owl/fldigi-dsp-core) (complex FFT, FIR band pass,
moving average, Blackman window). On the reference build the transmitter reproduces fldigi's
TX audio sample for sample and the receiver prints fldigi's characters byte for byte, clean,
in noise down to -12 dB, off frequency and on pure noise (see [Verification](#verification)).
It is the FSQ/IFKP implementation of [wave-owl](https://github.com/wave-owl/wave-owl).

| | |
|---|---|
| **Is** | A single-channel **receiver** for FSQ and IFKP: one event per decoded character (the byte, plus FSQ's frame markers BOT/EOL/EOT, whether fldigi prints it, whether it enters fldigi's FSQCALL text, and IFKP image headers), fldigi's s/n, metric and squelch (off as in fldigi's bench). |
| **Is** | The matching **transmitter** (capability tag `tx=1`): FSQ's start-of-frame and `mycall:` header (with the CRC-8 for directed messages), characters, idles, EOT/EOL and flush; IFKP's idles, characters and end; phase-continuous tones as fldigi's `tx_process`. |
| **Is** | fldigi's CRC-8 (`ffsq_crc8_hex`), so a host's FSQCALL parser and the TX header share one implementation. |
| **Is not** | An FSQCALL engine: no automatic replies, relay, sounder, file triggers or heard-list logic (that is protocol work for the host, and wave-owl keeps it passive). No images (FSQ/IFKP picture and avatar transfer). Not a skimmer: one carrier per instance. |
| | An I/O layer, threaded or allocating at run time: no sound card or file access, no threads, all memory allocated in `*_create()`, no global mutable state. |

## Contents

- [FSQ and IFKP in one paragraph](#fsq-and-ifkp-in-one-paragraph)
- [Modes and speeds](#modes-and-speeds)
- [Signal chain](#signal-chain)
- [fldigi source map](#fldigi-source-map)
- [fldigi behaviour kept on purpose](#fldigi-behaviour-kept-on-purpose)
- [API reference](#api-reference)
- [Usage examples](#usage-examples)
- [Contract and versioning](#contract-and-versioning)
- [Verification](#verification)
- [Performance and memory](#performance-and-memory)
- [Building](#building)
- [Maintenance and fldigi updates](#maintenance-and-fldigi-updates)
- [Roadmap](#roadmap)
- [License and credits](#license-and-credits)

## FSQ and IFKP in one paragraph

Both modes, by Murray Greenman ZL1BPU, use **incremental frequency keying**: 33 tones three
FFT bins apart, and every symbol 0..31 moves the tone by `symbol + 1` steps modulo 33
(`tone = (prev + sym + 1) % 33`), so the receiver needs neither carrier nor clock recovery:
it only measures the difference between successive loudest tones. Characters are one or two
symbols of an "unsquare" varicode (29 single-symbol codes, 3 x 29 two-symbol codes). FSQ
(Fast Simple QSO) adds frame markers and the FSQCALL directed protocol (`mycall:` + CRC-8,
`allcall`, triggers); IFKP is the plain keyboard variant with a slightly different varicode
and an image mode.

## Modes and speeds

| | FSQ | IFKP |
|---|---|---|
| Modem rate | 12000 Hz (`fsq.h:41`) | 16000 Hz (`ifkp.cxx:58`) |
| FFT, shift | 4096 points Blackman, every 256 samples | the same |
| Bins examined | 144 around the carrier | 151 around the carrier |
| Pre-filter | none | 129-tap band pass, carrier +- 0.75 x bandwidth |
| Bandwidth (fldigi's figure) | 290 Hz | 386 Hz |
| Speeds (TX samples per symbol) | 1.5 (8192), 2 (6144), 3 (4096), 4.5 (3072), 6 baud (2048) | 0.5x (8192), 1x (4096), 2x (2048) |
| Bin average, peak hits | 4, 3 (fldigi defaults) | 3, 4 |
| Framing | `" " + " \n" + mycall + ":" [+ CRC]`, text, `"  \b  "` (directed) or `"\n "`, 64 zero samples | two idles, text, one idle |
| Idle | symbols 28, 30 | symbol 0 (also while the TX buffer is empty) |

## Signal chain

Receiver (`fsq::rx_process`, `ifkp::rx_process`): real audio -> (IFKP: band pass `Irun`) ->
4352-sample window advanced by 256 samples -> Blackman window -> `(d, d)` -> complex FFT ->
`|X|^2` of 144/151 bins around the carrier -> moving average per bin -> loudest bin; when it
holds for `peak_hits` blocks and differs from the last symbol's bin, the bin difference goes
through the nibble table (`floor(0.5 + diff / 3)` wrapped, minus 1) -> two nibbles -> varicode
-> character event. FSQ watches the last bytes for BOT/EOL/EOT and keeps fldigi's squelch
latch; both compute fldigi's s/n and metric.

Transmitter (`fsq::tx_process`, `ifkp::tx_process`): per symbol `tone = (prev + sym + 1) %
33`, frequency `(basetone + 3 tone) * rate / 4096` in integer Hz, `symlen` samples of
`cos(phase)` with a continuous phase accumulator; peak 1.0 before fldigi's TX level.

## fldigi source map

fldigi 4.2.13 (mirror `wave-owl/fldigi`, tag v4.2.13, commit e7c3ab37):

| fldigi lines | Function | Here |
|---|---|---|
| `fsq.h:41-50`, `ifkp.h:37-48`, `ifkp.cxx:58` | constants | `fsq_internal.h` |
| `fsq.cxx:116-128` (= `ifkp.cxx:100-112`) | `init_nibbles` | `fsq_common.c` `ffsq_init_nibbles` |
| `fsq.cxx:168-220`, `fsq.cxx:257-284` | FSQ constructor, `rx_init` | `fsq_rx.c` `ffsq_rx_create`, `rx_init` |
| `fsq.cxx:297-306`, `modem.cxx:319-324` | FSQ `set_freq`, clamp | `fsq_rx.c` `set_freq`, `fsq_tx.c` `set_tonebase` |
| `fsq.cxx:322-338` | `adjust_for_speed` | `fsq_common.c` `ffsq_symlen` |
| `fsq.cxx:376-404` | `restart`, `valid_char` | `fsq_rx.c` `ffsq_rx_create`, `fsq_common.c` |
| `fsq.cxx:410-415` | `fsq_squelch_open` | `fsq_rx.c` |
| `fsq.cxx:1004-1027` | `lf_check` | `fsq_rx.c` `fsq_lf_check` |
| `fsq.cxx:1029-1125` | `process_symbol` (without `parse_rx_text`) | `fsq_rx.c` `fsq_process_symbol` |
| `fsq.cxx:1131-1196` | `process_tones` | `fsq_rx.c` `fsq_process_tones` |
| `fsq.cxx:1263-1320` | `rx_process` | `fsq_rx.c` `ffsq_rx_process`, `block` |
| `fsq.cxx:1333-1397`, `fsq.cxx:1483-1488` | `flush_buffer`, `send_tone`, `send_symbol`, `send_idle`, `send_char`, `send_string` | `fsq_tx.c` |
| `fsq.cxx:1497-1536` | `tx_process` | `fsq_tx.c` `ffsq_tx_begin/_put/_end` |
| `fsq_varicode.cxx:1-75`, `ifkp_varicode.cxx:1-75` | varicode tables | `fsq_tables.c` (generated by `tools/gen_tables.py`) |
| `crc8.h:10-35` | `CRC8` | `fsq_common.c` `ffsq_crc8_hex` |
| `ifkp.cxx:114-177`, `ifkp.cxx:207-293` | IFKP constructor, `rx_init`, `init`, `set_freq` | `fsq_rx.c` |
| `ifkp.cxx:344-371` | `restart`, `valid_char` | `fsq_rx.c`, `fsq_common.c` |
| `ifkp.cxx:377-421` | `parse_pic` (header only) | `fsq_rx.c` `ifkp_parse_pic` |
| `ifkp.cxx:423-551` | `process_symbol`, `process_tones` | `fsq_rx.c` |
| `ifkp.cxx:627-674` | `rx_process` | `fsq_rx.c` |
| `ifkp.cxx:687-733` | `send_tone`, `send_symbol`, `send_idle`, `send_char` | `fsq_tx.c` |
| `ifkp.cxx:1056-1104` | `tx_process` | `fsq_tx.c` |
| `misc.h:66-69`, `gfft.h`, `filters.cxx:46-310` | Blackman, FFT, FIR, `Cmovavg` | fldigi-dsp-core 0.6.0 |

Not ported (host protocol, GUI or images): `parse_rx_text` and the trigger handlers
(`fsq.cxx:448-1002`), the TX queue, delayed send, heard-list aging and sounder
(`fsq.cxx:1542-1918`), IFKP's heard-list scan (`ifkp.cxx:72-96`), the picture and avatar
code (`fsq-pic.cxx`, `ifkp-pic.cxx`, `tux.cxx`, `fsq.cxx:1198-1261`, `ifkp.cxx:553-625`).

## fldigi behaviour kept on purpose

The core reproduces fldigi bit for bit; each behaviour below is covered by a test, so that a
later "fix" is a visible decision:

| # | Behaviour | fldigi | Test |
|---|---|---|---|
| Q1 | The FSQ receiver does not depend on the speed: FFT size, shift and averages are fixed; the speed only sets the TX symbol length | `fsq.cxx:1131-1196`, `fsq.cxx:1263-1320` | goldens (every speed) |
| Q2 | FSQ always transmits around 1500 Hz (`get_txfreq`), whatever the receive carrier; the receive tone base only enters the rounding to a multiple of 3 | `modem.cxx:349`, `fsq.cxx:301-305` | `test_tx_api` |
| Q3 | Until the first retune FSQ computes its TX tone base with bandwidth 0: tones 1494-1775 Hz instead of 1353-1634 Hz, which fldigi's own receiver at 1500 Hz decodes only partly. `startup_tonebase = 1` reproduces it; the default is the state after a retune | `fsq.cxx:258-260`, `fsq.cxx:297-306` | golden `fsq_3_startup`, `test_tx_api` |
| Q4 | Tone frequencies are integer Hz (integer division), not exact bin centres | `fsq.cxx:1349`, `ifkp.cxx:692` | TX goldens, `test_tx_api` |
| Q5 | IFKP uses a bin average of 3 at every speed: `init` overrides the 3/4 of `restart`; the IFKP receiver is speed independent | `ifkp.cxx:241-254`, `ifkp.cxx:344-360` | goldens (every speed) |
| Q6 | Tones are phase continuous and unshaped; FSQ ends an over with 64 zero samples and on an empty TX buffer, IFKP idles on an empty buffer and ends with one idle | `fsq.cxx:1333-1365`, `fsq.cxx:1511-1523`, `ifkp.cxx:1095-1101` | TX goldens |
| Q7 | IFKP's s/n reads `tones[peak - 1]`/`tones[peak + 1]` without wrap at the band edges (fldigi reads neighbouring class members, a pointer seen as a denormal); 0.0 here | `ifkp.cxx:517` | goldens (s/n gates printing only through the squelch) |
| Q8 | Uninitialised members and statics made per instance and zero: `prev_symbol`, `prev_peak`, IFKP's `ch_sqlch_open` (never set), FSQ's `lf_check` statics, IFKP's global `no_signal` | `fsq.cxx:1006-1007`, `ifkp.cxx:482` | goldens (the first character after silence, e.g. `" wp"`, is fldigi's) |
| Q9 | fldigi's CRC-8 indexes its table with a signed char; bytes above 127 are taken unsigned here (callsigns are ASCII) | `crc8.h:27-31` | `test_crc` (fldigi vectors) |
| Q10 | An FSQ carrier below 208 Hz would make fldigi read before its FFT buffer; the core clamps the FSQ carrier to 210 .. 2855 Hz (fldigi's clamp is 145 .. 2855 Hz) | `fsq.cxx:1137`, `modem.cxx:321-324` | `test_rx_api` |
| Q11 | At IFKP 2x a symbol whose tone returns to the tone before the previous one inside 2048 samples is lost, e.g. `=` (`{0, 31}`); the fldigi bench drops it too | `ifkp.cxx:484-551` | `test_loopback` |
| Q12 | FSQ prints a single-nibble character only when the next nibble arrives, so the last spaces of EOL/EOT are never printed; IFKP's NUL (idle) is not printed | `fsq.cxx:1044-1057`, `ifkp.cxx:437-450` | goldens, `test_flags` |
| Q13 | An IFKP image header `pic%X` switches fldigi to picture reception; the core flags the byte (`FFSQ_EV_IMAGE`) and keeps decoding text | `ifkp.cxx:377-421`, `ifkp.cxx:458-459` | `test_image_header` |

Queue-level behaviour stays with the host: fldigi's FSQ text queue treats `^r` as the end of
the text and drops a `^` before any other character (`fsq.cxx:1581-1614`), and fldigi hands
bytes above 127 to the modems as negative `int`s; the core takes bytes 0..255 unsigned.

Also as fldigi: with the squelch off both receivers print whatever the noise produces; the
goldens include 10 s of pure noise, decoded byte for byte like fldigi.

## API reference

All declarations are in `include/fldigi_fsq.h`.

| Function | Description |
|---|---|
| `ffsq_version()`, `ffsq_abi()` | `"fldigi-fsq-core 0.1.0 contract=fsq-c1 abi=1 tx=1 dsp=0.6.0"`, `1`. |
| `ffsq_sample_rate(mode)`, `ffsq_bandwidth(mode)`, `ffsq_symlen(mode, speed)` | 12000/16000 Hz, 290/386 Hz, samples per TX symbol. |
| `ffsq_crc8_hex(s, out)` | fldigi's CRC-8 of `s` as two lower-case hex digits. |
| `ffsq_rx_create(cfg)` | Receiver: `mode` (`FFSQ_MODE_FSQ`/`FFSQ_MODE_IFKP`), `speed`, `carrier_hz`, `movavg`, `peak_hits` (0 = fldigi's defaults), `squelch_on`, `squelch` (metric units). NULL for a bad mode or speed. |
| `ffsq_rx_process(rx, x, n, ev, max)` | Audio at the modem rate (int16 / 32768 scale) in; one `ffsq_event {ch, flags}` per decoded character out (`FFSQ_EV_BOT/EOL/EOT/SHOWN/TEXT/IMAGE`); returns their number; overflow is counted (`ffsq_rx_dropped`). |
| `ffsq_rx_snr_db`, `_metric`, `_carrier_hz` | fldigi's s/n, squelch metric and the (clamped) carrier. |
| `ffsq_rx_set_carrier/_squelch`, `ffsq_rx_reset` | Runtime changes; reset returns to the state after create. |
| `ffsq_rx_footprint`, `ffsq_rx_take_symbols` | Bytes allocated; test tap of the symbols handed to `process_symbol`. |
| `ffsq_tx_create(cfg)` | Transmitter: `mode`, `speed`, `carrier_hz`, FSQ `mycall` (at most 20 bytes), `directed`, `startup_tonebase` (Q3). |
| `ffsq_tx_begin/_put/_idle/_end(tx, out, max)` | One state step of `tx_process`: the start of the over, one character, one idle, the end. Each writes all of its samples or none; `ffsq_tx_bound` is always enough. |
| `ffsq_tx_symlen`, `ffsq_tx_bound`, `ffsq_tx_symbols`, `ffsq_tx_tone_hz` | Samples per symbol, the largest step, the symbols a character takes, the audio frequency of a tone. |

## Usage examples

C:

```c
ffsq_tx_config tc = {FFSQ_MODE_FSQ, 3.0, 1500.0, "dg6fl", 1, 0};
ffsq_tx *tx = ffsq_tx_create(&tc);
size_t bound = ffsq_tx_bound(tx);
double *buf = malloc(bound * sizeof *buf);
size_t n = ffsq_tx_begin(tx, buf, bound);        /* " " " \n" "dg6fl:" CRC */
/* ... play n samples, then for each character of "dl1abc hello": */
n = ffsq_tx_put(tx, 'd', buf, bound);
n = ffsq_tx_end(tx, buf, bound);                 /* "  \b  " and 64 zeros */

ffsq_rx_config rc = {FFSQ_MODE_FSQ, 3.0, 1500.0, 0, 0, 0, 0.0};
ffsq_rx *rx = ffsq_rx_create(&rc);
ffsq_event ev[256];
size_t k = ffsq_rx_process(rx, audio, n_audio, ev, 256);
for (size_t i = 0; i < k; i++)
    if (ev[i].flags & FFSQ_EV_SHOWN)
        putchar(ev[i].ch);
```

Python (ctypes; wave-owl's `src/fsq_native.py` wraps the same calls):

```python
import ctypes
lib = ctypes.CDLL("build/libfldigi_fsq.dylib")
lib.ffsq_version.restype = ctypes.c_char_p
print(lib.ffsq_version().decode())
out = ctypes.create_string_buffer(3)
lib.ffsq_crc8_hex(b"dg6fl", out)
print(out.value)        # b'00'
```

## Contract and versioning

The contract tag `fsq-c1` and the ABI tag `abi=1` change only with an incompatible API change
(the event layout or a config struct included); wave-owl refuses a library with other tags.
Versions follow semantic versioning. The `dsp/` submodule is pinned by `dsp.lock` (version,
commit and content checksum of the linked files); `tools/check_dsp.sh` runs before every
build and refuses any other fldigi-dsp-core (wave-owl R-920.3).

## Verification

`make test` runs two programs:

- `tests/test_fsq.c` (independent of fldigi except the CRC vectors): parameters, CRC-8 against
  16 vectors from fldigi's own `crc8.h` (`tests/golden/make_crc_golden.cxx`), loopback TX ->
  RX of every printable character at every FSQ and IFKP speed (IFKP also at 1000 Hz),
  directed headers with the CRC computed by `ffsq_crc8_hex`, frame flags, dropped events,
  chunk invariance (1, 333, 4096 samples, the whole input) and reset, squelch (noise prints
  nothing above the noise metric, a -6 dB FSQ signal still decodes), IFKP image headers,
  carrier clamps, +-10 Hz offsets, TX framing sizes and tone frequencies, footprint.
- `tests/test_fsq_fldigi.c`: goldens from the fldigi 4.2.13 bench build
  (`tests/golden/make_fldigi_golden.py`; headless fldigi with the TX extension of wave-owl's
  archived change `fix-psk-rtty-vs-fldigi` and `tests/golden/fldigi-bench-fsq.patch`, which
  routes FSQ's monitor output to the bench text, disables `parse_rx_text` and feeds the FSQ
  TX queue): 20 TX overs (every speed with `cq cq de dg6fl dg6fl k` and with a pangram of every
  character class, a directed and an allcall frame, the start-up tone base, IFKP 10 Hz off),
  66 RX decodes (12 stored overs clean and at -3, -6, -9, -12 dB in 2500 Hz with deterministic
  noise rebuilt bit for bit in C, FSQ received 10 Hz off, 10 s of pure noise per mode).

| | Result (macOS arm64, Apple clang `-O3` and the `-O1` sanitizer build) |
|---|---|
| TX overs identical to fldigi (FNV-1a of the int16 output) | 20 of 20 |
| RX decodes identical to fldigi (byte for byte) | 66 of 66 |

On other platforms (another libm, x86 without FMA) the results need not be bit-identical;
there the clean decodes must stay within 1 + 5 % edit distance of fldigi's and the noisy
ones in total within 20 % + 10 errors. The arithmetic the reference build fuses (`norm`, the
s/n line, the IFKP filter corners, the Blackman window) is written as explicit `fma`, so the
result does not depend on the compiler's contraction setting. CI runs both programs on
Ubuntu and macOS and again under AddressSanitizer and UndefinedBehaviorSanitizer.

## Performance and memory

`make bench` (Apple M4, one core), share of one core for real-time audio:

| | FSQ (every speed) | IFKP (every speed) |
|---|---|---|
| Receiver | 0.06 % | 0.12 % |
| Transmitter | < 0.01 % | < 0.01 % |
| fldigi 4.2.13 receiver (bench, same machine) | 0.06 % | 0.13 % |

One receiver allocates about 230 kB (the FFT window and buffers, the IFKP band pass, 151
moving averages).

## Building

```sh
git clone --recursive https://github.com/wave-owl/fldigi-fsq-core
cd fldigi-fsq-core
make            # build/libfldigi_fsq.a and build/libfldigi_fsq.{so,dylib}
make test       # unit tests and fldigi goldens
make sanitize   # the same under ASan + UBSan
make bench      # CPU and memory
```

C99, `libm` only. wave-owl builds it with `make build-fsq-core` into
`third_party/build/libfldigi_fsq.{dylib,so}`.

## Maintenance and fldigi updates

Every ported function cites its fldigi lines. When fldigi changes `fsq.cxx`, `ifkp.cxx` or
the varicodes, wave-owl's `scripts/fldigi_upstream_check.py` reports the affected citations;
regenerate the tables (`python3 tools/gen_tables.py /path/to/fldigi > src/fsq_tables.c`) and
the goldens (`FLDIGI_BENCH_BIN=/path/to/fldigi python3 tests/golden/make_fldigi_golden.py`,
after applying `tests/golden/fldigi-bench-fsq.patch` to the bench build) and port the change.
A change to a behaviour of the quirk table needs a new wave-owl ADR.

## Roadmap

- FSQ and IFKP picture and avatar transfer (`fsq-pic.cxx`, `ifkp-pic.cxx`) under a new ABI.
- Nothing of FSQCALL's automatic behaviour will be added here: replies, relay and sounder are
  host decisions (and wave-owl keeps them off, D-883).

## License and credits

GPL-3.0-or-later (see `LICENSE`), as fldigi. FSQ and IFKP were designed by Murray Greenman
ZL1BPU. The fldigi modems are (c) 2015 Dave Freese W1HKJ and the fldigi authors; the varicode
files carry no header of their own and are distributed under fldigi's licence. Port (c) 2026
Gerolf Ziegenhain.
