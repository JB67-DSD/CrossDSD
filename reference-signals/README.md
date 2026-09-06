# Reference signals

The `CrossDSD_Reference_Signals_v0.2/` directory contains the complete corpus: **217 audio files**, comprising 31 synthetic DXD masters, 93 DSF derivatives (DSD64/128/256) and 93 comparison DXD decodes produced with AudioGate.

The synthetic masters are the references. The AudioGate derivatives support comparisons. No private music recordings are included. Provenance, the Apache 2.0 license, settings, manifests and expected results are described in the [corpus README](CrossDSD_Reference_Signals_v0.2/README.md).

## Install from GitHub

Large audio files are available as [CrossDSD_Reference_Signals_v0.2.zip](https://github.com/JB67-DSD/CrossDSD/releases/download/v1.0.0-rc1/CrossDSD_Reference_Signals_v0.2.zip) in the GitHub Release. After downloading, extract the archive **into `reference-signals/`**, preserving its root directory. Corpus documents and tools are also included in the Git repository.

All public downloads are hosted by the same GitHub repository. A source-only checkout contains the corpus documentation and tools; download the archive to obtain the audio files.

## Verify

From `reference-signals/CrossDSD_Reference_Signals_v0.2/`:

```sh
shasum -a 256 -c SHA256SUMS.txt
```

The structural validator requires Python 3.9 or later and `ffprobe`. For full scientific analyses, also install NumPy, SciPy and FFmpeg; see the corpus README. Audio files and numerical results are unchanged. Chart labels and their generator were translated into English for this public distribution, and the checksum index was updated.

CMake tests recognize this path automatically after reconfiguration. For another location, use `CROSSDSD_REFERENCE_SIGNALS_DIR`.

The v0.2 corpus does not include AudioGate DSD512 derivatives; the engine’s own synthetic tests also cover that rate.
