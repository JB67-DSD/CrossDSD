# CrossDSD Benchmark Suite v0.2

## Scope

Version 0.2 formalizes the complete Phase 0 benchmark corpus and its automated
validation workflow.

## Contents

- 31 deterministic 352.8 kHz stereo float32 DXD reference masters;
- 93 AudioGate DSF derivatives: DSD64, DSD128 and DSD256;
- 93 AudioGate-decoded 352.8 kHz stereo float32 DXD derivatives;
- 217 manifest records in JSON and CSV;
- SHA-256 integrity index for every distributed file;
- structural, signal, decoder-comparison and advanced-metrics tools;
- machine-readable and human-readable reference reports.

## AudioGate provenance

- KORG AudioGate 4.6.1 Intel build for macOS;
- executed through Rosetta 2 on an Apple Silicon M4 Max Mac;
- Conversion: High Quality;
- Normalize: Off.

## Major v0.2 decisions

- DXD masters remain the only reference masters.
- All DSF and decoded WAV files are labelled derivatives.
- Embedded `File Type` metadata is authoritative for DSD rate and decoded origin.
- AudioGate-decoded WAV metadata is normalized without changing PCM payloads.
- FFmpeg and AudioGate decoder branches are compared independently.
- Reconstruction differences use deterministic fractional-sample alignment.
- THD+N is evaluated from 20 Hz to 20 kHz; ultrasonic energy is reported separately.
- The private exploratory comparison directory is excluded from the suite.

## Reference status

The complete release validation passes with zero structural errors and zero
warnings. Scientific thresholds are regression guards, not claims of audibility.

