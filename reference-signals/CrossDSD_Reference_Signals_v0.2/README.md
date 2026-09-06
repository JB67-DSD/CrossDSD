# CrossDSD Benchmark Suite v0.2

Deterministic mathematical reference signals for evaluating DSD ⇄ PCM/DXD converters.

## Provenance and licensing

The DXD reference masters are original deterministic mathematical signals
generated specifically for the CrossDSD project. They contain no musical
recording or other third-party audio content.

The DSD64, DSD128 and DSD256 DSF derivatives, together with their decoded DXD
counterparts, were produced by Jean-Luc EVRARD from these synthetic masters
using KORG AudioGate. AudioGate is identified solely to make the conversion
chain reproducible; KORG is not affiliated with or endorsing this project.

The generic DSD/DXD artwork embedded in the derivatives is an original
CrossDSD project asset. The `CrossDSD Project` value found in embedded metadata
is the stable technical identity of the corpus; its copyright holder is
Jean-Luc EVRARD.

The complete suite, including audio signals, documentation, manifests, reports
and validation tools, is licensed under the Apache License, Version 2.0. See
`LICENSE` and `NOTICE`.

## Reference masters

- 31 WAV files
- WAV IEEE 754 float32
- Stereo
- 352,800 Hz
- No dither
- Random seed: 20260716

The files in `DXD_352k8_float32` are the reference masters.

## AudioGate DSD derivatives

Each reference master has been converted to three DSF derivatives:

- `AudioGate64`: 31 DSD64 files
- `AudioGate128`: 31 DSD128 files
- `AudioGate256`: 31 DSD256 files

Each DSF has also been decoded by AudioGate to 352.8 kHz stereo float32 WAV:

- `AudioGateDecode_DSD64_to_DXD`: 31 WAV files
- `AudioGateDecode_DSD128_to_DXD`: 31 WAV files
- `AudioGateDecode_DSD256_to_DXD`: 31 WAV files

The encoder metadata embedded in all 93 DSF files is:

`KORG AudioGate ver.4.6.1 (Mac OSX 26.5.210.5 Intel)`

Although the Intel build is the only available macOS version of AudioGate, the
PCM-to-DSD conversions were performed on a Mac with an Apple Silicon M4 Max
processor. AudioGate therefore ran through Rosetta 2 translation.

AudioGate conversion settings:

- Conversion: High Quality
- Normalize: Off

The DSF and decoded WAV files are derivatives used to characterize and compare
DSD decoders. They must not be treated as reference masters. Each derivative
contains identifying metadata, including its source and file type.

The decoded WAV metadata was normalized without re-encoding the audio by
`tools/rebuild_audiogate_wav_metadata.py`. The embedded artwork was preserved and
the PCM payload of every file was verified bit-for-bit before and after the
metadata update.

## Manifests

`manifest.json` and `manifest.csv` each contain one record for every physical audio
file: 31 DXD reference masters, 93 DSD derivatives and 93 AudioGate-decoded DXD
derivatives, for a total of 217 records. Each derivative identifies its direct
source and includes its technical properties, embedded metadata, conversion
settings, file size and SHA-256 checksum.

The existing directory and file naming convention is intentional and remains
unchanged. The authoritative DSD type is stored inside each DSF in the `File Type`
metadata field (`DSD64`, `DSD128` or `DSD256`), so the DSD type does not need to
be duplicated in the filename.

## Contents
- A: fundamental/time-domain signals
- B: tones and stereo phase tests
- C: sweeps
- D: multitone/intermodulation
- E: deterministic noise
- F: pathological/torture signals

## Integrity

`SHA256SUMS.txt` contains a SHA-256 checksum for every distributed WAV and DSF
file, as well as the suite documentation and manifest files. From this directory,
verify the complete set with:

```sh
shasum -a 256 -c SHA256SUMS.txt
```

## Automated validation

The complete release workflow is executed with one command:

```sh
python3 tools/run_benchmark.py
```

It validates the corpus, regenerates every scientific report and graphic, updates
the release checksums, then performs a final integrity validation. Python 3.9 or
later with NumPy and SciPy is required, together with FFmpeg/FFprobe.

`tools/validate_benchmark.py` performs the complete structural validation of the
suite:

- agreement between the JSON and CSV manifests;
- agreement between the manifest inventory and the audio files on disk;
- WAV/DSF container, codec, sample rate, PCM depth, channels and duration;
- DSF bit order, embedded metadata and authoritative `File Type`;
- relationship between every DSD derivative and its DXD source;
- file sizes, per-record SHA-256 values and the global checksum index.

The validator requires Python 3.9 or later and `ffprobe`. Run it from the suite
directory:

```sh
python3 tools/validate_benchmark.py
```

It returns exit status 0 on success and 1 when a validation error is found. A
machine-readable report can also be generated:

```sh
python3 tools/validate_benchmark.py --json-report validation-report.json
```

## Audio validation

`tools/analyze_audio_validation.py` executes the signal-level validation of all
93 AudioGate derivatives against their 31 DXD masters. It requires Python 3,
NumPy and FFmpeg:

```sh
python3 tools/analyze_audio_validation.py
```

The analysis decodes every DSF to stereo float64 PCM at 352.8 kHz and uses the
single-impulse test to establish alignment independently for DSD64, DSD128 and
DSD256. It measures:

- actual RMS and peak levels;
- the PCM/DSD convention gain;
- decoded silence noise;
- channel order and event separation;
- interchannel quadrature and opposite-polarity phase;
- effective decoded duration, latency and trailing padding;
- finite output and clipping on difficult near-full-scale signals.

The current complete run passes all defined checks. Its versioned reports are in
`reports/audio-validation`:

- `audio-validation.md`: human-readable summary;
- `audio-validation.csv`: one measurement row per DSD derivative;
- `audio-validation.json`: complete measurements, thresholds and findings.

Principal results:

- measured steady-state gain: -6.0206 dB for DSD64, DSD128 and DSD256;
- fractional latency at 352.8 kHz: 6.2456 samples for DSD64, 4.0860 samples
  for DSD128 and 3.6733 samples for DSD256;
- median trailing padding: 47.384 ms for DSD64, 24.065 ms for DSD128 and
  11.978 ms for DSD256;
- correct left/right order;
- quadrature and opposite-polarity phase preserved within 1 degree;
- no clipping or non-finite output detected by the selected stability tests.

RMS and peak results are unweighted full-band measurements and therefore include
the decoded ultrasonic DSD noise. The decoder used for this report is identified
inside the report itself.

## AudioGate and FFmpeg decoder comparison

`tools/compare_decoders.py` compares the AudioGate and FFmpeg decoding branches
for all 93 DSF files:

```sh
python3 tools/compare_decoders.py
```

The complete reports are in `reports/decoder-comparison`. Principal findings:

- AudioGate restores the PCM reference level, while FFmpeg retains the DSD
  convention level at approximately -6.0206 dB;
- AudioGate impulse latency is 1.2628, 2.1125 and 2.7344 samples for DSD64,
  DSD128 and DSD256;
- FFmpeg impulse latency is 6.2456, 4.0860 and 3.6733 samples respectively;
- decoded lengths match for every DSD64 and DSD128 file; eight DSD256 AudioGate
  WAV files are one sample shorter than the corresponding FFmpeg output;
- AudioGate increasingly filters ultrasonic content at lower DSD rates: the
  80 kHz tone is attenuated by 68.720 dB in DSD64, 0.855 dB in DSD128 and
  0.036 dB in DSD256;
- after gain compensation, median AudioGate/FFmpeg correlation rises from
  0.876033 for DSD64 to 0.993464 for DSD128 and 0.999982 for DSD256.

These differences characterize the complete decoding branches, including their
gain conventions, filters and ultrasonic-noise handling.

## Advanced audio metrics

`tools/advanced_audio_metrics.py` generates the advanced scientific measurements
for all 186 AudioGate and FFmpeg decoder branches:

```sh
python3 tools/advanced_audio_metrics.py
```

The versioned outputs are in `reports/advanced-metrics`:

- `advanced-metrics.csv` and `advanced-metrics.json`: aligned peak error,
  reconstruction SNR, 20 Hz–20 kHz THD+N, error noise by band and decoded-output
  energy above 20, 30, 40 and 50 kHz;
- `error-spectra.csv`: logarithmically binned full-band error spectra for silence,
  impulse, 1 kHz tone, sweep, multitone and dense torture signals;
- `impulse-responses.csv`: detailed responses for every decoder and DSD rate;
- `advanced-metrics.md`: human-readable methodology and summary.

Spectral measurements use 50%-overlapped Hann-window Welch analysis with a
32,768-sample FFT. Reconstruction SNR and peak error use the gain-normalized,
time-aligned difference against the DXD master. Fractional latency is estimated
once per decoder and DSD rate by parabolic interpolation of the A002 impulse peak,
then compensated with cubic Lagrange interpolation. Raw decoded samples are still
used for level, THD+N and ultrasonic-energy measurements so the alignment process
cannot alter their spectrum. THD+N is deliberately limited to
20 Hz–20 kHz; ultrasonic DSD noise is reported separately rather than being
mislabelled as audible distortion.

Initial THD+N pass thresholds at 1 kHz are -95 dB for DSD64, -102 dB for
DSD128 and -105 dB for DSD256. Results up to 3 dB above these limits are warnings;
larger regressions are failures.

All release criteria, including gain, phase, frequency response, THD+N, group
delay and spectral-regression tolerances, are defined in
`benchmark_thresholds.json`. The rules for accepting a new signal, encoder or DSD
rate are defined in `BENCHMARK_RULES.md`.

Impulse latency is measured from the raw peak. Group delay is calculated from the
phase derivative of the impulse response. The significant impulse span uses a
threshold 20 dB above the estimated stationary noise RMS, because raw DSD
noise-shaping energy would otherwise dominate a simple time-domain energy span.

The final release summary is stored in `reports/benchmark-v0.2-summary.md` and
`.json`. Reproducible SVG figures are stored in `reports/graphics`:

- tone response of AudioGate and FFmpeg at all three DSD rates;
- B004 aligned-error spectrum by frequency band;
- measured fractional latency by decoder and rate.

## Important

AudioGate is currently a documented comparison encoder, not a selected reference
DSD encoder. Any DSF produced by AudioGate, TASCAM, FFmpeg-derived tools or future
CrossDSD builds must be labelled as a derivative, not as a reference master.

Files marked `torture` or containing ultrasonic energy are intended for measurement,
not casual listening. Keep playback levels low.
