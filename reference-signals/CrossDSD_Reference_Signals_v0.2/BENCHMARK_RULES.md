# CrossDSD Benchmark Suite rules

## Reference and derivative roles

1. Only deterministic files generated directly in `DXD_352k8_float32` may have
   the role `reference_master`.
2. Encoded DSF and decoded PCM files must have derivative roles and identify
   their direct source in the manifest and embedded metadata.
3. A derivative must never silently replace or overwrite a reference master.

## Adding a signal

Every new signal must have:

- a stable test identifier;
- a documented measurement purpose;
- deterministic generation parameters and random seed where applicable;
- sample rate, channels, encoding, duration and target level;
- JSON and CSV manifest records;
- SHA-256 checksum;
- corresponding derivatives for every rate required by the release;
- automated validation coverage.

## Adding an encoder or decoder

Record:

- product and exact version;
- operating system and processor architecture;
- compatibility or translation layer, if any;
- conversion-quality setting;
- normalization setting;
- output format and precision;
- direct source file;
- embedded provenance metadata.

The new implementation must be measured independently against the DXD master. A
round trip through one vendor's encoder and decoder is useful but is not an
independent reference.

## Naming and metadata

- Existing test filenames remain unchanged.
- Provenance and processing stage belong in directory names and manifests.
- Embedded `File Type` identifies DSD64/128/256 or `DXD from DSD64/128/256`.
- Directory names use the canonical `AudioGate` capitalization.

## Numerical policy

- Analysis and future CrossDSD reference processing use float64.
- Interchange and editor-facing output may use float32 or float64.
- CrossDSD does not produce integer PCM.
- No implicit normalization, quantization, dither or clipping is permitted.
- Editing-mode DSD → DXD restores PCM level continuity.
- Raw DSD convention level is an explicit measurement/interoperability mode.

## Release procedure

1. Run `python3 tools/run_benchmark.py`.
2. Require zero structural errors and warnings.
3. Review scientific warnings and failures.
4. Confirm manifest and report record counts.
5. Verify every entry in `SHA256SUMS.txt`.
6. Update `VERSION`, release notes and README.
7. Commit the immutable release state.

