# Changelog

All notable changes to fldigi-fsq-core. Versions follow semantic versioning; the contract
tag `fsq-c1` changes only with an incompatible API change.

## 0.1.0 - 2026-10-18

First release, for wave-owl change `add-fldigi-fsq-ifkp-core` (umbrella
`add-fldigi-native-modes`).

- Receiver and transmitter for FSQ (1.5, 2, 3, 4.5, 6 baud) and IFKP (0.5x, 1x, 2x) of
  fldigi 4.2.13, ported from `fsq.cxx`, `ifkp.cxx`, `fsq_varicode.cxx`, `ifkp_varicode.cxx`
  and `crc8.h`: 4096-point Blackman FFT every 256 samples, per-bin moving average, peak-hit
  symbol detector, nibble table, varicodes; FSQ frame markers (BOT/EOL/EOT) and squelch
  latch, IFKP band pass and image-header flag; fldigi's s/n, metric and squelch;
  phase-continuous tone generator with FSQ's header, CRC-8, EOT/EOL and flush and IFKP's
  idles. Capability tag `tx=1`. `ffsq_crc8_hex` for hosts.
- fldigi's behaviours Q1-Q13 kept and tested (README).
- Built on fldigi-dsp-core 0.6.1 (`dsp/`, pinned by `dsp.lock`; 0.6.0 adds the Blackman
  window).
- Goldens from the fldigi 4.2.13 bench: 20 TX overs and 66 RX decodes identical to fldigi on
  the reference platform, also in the sanitizer build; CRC vectors from fldigi's `crc8.h`;
  unit tests; CPU and memory benchmark.
