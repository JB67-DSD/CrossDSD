# CrossDSD reconstructed-peak analysis

CrossDSD can measure a DSF file without creating a DXD or DSF output:

```sh
crossdsd analyze input.dsf
```

The analysis reconstructs the audio at 352.8 kHz with the validated Standard
filter and PCM-continuity level. It reports the global peak, every channel peak,
and their positions on the reconstructed-audio timeline. Raw one-bit values and
ultrasonic modulation noise are not treated as PCM amplitudes.

## Search policies

Adaptive search is the default:

```sh
crossdsd analyze input.dsf --quicksearch
```

It performs a low-cost scan, selects candidate regions, and reconstructs those
regions precisely. The existing scientific corpus established this policy for
peak-controlled DSD transcoding.

Exhaustive search reconstructs the complete programme:

```sh
crossdsd analyze input.dsf --fullsearch
```

## JSON report

```sh
crossdsd analyze input.dsf --report=analysis.json
crossdsd analyze input.dsf --report=-
```

The report includes:

- DSD rate, channel count and DSF channel type;
- duration and reconstructed frame count;
- quick or full search policy;
- analysis duration, real-time factor and worker count;
- analyzed input fraction and quick-scan duration;
- global reconstructed peak, channel and time;
- reconstructed peak, frame and time for every channel.

Peak levels are expressed in dBFS after reconstructed-audio filtering. Values
above 0 dBFS are valid measurements and are never clipped by the analyzer.

The C API is `crossdsd_analyze_file()` in `crossdsd/convert.h`. It uses bounded
memory, supports progress reporting and can be cancelled synchronously through
the standard CrossDSD callback contract.

## Album and batch analysis

Pass several ordered DSF paths to analyze them as one album session:

```sh
crossdsd analyze track01.dsf track02.dsf track03.dsf \
  --report=album-analysis.json
```

Shell expansion can select a directory on macOS and Linux:

```sh
crossdsd analyze "/Music/Album/"*.dsf --report=album-analysis.json
```

The default recommendation target is 0 dBFS. Select another non-positive
target explicitly:

```sh
crossdsd analyze "/Music/Album/"*.dsf \
  --album-target=-1 --report=album-analysis.json
```

The batch report supplies two policies without modifying audio:

- `individual_gain_db`: the attenuation needed by each track independently;
- `common_gain_db`: the single attenuation required by the loudest track,
  suitable for preserving all inter-track and album-level relationships.

Neither recommendation can add gain. Batch analysis is sequential across files
to bound memory and hardware pressure; each file retains the adaptive internal
parallelism of the analysis engine. Progress is monotonic over the complete
ordered set and cancellation stops the active file and remaining queue.

The equivalent C API is `crossdsd_analyze_batch()`. The caller supplies an
ordered path array, one `crossdsd_analyze_file_report` per path and one gain
slot per path. No file list or report storage is retained by the library after
the call returns.
