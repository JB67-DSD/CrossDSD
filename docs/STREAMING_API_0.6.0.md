# CrossDSD 0.6.0 — streaming architecture

> Historical documentation. For the current distribution, start with [GETTING_STARTED.md](GETTING_STARTED.md) and [API_GUIDE.md](API_GUIDE.md). The applicable license is [Apache 2.0](../LICENSE).

## Purpose

The 0.6.0 branch exposes progressive processing engines without requiring DSF, WAV or RF64 files. A host application can feed CrossDSD in blocks, retain a context between calls and immediately retrieve the output blocks.

The 0.5.1 file APIs remain available and serve as the regression reference.

## Shared invariants

- Memory usage is proportional to block size, never to programme duration.
- Filter and modulator state is preserved between calls.
- Results are independent of block boundaries.
- No implicit normalization, clipping, limiting or dithering is applied.
- Cumulative counters and latency are available through an information structure.
- Errors are synchronous and context destruction is explicit.
- C structures are versioned through `struct_size`, with a portable macOS/Windows API.

## First component: DSD to DXD

`crossdsd_dsd_to_dxd_stream` accepts packed planar DSD, LSB-first, and produces interleaved float32 DXD at 352.8 kHz. This corresponds directly to buffers typically exchanged with an audio editor:

1. Create a context with the DSD rate, channel count and filter.
2. Call `crossdsd_dsd_to_dxd_stream_process` repeatedly.
3. Optionally query counters, peaks and latency.
4. Destroy the context.

A final partial byte seals the stream. This API creates no file or metadata: the host application remains responsible for the container.

## Second component: DXD to DSD

`crossdsd_dxd_to_dsd_stream` accepts interleaved float32 or float64 DXD at 352.8 kHz and produces packed planar DSD, LSB-first. The same contract covers DSD64 Standard, DSD64 Enhanced, DSD128 and DSD256.

Finalization is explicit: `crossdsd_dxd_to_dsd_stream_finish` symmetrically extends the signal boundaries, flushes the filters and returns the final bytes. Counters allow the exact remaining capacity to be calculated.

## Third component: DSD to DSD

`crossdsd_dsd_to_dsd_stream` combines the decoder and encoder in a single persistent context. Input and output are packed planar DSD, LSB-first. The intermediate representation remains float64 and is never written to a PCM file.

All nine DSD64/DSD128/DSD256 pairs are available. DSD64 Enhanced can be selected for output. Finalization flushes only the encoder, matching the 0.5.1 file engine, which preserves the exact reconstructed duration.

## Fourth component: reconstructed-peak analysis

`crossdsd_dsd_peak_stream` reproduces the first `maxpeak` pass of the 0.5.1 engine exactly. It measures the absolute maximum of the filtered, reconstructed float64 signal, globally and per channel, and reports it as an amplitude and in dBFS.

This component intentionally performs no inter-sample prediction, additional oversampling or temporal weighting. It provides a stable reference against which other measurement strategies can later be compared.

## Next steps identified in version 0.6.0

1. Replay callbacks for two-pass `maxpeak` processing.
2. An ID3 metadata layer and `TSSE` processing history — implemented in the 0.7.0 file APIs; streaming APIs intentionally remain independent of containers and metadata.
3. Connect the file APIs and the macOS application.
