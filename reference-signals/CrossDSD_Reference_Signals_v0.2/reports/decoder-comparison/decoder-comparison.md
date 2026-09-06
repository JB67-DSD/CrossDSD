# AudioGate vs FFmpeg DSD Decoder Comparison

- Status: **PASS**
- DSF files compared: 93
- Common PCM analysis format: float64, stereo, 352,800 Hz

## Scope and method

The same AudioGate-generated DSF is decoded through two independent branches:

1. the WAV exported by AudioGate at 352.8 kHz float32;
2. FFmpeg decoding to 352.8 kHz float64.

Each branch is aligned independently to the original DXD master using the A002
impulse. Gain is estimated by least squares. Decoder-to-decoder differences are
also reported after removing each branch's measured gain.

## Alignment

| Source | AudioGate latency | FFmpeg latency | Equal decoded lengths |
|---|---:|---:|---:|
| DSD64 | 1.2628 samples | 6.2456 samples | 31/31 |
| DSD128 | 2.1125 samples | 4.0860 samples | 31/31 |
| DSD256 | 2.7344 samples | 3.6733 samples | 23/31 |

## 1 kHz gain

| Signal | Source | AudioGate | FFmpeg | Difference |
|---|---|---:|---:|---:|
| B004 | DSD64 | -0.0000 dB | -6.0207 dB | 6.0206 dB |
| B004 | DSD128 | -0.0000 dB | -6.0206 dB | 6.0206 dB |
| B004 | DSD256 | -0.0001 dB | -6.0207 dB | 6.0206 dB |
| B005 | DSD64 | -0.0000 dB | -6.0207 dB | 6.0206 dB |
| B005 | DSD128 | -0.0000 dB | -6.0206 dB | 6.0206 dB |
| B005 | DSD256 | -0.0001 dB | -6.0207 dB | 6.0206 dB |

## Digital silence

| Source | AudioGate RMS | FFmpeg RMS |
|---|---:|---:|
| DSD64 | -141.045 dBFS | -106.692 dBFS |
| DSD128 | -144.127 dBFS | -119.339 dBFS |
| DSD256 | -144.491 dBFS | -172.028 dBFS |

## Tone response relative to the DXD master

| Signal | Frequency | Source | AudioGate gain | FFmpeg gain |
|---|---:|---|---:|---:|
| B001 | 20 Hz | DSD64 | -0.000 dB | -6.021 dB |
| B001 | 20 Hz | DSD128 | -0.000 dB | -6.021 dB |
| B001 | 20 Hz | DSD256 | -0.000 dB | -6.021 dB |
| B002 | 100 Hz | DSD64 | -0.000 dB | -6.021 dB |
| B002 | 100 Hz | DSD128 | -0.000 dB | -6.021 dB |
| B002 | 100 Hz | DSD256 | -0.000 dB | -6.021 dB |
| B006 | 10,000 Hz | DSD64 | -0.004 dB | -6.025 dB |
| B006 | 10,000 Hz | DSD128 | -0.001 dB | -6.021 dB |
| B006 | 10,000 Hz | DSD256 | -0.000 dB | -6.021 dB |
| B007 | 18,000 Hz | DSD64 | -0.025 dB | -6.046 dB |
| B007 | 18,000 Hz | DSD128 | -0.003 dB | -6.024 dB |
| B007 | 18,000 Hz | DSD256 | -0.001 dB | -6.021 dB |
| B008 | 20,000 Hz | DSD64 | -0.035 dB | -6.057 dB |
| B008 | 20,000 Hz | DSD128 | -0.004 dB | -6.025 dB |
| B008 | 20,000 Hz | DSD256 | -0.001 dB | -6.021 dB |
| B009 | 22,000 Hz | DSD64 | -0.049 dB | -6.071 dB |
| B009 | 22,000 Hz | DSD128 | -0.006 dB | -6.027 dB |
| B009 | 22,000 Hz | DSD256 | -0.001 dB | -6.022 dB |
| B010 | 30,000 Hz | DSD64 | -0.159 dB | -6.172 dB |
| B010 | 30,000 Hz | DSD128 | -0.014 dB | -6.035 dB |
| B010 | 30,000 Hz | DSD256 | -0.002 dB | -6.023 dB |
| B011 | 40,000 Hz | DSD64 | -0.855 dB | -6.468 dB |
| B011 | 40,000 Hz | DSD128 | -0.035 dB | -6.057 dB |
| B011 | 40,000 Hz | DSD256 | -0.004 dB | -6.025 dB |
| B012 | 80,000 Hz | DSD64 | -68.720 dB | -13.146 dB |
| B012 | 80,000 Hz | DSD128 | -0.855 dB | -6.468 dB |
| B012 | 80,000 Hz | DSD256 | -0.036 dB | -6.057 dB |

## Aggregate decoder agreement

| Source | Median normalized correlation | Median normalized difference RMS |
|---|---:|---:|
| DSD64 | 0.922008 | -29.344 dBFS |
| DSD128 | 0.994881 | -42.421 dBFS |
| DSD256 | 0.999868 | -59.719 dBFS |

## Interpretation

- AudioGate restores the approximately +6.0206 dB PCM level that FFmpeg leaves at the DSD convention level.
- Decoded lengths match for every DSD64 and DSD128 file; eight DSD256 AudioGate WAV files are one sample shorter than FFmpeg output.
- Differences remaining after gain compensation reflect the decoders' filters and ultrasonic-noise handling, not only the encoder.
