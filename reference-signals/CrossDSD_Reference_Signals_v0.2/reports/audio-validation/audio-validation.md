# CrossDSD Audio Validation Report

- Status: **PASS**
- Analysis rate: 352,800 Hz
- Files analyzed: 93 derivatives and 31 masters
- Decoder: `ffmpeg version 8.1.2 Copyright (c) 2000-2026 the FFmpeg developers`

## Method

Every DSF was decoded by FFmpeg to stereo float64 PCM at 352.8 kHz. The peak of
`A002_single_impulse_-6dBFS` establishes the alignment latency independently for
DSD64, DSD128 and DSD256. Measurements use the aligned interval corresponding to
the DXD master. RMS and peak values are unweighted full-band measurements.

The expected steady-state DSD convention gain is -6.0206 dB. Compliance is checked
on the -20 dBFS and -6 dBFS 1 kHz tones with a ±0.25 dB tolerance.

## Latency and padding

| Type | Latency (samples) | Latency (µs) | Trailing padding min/median/max (ms) |
|---|---:|---:|---:|
| DSD128 | 4.08597 | 11.582 | 23.208 / 24.064 / 24.637 |
| DSD256 | 3.673264 | 10.412 | 11.600 / 11.979 / 12.308 |
| DSD64 | 6.245561 | 17.703 | 46.422 / 47.383 / 49.228 |

## 1 kHz level and gain

| Signal | Type | Master RMS | Derivative RMS | Estimated gain | Result |
|---|---|---:|---:|---:|---|
| B004 | DSD64 | -23.047 dBFS | -27.699 dBFS | -6.021 dB | PASS |
| B004 | DSD128 | -23.047 dBFS | -29.003 dBFS | -6.021 dB | PASS |
| B004 | DSD256 | -23.047 dBFS | -29.067 dBFS | -6.021 dB | PASS |
| B005 | DSD64 | -9.047 dBFS | -14.989 dBFS | -6.021 dB | PASS |
| B005 | DSD128 | -9.047 dBFS | -15.064 dBFS | -6.021 dB | PASS |
| B005 | DSD256 | -9.047 dBFS | -15.067 dBFS | -6.021 dB | PASS |

## Digital silence

| Type | Decoded RMS | Decoded peak |
|---|---:|---:|
| DSD64 | -106.692 dBFS | -46.461 dBFS |
| DSD128 | -119.339 dBFS | -61.322 dBFS |
| DSD256 | -172.028 dBFS | -114.011 dBFS |

## Channel order and separation

| Type | L before R | L-event separation | R-event separation | Result |
|---|---|---:|---:|---|
| DSD128 | True | 34.321 dB | 30.239 dB | PASS |
| DSD256 | True | 70.421 dB | 64.970 dB | PASS |
| DSD64 | True | 16.206 dB | 15.008 dB | PASS |

## Interchannel phase

| Signal | Type | Master phase | Derivative phase | Error | Result |
|---|---|---:|---:|---:|---|
| B013 | DSD128 | 90.000° | 90.000° | -0.000° | PASS |
| B013 | DSD256 | 90.000° | 90.000° | 0.000° | PASS |
| B013 | DSD64 | 90.000° | 90.000° | 0.000° | PASS |
| B014 | DSD128 | 180.000° | 180.000° | -0.000° | PASS |
| B014 | DSD256 | 180.000° | 180.000° | -0.000° | PASS |
| B014 | DSD64 | 180.000° | -180.000° | 0.000° | PASS |

## Stability and full-scale checks

| Signal | Type | Peak | Finite | Clipping detected | Result |
|---|---|---:|---|---|---|
| F001 | DSD64 | -4.242 dBFS | True | False | PASS |
| F001 | DSD128 | -5.190 dBFS | True | False | PASS |
| F001 | DSD256 | -4.892 dBFS | True | False | PASS |
| F004 | DSD64 | -6.210 dBFS | True | False | PASS |
| F004 | DSD128 | -6.493 dBFS | True | False | PASS |
| F004 | DSD256 | -6.517 dBFS | True | False | PASS |

## Findings

- No validation error or warning.
