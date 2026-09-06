# libcrossdsd-formats error model

The C API never exposes C++ exceptions. Every fallible operation returns a
`crossdsd_result`. When provided, `crossdsd_error` also contains the relevant
file offset and a short diagnostic intended for logs.

| Code | Meaning |
|---|---|
| `CROSSDSD_OK` | Operation completed |
| `CROSSDSD_ERROR_INVALID_ARGUMENT` | Invalid pointer, capacity or seek |
| `CROSSDSD_ERROR_IO` | File open, seek or read failed |
| `CROSSDSD_ERROR_TRUNCATED` | A required region ends outside the file |
| `CROSSDSD_ERROR_INVALID_SIGNATURE` | File is not DSF |
| `CROSSDSD_ERROR_INVALID_CHUNK` | Required chunk or size is inconsistent |
| `CROSSDSD_ERROR_UNSUPPORTED_FORMAT` | DSF version, format or bit order is unsupported |
| `CROSSDSD_ERROR_UNSUPPORTED_RATE` | Rate is not supported by the selected operation |
| `CROSSDSD_ERROR_UNSUPPORTED_CHANNELS` | Channel count is zero or over the safety limit |
| `CROSSDSD_ERROR_INVALID_SAMPLE_COUNT` | Declared samples and payload disagree |
| `CROSSDSD_ERROR_INVALID_METADATA` | Metadata offset is unsafe |
| `CROSSDSD_ERROR_OVERFLOW` | An offset or size calculation would overflow |
| `CROSSDSD_ERROR_END_OF_STREAM` | No valid DSD byte remains |
| `CROSSDSD_ERROR_OUT_OF_MEMORY` | Handle allocation failed |
| `CROSSDSD_ERROR_CANCELLED` | A progress callback requested cancellation |

Invalid ID3 content at a structurally safe offset is deliberately a stream
property (`CROSSDSD_METADATA_INVALID`), not a fatal parsing error.
