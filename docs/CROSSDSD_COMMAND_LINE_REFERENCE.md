# CrossDSD 1.0.0 — command-line reference

The `crossdsd` executable provides five audio operations and a version command. With no arguments, it displays help. Paths may contain spaces and must be quoted in that case.

## Version

```text
crossdsd --version
```

## Decode DSF to DXD

```text
crossdsd decode INPUT.dsf OUTPUT.wav [options]
```

The output is an IEEE float32 WAV or RF64 file at 352.8 kHz. Levels are neither normalized nor clipped, and ID3 metadata is preserved.

- `--profile=standard|raw`: reconstruction filter; defaults to `standard`.
- `--container=auto|wav|rf64`: output container; defaults to `auto`.
- `--backend=auto|cpu|metal`: processing backend; defaults to `auto`. Metal is available only on macOS; Windows uses the CPU.
- `--report=FILE`: JSON report; `--report=-` writes it to standard output.
- `--overwrite`: allows an existing output file to be replaced.

Example:

```text
crossdsd decode album.dsf album.wav --profile=standard --report=decode.json
```

## Encode DXD to DSD

```text
crossdsd encode INPUT.wav OUTPUT.dsf --dsd-rate=RATE [options]
```

Input must be an IEEE float32 WAV/RF64 file at 352.8 kHz. Float32 input at 705.6 kHz is also accepted for DSD512 output.

- `--dsd-rate=64|128|256|512`: required output rate.
- `--enhanced`: segmented DSD64 Enhanced profile. Ignored for other rates.
- `--channel-type=N`: explicit DSF channel type, required when the multichannel layout cannot be inferred.
- `--report=FILE` or `--report=-`: JSON report.
- `--overwrite`: allows an existing output file to be replaced.

Example:

```text
crossdsd encode master.wav master.dsf --dsd-rate=256 --report=encode.json
```

## Convert AIFF to DXD

```text
crossdsd upscale INPUT.aiff OUTPUT.wav [options]
```

Input must be 16-, 24- or 32-bit integer PCM AIFF/AIFC at 44.1 kHz, uncompressed (`NONE`) or little-endian (`sowt`). Output is float32 WAV/RF64 at 352.8 kHz. Level, duration and the ID3 block are preserved.

- `--container=auto|wav|rf64`: output container; defaults to `auto`.
- `--overwrite`: allows an existing output file to be replaced.

## Transcode DSD to DSD

```text
crossdsd transcode INPUT.dsf OUTPUT.dsf --dsd-rate=RATE [options]
```

All combinations of DSD64, DSD128, DSD256 and DSD512 are accepted. The signal is reconstructed in memory and re-encoded without an intermediate PCM file.

- `--dsd-rate=64|128|256|512`: required output rate.
- `--enhanced`: DSD64 Enhanced profile; ignored for other rates.
- `--maxpeak=DBFS`: uniform attenuation to keep the measured reconstructed peak at or below the target. The value must be finite and no greater than 0 dBFS. No positive gain is applied.
- `--quicksearch`: adaptive peak search; the default method with `--maxpeak`.
- `--fullsearch`: exhaustive reconstruction for peak search.
- `--channel-type=N`: overrides the source DSF channel type.
- `--report=FILE` or `--report=-`: JSON report.
- `--overwrite`: allows an existing output file to be replaced.

Example:

```text
crossdsd transcode source.dsf result.dsf --dsd-rate=128   --maxpeak=-1 --quicksearch --report=transcode.json
```

## Analyze a file, batch or album

```text
crossdsd analyze INPUT1.dsf [INPUT2.dsf ...] [options]
```

Analysis does not create an audio file. It measures the reconstructed signal’s peak, position and channel. With multiple inputs, it provides individual gains and a common gain that preserves their relative levels.

- `--quicksearch`: adaptive search; the default method.
- `--fullsearch`: exhaustive reconstruction.
- `--album-target=DBFS`: recommendation target; defaults to 0 dBFS. The value must be finite and no greater than zero.
- `--report=FILE` or `--report=-`: individual or batch JSON report.

Example:

```text
crossdsd analyze track01.dsf track02.dsf track03.dsf   --album-target=-1 --quicksearch --report=album.json
```

## Common behaviour

- Commands that create a file refuse to overwrite an existing output unless `--overwrite` is specified.
- Output is first created under a temporary name and moved into place only after successful completion.
- `Ctrl+C` requests clean cancellation and removes incomplete output.
- JSON reports contain formats, durations, performance, levels, metadata and operation-specific measurements.

## Public C API

The SDK installs headers under `include/crossdsd` and provides both static and shared libraries.

- `formats.h`: progressive DSF reading/writing, metadata access and error codes.
- `audio.h`: AIFF/AIFC and WAV/RF64 reading, float32 WAV/RF64 writing.
- `convert.h`: file-level decoding, encoding, transcoding, individual/batch analysis and AIFF-to-DXD conversion.
- `stream.h`: progressive DSD-to-DXD, DXD-to-DSD and DSD-to-DSD streams, and peak analysis.
- `dsp.h`: low-level decoder, upsampler, modulator and encoder.

Zero-initialize configuration structures and set their `struct_size` field. Long-running operations accept a progress callback: return a non-zero value to continue, or zero to request clean cancellation.
