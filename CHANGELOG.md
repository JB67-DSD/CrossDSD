# Library changelog

## 1.0.0 RC1 — audio engine

- Decode DSD64, DSD128, DSD256 and DSD512 to float32 DXD at 352.8 kHz.
- Encode DXD to all four DSD rates; DSD64 Enhanced profile.
- DSD transcoding, adaptive or exhaustive peak analysis, and uniform attenuation.
- Convert 44.1 kHz PCM AIFF/AIFC to DXD.
- Preserve compatible ID3 metadata and record processing history.
- C APIs for file processing, progressive streams and low-level DSP.
- Portable CPU engine, with optional Apple acceleration.

## Public repository preparation — September 5, 2026

- Apache 2.0 distribution, author headers and disclaimer summary.
- Standalone library build, without graphical applications.
- Developer guides, examples, tests and reference corpus.
- Rebuilt universal Mac SDK; Windows RC1 SDKs repackaged with Apache 2.0 notices, without changing the binaries.
- Documentation consolidated into a single English version, including SDK documents and reference chart labels.
