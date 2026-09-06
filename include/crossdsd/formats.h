/*
 * Copyright 2026-present, Jean-Luc EVRARD.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Author Jean-Luc EVRARD
 */

#ifndef CROSSDSD_FORMATS_H
#define CROSSDSD_FORMATS_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(CROSSDSD_STATIC)
#define CROSSDSD_API
#elif defined(_WIN32) && defined(CROSSDSD_FORMATS_BUILD)
#define CROSSDSD_API __declspec(dllexport)
#elif defined(_WIN32)
#define CROSSDSD_API __declspec(dllimport)
#elif defined(__GNUC__)
#define CROSSDSD_API __attribute__((visibility("default")))
#else
#define CROSSDSD_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define CROSSDSD_FORMATS_API_VERSION 1u
#define CROSSDSD_ERROR_MESSAGE_CAPACITY 192u

typedef struct crossdsd_reader crossdsd_reader;
typedef struct crossdsd_dsf_writer crossdsd_dsf_writer;

typedef enum crossdsd_result {
    CROSSDSD_OK = 0,
    CROSSDSD_ERROR_INVALID_ARGUMENT = 1,
    CROSSDSD_ERROR_IO = 2,
    CROSSDSD_ERROR_TRUNCATED = 3,
    CROSSDSD_ERROR_INVALID_SIGNATURE = 4,
    CROSSDSD_ERROR_INVALID_CHUNK = 5,
    CROSSDSD_ERROR_UNSUPPORTED_FORMAT = 6,
    CROSSDSD_ERROR_UNSUPPORTED_RATE = 7,
    CROSSDSD_ERROR_UNSUPPORTED_CHANNELS = 8,
    CROSSDSD_ERROR_INVALID_SAMPLE_COUNT = 9,
    CROSSDSD_ERROR_INVALID_METADATA = 10,
    CROSSDSD_ERROR_OVERFLOW = 11,
    CROSSDSD_ERROR_END_OF_STREAM = 12,
    CROSSDSD_ERROR_OUT_OF_MEMORY = 13,
    CROSSDSD_ERROR_CANCELLED = 14
} crossdsd_result;

typedef enum crossdsd_dsd_rate {
    CROSSDSD_DSD_RATE_UNKNOWN = 0,
    CROSSDSD_DSD64 = 64,
    CROSSDSD_DSD128 = 128,
    CROSSDSD_DSD256 = 256,
    CROSSDSD_DSD512 = 512
} crossdsd_dsd_rate;

typedef enum crossdsd_bit_order {
    CROSSDSD_BIT_ORDER_UNKNOWN = 0,
    CROSSDSD_BIT_ORDER_LSB_FIRST = 1
} crossdsd_bit_order;

typedef enum crossdsd_metadata_status {
    CROSSDSD_METADATA_NONE = 0,
    CROSSDSD_METADATA_ID3 = 1,
    CROSSDSD_METADATA_INVALID = 2
} crossdsd_metadata_status;

typedef struct crossdsd_error {
    uint32_t struct_size;
    crossdsd_result code;
    uint64_t byte_offset;
    char message[CROSSDSD_ERROR_MESSAGE_CAPACITY];
} crossdsd_error;

typedef struct crossdsd_stream_info {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t format_version;
    uint32_t format_id;
    uint32_t channel_type;
    uint32_t channel_count;
    uint32_t sample_rate_hz;
    crossdsd_dsd_rate dsd_rate;
    crossdsd_bit_order bit_order;
    uint64_t sample_count_per_channel;
    double duration_seconds;
    uint32_t block_size_per_channel;
    uint64_t audio_offset;
    uint64_t audio_size;
    uint64_t stored_bits_per_channel;
    uint64_t padding_bits_per_channel;
    uint64_t metadata_offset;
    uint64_t metadata_size;
    crossdsd_metadata_status metadata_status;
} crossdsd_stream_info;

typedef struct crossdsd_dsf_writer_config {
    uint32_t struct_size;
    uint32_t sample_rate_hz;
    uint32_t channel_count;
    /*
     * DSF channel type. Zero selects 1 for mono and 2 for stereo. It must be
     * supplied explicitly for more than two channels.
     */
    uint32_t channel_type;
    /* Zero selects the interoperable DSF default of 4096 bytes per channel. */
    uint32_t block_size_per_channel;
} crossdsd_dsf_writer_config;

typedef struct crossdsd_dsf_writer_info {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t sample_rate_hz;
    crossdsd_dsd_rate dsd_rate;
    uint32_t channel_count;
    uint32_t channel_type;
    uint32_t block_size_per_channel;
    uint64_t sample_count_per_channel;
    uint64_t stored_bytes_per_channel;
    uint64_t audio_size;
    uint64_t metadata_size;
} crossdsd_dsf_writer_info;

/*
 * Opens and validates a DSF file. On success, out_reader owns an opaque handle.
 * error may be NULL. If supplied, set error.struct_size before the call.
 */
CROSSDSD_API crossdsd_result crossdsd_reader_open_file(
    const char* path,
    crossdsd_reader** out_reader,
    crossdsd_error* error);

CROSSDSD_API void crossdsd_reader_close(crossdsd_reader* reader);

/* Set info.struct_size before calling. */
CROSSDSD_API crossdsd_result crossdsd_reader_get_info(
    const crossdsd_reader* reader,
    crossdsd_stream_info* info);

/*
 * Reads planar packed bytes. Each channel occupies capacity_per_channel bytes
 * in output_planar. Bits are LSB-first. Padding bits in the final returned byte
 * are cleared and valid_bits_in_last_byte reports 1..8 (0 at end of stream).
 */
CROSSDSD_API crossdsd_result crossdsd_reader_read_packed(
    crossdsd_reader* reader,
    uint8_t* output_planar,
    uint64_t capacity_per_channel,
    uint64_t* bytes_read_per_channel,
    uint8_t* valid_bits_in_last_byte,
    crossdsd_error* error);

CROSSDSD_API crossdsd_result crossdsd_reader_seek_sample(
    crossdsd_reader* reader,
    uint64_t sample_offset,
    crossdsd_error* error);

/*
 * Expands packed LSB-first DSD into signed bytes: bit 1 -> +1, bit 0 -> -1.
 */
CROSSDSD_API crossdsd_result crossdsd_expand_dsd_lsb(
    const uint8_t* packed,
    uint64_t valid_bit_count,
    int8_t* expanded,
    uint64_t expanded_capacity);

/* Reads raw metadata bytes without interpreting ID3 frames. */
CROSSDSD_API crossdsd_result crossdsd_reader_read_metadata(
    crossdsd_reader* reader,
    uint8_t* output,
    uint64_t output_capacity,
    uint64_t* bytes_read,
    crossdsd_error* error);

/*
 * Opens a progressive DSF writer. Packed input is planar and LSB-first, just
 * like crossdsd_reader_read_packed output. Existing files are replaced.
 */
CROSSDSD_API crossdsd_result crossdsd_dsf_writer_open_file(
    const char* path,
    const crossdsd_dsf_writer_config* config,
    crossdsd_dsf_writer** out_writer,
    crossdsd_error* error);

/*
 * Writes the same number of packed bytes for every channel. Channel N starts
 * at packed_planar + N * bytes_per_channel. valid_bits_in_last_byte is 1..8.
 * A value below 8 seals the audio stream and is only valid on the final call.
 */
CROSSDSD_API crossdsd_result crossdsd_dsf_writer_write_packed(
    crossdsd_dsf_writer* writer,
    const uint8_t* packed_planar,
    uint64_t bytes_per_channel,
    uint8_t valid_bits_in_last_byte,
    crossdsd_error* error);

/*
 * Stores a complete ID3v2 tag to append after the DSF audio payload. Call at
 * most once and before close. A missing tag is valid.
 */
CROSSDSD_API crossdsd_result crossdsd_dsf_writer_write_id3(
    crossdsd_dsf_writer* writer,
    const uint8_t* id3_data,
    uint64_t id3_size,
    crossdsd_error* error);

/* Set info.struct_size before calling. */
CROSSDSD_API crossdsd_result crossdsd_dsf_writer_get_info(
    const crossdsd_dsf_writer* writer,
    crossdsd_dsf_writer_info* info);

/*
 * Flushes the final padded DSF block, appends metadata, patches all sizes,
 * closes the file and destroys the handle. The handle is consumed even when
 * close returns an error.
 */
CROSSDSD_API crossdsd_result crossdsd_dsf_writer_close(
    crossdsd_dsf_writer* writer,
    crossdsd_error* error);

CROSSDSD_API const char* crossdsd_result_string(crossdsd_result result);

#ifdef __cplusplus
}
#endif

#endif
