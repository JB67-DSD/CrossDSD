# CrossDSD Library 1.1.0 validation

The macOS validation ran on an Apple M4 Max on November 6, 2026.

## Reference corpus

The CLI decoded 93 reference DSF files using forced CPU and forced Metal with
both filter profiles. All 186 comparisons passed. Each pair has the same frame
count and ID3 metadata. The comparison covers 634,459,936 float32 values.

| Profile | Passed | Worst absolute error | Worst RMS error | Absolute limit | RMS limit |
| --- | ---: | ---: | ---: | ---: | ---: |
| Standard | 93/93 | 1.788e-6 | 2.096e-7 | 2.0e-6 | 2.5e-7 |
| Raw | 93/93 | 2.146e-6 | 2.940e-7 | 5.0e-6 | 5.0e-7 |

## Long RF64 output

A repeated stereo DSD64 reference signal containing 4,563,402,752 input samples
per channel was decoded with forced Metal and automatic container selection.
The result was one 4.25 GiB RF64 file containing 570,425,344 DXD frames. The
64-bit container lengths and the reported file size matched the actual file.
Every output value, including those beyond the 4 GiB boundary, was compared
with the periodic CPU reference. The maximum absolute error was 3.874e-7 and
the RMS error was 5.555e-8. The measured decode time was 7.30 s for 26 min 57 s
of audio, with a peak process memory footprint of 183 MB on this configuration.

## Architecture coverage

The SDK contains arm64 and x86_64 slices. The Metal regression test passed in
arm64 and in x86_64 under Rosetta on the M4 Max. It covers all four DSD rates,
block boundaries, fixed gain, cancellation and admission beyond the previous
32-bit sample limit. Physical Intel hardware and Windows 1.1 binaries are
outside this validation coverage.
