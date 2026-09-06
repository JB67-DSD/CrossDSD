# CrossDSD DSF canonical format

## Scope

`libcrossdsd-formats` version 0.1 reads DSF version 1, format ID 0, for DSD64,
DSD128 and DSD256. It accepts 1 to 256 declared channels. The initial validation
corpus is mono and stereo; the storage and API do not impose a stereo limit.

The reader never filters, changes level, decimates or converts DSD to PCM.

## Required chunks and validation

All integer fields are unsigned little-endian.

1. `DSD ` starts at byte 0 and has a 28-byte chunk size. Its declared file size
   must equal the physical size. A non-zero metadata pointer must identify the
   first byte immediately following `data` and remain inside the file.
2. `fmt ` starts at byte 28 and has a 52-byte chunk size. Version must be 1,
   format ID 0, channel count 1 through 256, bit order field 1, rate one of
   2,822,400, 5,644,800 or 11,289,600 Hz, and block size non-zero.
3. `data` starts at byte 80. Its chunk size includes its 12-byte header. Its
   payload normally holds exactly `ceil(valid_bytes / block_size)` complete
   block groups. For compatibility with field files, one additional complete
   trailing block group is accepted as padding. A partial group, a short
   payload or more than one additional group remains invalid.

Calculations are checked before addition and multiplication. Truncation,
contradictory sizes, unsupported values and unsafe offsets return stable error
codes and a byte offset.

## Audio layout

Within each DSF block group, one complete block is stored for channel 0, then
one for channel 1, and so on. The next group repeats that order. The canonical
API removes this physical interleaving and returns planar buffers.

Bits in every byte are chronological from least-significant to
most-significant bit. Packed output preserves that order. Control expansion is:

- DSD bit `1` becomes signed byte `+1`;
- DSD bit `0` becomes signed byte `-1`.

The declared sample count is authoritative when the payload can contain it.
Storage is rounded first to a byte and then to a complete DSF block per
channel. `padding_bits_per_channel` reports all physically stored bits after
the declared signal, including a tolerated trailing block group. Packed reads
never return complete padding bytes; unused high bits in the final partial
byte are cleared and reported separately.

## Progressive reads

`crossdsd_reader_read_packed` accepts any positive byte capacity per channel.
The result is independent of the DSF block size and may cross any number of
physical blocks. Concatenating each channel's returned bytes reproduces a
complete read byte-for-byte. Only the last returned byte can be partial.

Seeking is expressed in DSD samples and is byte-aligned in API version 1.

## Metadata

Metadata is separate from audio. The public API exposes its offset, size,
status and raw bytes. A valid `ID3` signature produces `CROSSDSD_METADATA_ID3`.
A malformed signature produces `CROSSDSD_METADATA_INVALID` while audio remains
readable. An unsafe metadata offset rejects the file.
