# DSD-to-DSD transcoding — CrossDSD 0.7.2

> Historical documentation for version 0.7.2. The current release also supports DSD512; see the [command-line reference](CROSSDSD_COMMAND_LINE_REFERENCE.md) and the supplied public headers.

CrossDSD can directly convert a DSD64, DSD128 or DSD256 DSF file to any of these three output rates:

```text
crossdsd transcode input.dsf output.dsf --dsd-rate=128
```

“Directly” means that no temporary WAV/DXD file is created. The engine still reconstructs the audio signal in `float64`, block by block, before remodulating it to DSD. Memory usage therefore remains constant regardless of programme duration.

ID3 metadata and the DSF channel type are preserved. Output duration matches the reconstructed DXD representation, to the nearest DXD sample.

CrossDSD sets the ID3 `TFLT` (`File Type`) field to the output DSD rate: `DSD64`, `DSD128` or `DSD256`. The Enhanced profile is recorded in `Encoder Settings`, because it is not a separate file type.

## Rates and Enhanced mode

```text
--dsd-rate=64
--dsd-rate=128
--dsd-rate=256
```

The output rate is required and is independent of the input rate. All nine DSD64/128/256 combinations follow the same processing path.

Segmented mode is available for DSD64 output:

```text
crossdsd transcode input.dsf output.dsf --dsd-rate=64 --enhanced
```

As with `encode`, `--enhanced` is silently accepted with DSD128 or DSD256, but output then uses the corresponding Standard modulator.

## Reconstructed-peak ceiling

```text
--maxpeak=<dBFS>
```

When necessary, this option attenuates the reconstructed audio before DSD remodulation so that its measured peak does not exceed the specified value. The value must be finite and no greater than `0 dBFS`.

Examples:

```text
crossdsd transcode input.dsf output.dsf --dsd-rate=128 --maxpeak=0
crossdsd transcode input.dsf output.dsf --dsd-rate=64 --maxpeak=-1
```

The applied gain is:

```text
gain_dB = min(0, requested_maxpeak - measured_reconstructed_peak)
```

CrossDSD therefore never increases the level:

| Reconstructed peak | Target | Gain |
|---:|---:|---:|
| +1.7 dBFS | 0 dBFS | −1.7 dB |
| +1.7 dBFS | −1 dBFS | −2.7 dB |
| −0.4 dBFS | 0 dBFS | 0 dB |
| −0.4 dBFS | −1 dBFS | −0.6 dB |
| −3 dBFS | −1 dBFS | 0 dB |

The measurement uses the reference decoder and its Standard audio filter, excluding the ultrasonic noise inherent in the DSD stream.

### Why two passes?

Uniform attenuation requires knowing the programme’s maximum level before encoding its beginning. With `--maxpeak`, CrossDSD therefore performs:

1. An analysis pass: DSD → filtered reconstruction → peak measurement.
2. A processing pass: DSD → reconstruction → fixed gain → modulation → DSF.

Both passes operate on blocks. The complete file is never held in memory, and no dynamic limiter or change to relative dynamics is applied.

Without `--maxpeak`, CrossDSD preserves the original level and performs a single pass.

### Quick Search and Full Search

CrossDSD 0.7.0 defaults to adaptive search when `--maxpeak` is enabled. A lightweight DSD scan selects likely global and local maxima, then the exact reconstruction filter analyzes those regions with filter warm-up. On the long, demanding music recordings tested, this stage was approximately 2.1 to 2.8 times faster.

```text
--quicksearch   recommended adaptive search (default)
--fullsearch    original exhaustive analysis
```

`--quicksearch` can be specified explicitly to make the choice visible in a script. `--fullsearch` examines every reconstructed sample and provides exhaustive coverage at the cost of longer analysis time.

Quick Search found exactly the same peak, channel and sample on the 31 scientific reference masters and the demanding validation music files. On continuously demanding signals, it automatically falls back to nearly exhaustive analysis.

## Progress, cancellation and reporting

Progress is monotonic. In two-pass mode, analysis occupies the first half and transcoding the second. `100%` is reported only after the DSF file has been successfully finalized.

`Ctrl-C` cancels the operation and removes the partial file.

A JSON report can be requested:

```text
crossdsd transcode input.dsf output.dsf --dsd-rate=256 --maxpeak=0 --report=report.json
```

It includes the reconstructed peak in linear units and dBFS, the target, actual attenuation, DSD rates, duration, analysis and transcoding times, and modulator metrics.

## C API

The public function is:

```c
crossdsd_result crossdsd_transcode_file(
    const char* input_path,
    const char* output_path,
    const crossdsd_transcode_file_config* config,
    crossdsd_transcode_file_report* report,
    crossdsd_error* error);
```

`max_peak_enabled` explicitly distinguishes an absent ceiling from a target of `0 dBFS`. A zero-initialized configuration selects Quick Search; `full_peak_search = 1` forces exhaustive analysis. The progress callback and cancellation mechanism follow the same contract as `crossdsd_encode_file`.
