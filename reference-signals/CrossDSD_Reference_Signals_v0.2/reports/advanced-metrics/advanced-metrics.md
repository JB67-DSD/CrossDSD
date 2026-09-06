# CrossDSD Advanced Audio Metrics

- Status: **PASS**
- Decoder branches analyzed: 186
- Analysis rate: 352,800 Hz
- Welch FFT size: 32,768 samples

## Definitions

- Reconstruction SNR uses the gain-normalized, time-aligned error against the DXD master.
- Peak error is measured after the same gain normalization and alignment.
- THD+N is measured from 20 Hz to 20 kHz after least-squares removal of the known fundamental and DC.
- Band noise values are RMS levels of the gain-normalized reconstruction error.
- Ultrasonic energy values describe each decoded output, not only its error.

## 1 kHz reference metrics

| Signal | Source | Decoder | SNR | Peak error | THD+N | Result |
|---|---|---|---:|---:|---:|---|
| B004 | DSD64 | AudioGate | 36.921 dB | -44.874 dBFS | -96.239 dB | PASS |
| B004 | DSD64 | FFmpeg | 6.639 dB | -15.808 dBFS | -96.241 dB | PASS |
| B004 | DSD128 | AudioGate | 37.652 dB | -45.812 dBFS | -104.885 dB | PASS |
| B004 | DSD128 | FFmpeg | 19.290 dB | -27.110 dBFS | -104.853 dB | PASS |
| B004 | DSD256 | AudioGate | 47.536 dB | -58.720 dBFS | -108.413 dB | PASS |
| B004 | DSD256 | FFmpeg | 50.289 dB | -61.866 dBFS | -108.362 dB | PASS |
| B005 | DSD64 | AudioGate | 48.602 dB | -44.446 dBFS | -108.938 dB | PASS |
| B005 | DSD64 | FFmpeg | 19.704 dB | -16.191 dBFS | -108.927 dB | PASS |
| B005 | DSD128 | AudioGate | 50.341 dB | -45.943 dBFS | -117.693 dB | PASS |
| B005 | DSD128 | FFmpeg | 32.204 dB | -27.946 dBFS | -117.661 dB | PASS |
| B005 | DSD256 | AudioGate | 49.071 dB | -51.692 dBFS | -121.217 dB | PASS |
| B005 | DSD256 | FFmpeg | 51.406 dB | -54.363 dBFS | -121.159 dB | PASS |

## Error noise by band — B004 1 kHz at -20 dBFS

| Source | Decoder | 0–20 kHz | 20–30 kHz | 30–40 kHz | 40–50 kHz | 50–88.2 kHz | 88.2–176.4 kHz |
|---|---|---:|---:|---:|---:|---:|---:|
| DSD64 | AudioGate | -77.461 | -93.069 | -75.814 | -65.942 | -61.473 | -170.693 |
| DSD64 | FFmpeg | -74.384 | -93.066 | -75.590 | -64.068 | -39.752 | -30.132 |
| DSD128 | AudioGate | -87.282 | -124.993 | -121.601 | -107.352 | -71.204 | -61.107 |
| DSD128 | FFmpeg | -86.172 | -124.982 | -121.593 | -107.345 | -70.553 | -42.339 |
| DSD256 | AudioGate | -72.184 | -131.494 | -137.244 | -134.001 | -118.180 | -75.611 |
| DSD256 | FFmpeg | -74.488 | -131.467 | -137.145 | -133.963 | -118.249 | -79.547 |

## Output energy above frequency thresholds — B004

| Source | Decoder | >20 kHz | >30 kHz | >40 kHz | >50 kHz |
|---|---|---:|---:|---:|---:|
| DSD64 | AudioGate | -59.898 dBFS | -59.900 dBFS | -60.014 dBFS | -61.318 dBFS |
| DSD64 | FFmpeg | -33.376 dBFS | -33.376 dBFS | -33.376 dBFS | -33.377 dBFS |
| DSD128 | AudioGate | -60.047 dBFS | -60.047 dBFS | -60.047 dBFS | -60.047 dBFS |
| DSD128 | FFmpeg | -47.325 dBFS | -47.325 dBFS | -47.325 dBFS | -47.325 dBFS |
| DSD256 | AudioGate | -70.629 dBFS | -70.629 dBFS | -70.629 dBFS | -70.629 dBFS |
| DSD256 | FFmpeg | -79.113 dBFS | -79.113 dBFS | -79.113 dBFS | -79.113 dBFS |

## Impulse and group-delay summary

| Source | Decoder | Latency | Pre-ringing energy | Post-ringing energy | Significant response span | Median GD 20 Hz–20 kHz |
|---|---|---:|---:|---:|---:|---:|
| DSD64 | AudioGate | 1.262844 samples | -6.005 dB | -3.714 dB | 17 samples | 0.170 samples |
| DSD64 | FFmpeg | 6.245561 samples | n/a dB | -4.483 dB | 1 samples | 0.116 samples |
| DSD128 | AudioGate | 2.112541 samples | -9.853 dB | -5.982 dB | 10 samples | 0.072 samples |
| DSD128 | FFmpeg | 4.08597 samples | n/a dB | -8.706 dB | 3 samples | 0.039 samples |
| DSD256 | AudioGate | 2.734379 samples | -4.044 dB | -10.500 dB | 79 samples | -0.469 samples |
| DSD256 | FFmpeg | 3.673264 samples | -3.676 dB | -11.748 dB | 50 samples | -0.484 samples |

## Detailed outputs

- `advanced-metrics.csv`: metrics for all 186 decoder branches.
- `advanced-metrics.json`: complete metrics and methodology.
- `error-spectra.csv`: logarithmically binned error spectra for representative signals.
- `impulse-responses.csv`: detailed impulse responses for all decoder/rate combinations; the reported significant span uses a threshold 20 dB above the stationary noise RMS.
