# CrossDSD API guide

## Choose an integration level

| Requirement | Public header | Starting point |
| --- | --- | --- |
| Convert, analyze or transcode a file | [`convert.h`](../include/crossdsd/convert.h) | `crossdsd_decode_file`, `crossdsd_encode_file`, analysis and transcoding APIs |
| Supply and receive audio blocks | [`stream.h`](../include/crossdsd/stream.h) | Persistent decoding, encoding, transcoding and peak-analysis contexts |
| Read and write audio containers | [`formats.h`](../include/crossdsd/formats.h), [`audio.h`](../include/crossdsd/audio.h) | DSF, WAV/RF64 and AIFF readers and writers, as supported by each interface |
| Integrate individual DSP stages | [`dsp.h`](../include/crossdsd/dsp.h) | Decoder, upsampler and modulators |

The declarations and comments in these headers are the exact reference for the supplied version. API version numbers are: formats 1, audio 4, DSP 7, conversion 9 and streaming 1. They are distinct from the software version (1.0.0).

## Make your first call

The [decoding example](../examples/twistedwave/decode_file.c) and [encoding example](../examples/twistedwave-encoder/encode_file.c) demonstrate complete structure initialization, result checking and error reporting.

Zero-initialize structures, then set `struct_size = sizeof(structure)` where that field is present. Choose the relevant parameters explicitly. Do not reuse a structure copied from another version without checking the supplied header.

## Errors, progress and resources

Operations that can fail return a `crossdsd_result`. Check this result before using any output. The optional `crossdsd_error` diagnostic provides an explanation and, where relevant, a file offset. See [ERRORS.md](ERRORS.md).

File-operation progress callbacks are synchronous: they run on the thread that calls the conversion. Return a non-zero value to continue, or zero to request cancellation. Run processing outside the UI thread to keep a host application responsive.

Destroy each context with its corresponding destruction function. Progressive contexts preserve signal state: use a separate context for each stream and serialize calls on the same context. Follow the buffer capacities and counters documented in `stream.h`, including finalization calls where required.

## Preserve the audio conventions

- DXD files use float32 PCM at 352,800 Hz; internal calculations use double precision.
- The default SACD convention preserves PCM floating-point values above 1.0. Avoid unintended clipping in the host application.
- DSD64 Enhanced is an encoding profile that produces an ordinary DSD64 file.
- DSD transcoding reconstructs the signal internally in double precision before re-encoding it. It does not create detail absent from the original recording.
- Stream APIs do not manage containers or metadata. File-processing APIs handle them according to their contracts.

## Further documentation

- [DSD analysis](DSD_ANALYSIS.md)
- [DSD transcoding](DSD_TRANSCODING.md)
- [DSF format](DSF_CANONICAL_FORMAT.md)
- [Streaming architecture](STREAMING_API_0.6.0.md): historical documentation; consult `stream.h` for the current contract.
