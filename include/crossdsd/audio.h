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

#ifndef CROSSDSD_AUDIO_H
#define CROSSDSD_AUDIO_H

#include "crossdsd/formats.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CROSSDSD_AUDIO_API_VERSION 4u
#define CROSSDSD_DXD_SAMPLE_RATE 352800u

typedef struct crossdsd_wave_writer crossdsd_wave_writer;
typedef struct crossdsd_wave_reader crossdsd_wave_reader;
typedef struct crossdsd_aiff_reader crossdsd_aiff_reader;

typedef enum crossdsd_wave_container {
    CROSSDSD_WAVE_AUTO = 0,
    CROSSDSD_WAVE_RIFF = 1,
    CROSSDSD_WAVE_RF64 = 2
} crossdsd_wave_container;

typedef struct crossdsd_wave_writer_config {
    uint32_t struct_size;
    uint32_t sample_rate_hz;
    uint32_t channel_count;
    uint64_t expected_frame_count;
    crossdsd_wave_container container;
    /* Sony DSF channel type (1..7), or 0 when no layout is known. */
    uint32_t channel_type;
} crossdsd_wave_writer_config;

typedef struct crossdsd_wave_writer_info {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t sample_rate_hz;
    uint32_t channel_count;
    uint32_t bits_per_sample;
    crossdsd_wave_container container;
    uint64_t frame_count;
    uint64_t data_size_bytes;
    uint64_t metadata_size_bytes;
    uint32_t channel_type;
    uint32_t channel_mask;
} crossdsd_wave_writer_info;

typedef struct crossdsd_wave_reader_info {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t sample_rate_hz;
    uint32_t channel_count;
    uint32_t bits_per_sample;
    crossdsd_wave_container container;
    uint64_t frame_count;
    uint64_t data_offset;
    uint64_t data_size_bytes;
    uint64_t metadata_offset;
    uint64_t metadata_size_bytes;
    crossdsd_metadata_status metadata_status;
    uint32_t channel_type;
    uint32_t channel_mask;
} crossdsd_wave_reader_info;

typedef struct crossdsd_aiff_reader_info {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t sample_rate_hz;
    uint32_t channel_count;
    uint32_t bits_per_sample;
    uint64_t frame_count;
    uint64_t data_offset;
    uint64_t data_size_bytes;
    uint64_t metadata_offset;
    uint64_t metadata_size_bytes;
    crossdsd_metadata_status metadata_status;
    /* Sony-compatible layout when it is unambiguous (mono/stereo), else 0. */
    uint32_t channel_type;
} crossdsd_aiff_reader_info;

/* Opens uncompressed integer PCM AIFF/AIFC (`NONE` or `sowt`) as float32. */
CROSSDSD_API crossdsd_result crossdsd_aiff_reader_open_file(
    const char* path,
    crossdsd_aiff_reader** out_reader,
    crossdsd_error* error);

CROSSDSD_API void crossdsd_aiff_reader_close(crossdsd_aiff_reader* reader);

CROSSDSD_API crossdsd_result crossdsd_aiff_reader_get_info(
    const crossdsd_aiff_reader* reader,
    crossdsd_aiff_reader_info* info);

CROSSDSD_API crossdsd_result crossdsd_aiff_reader_read(
    crossdsd_aiff_reader* reader,
    float* interleaved,
    uint64_t frame_capacity,
    uint64_t* frames_read,
    crossdsd_error* error);

CROSSDSD_API crossdsd_result crossdsd_aiff_reader_read_metadata(
    crossdsd_aiff_reader* reader,
    uint8_t* output,
    uint64_t output_capacity,
    uint64_t* bytes_read,
    crossdsd_error* error);

CROSSDSD_API crossdsd_result crossdsd_wave_reader_open_file(
    const char* path,
    crossdsd_wave_reader** out_reader,
    crossdsd_error* error);

CROSSDSD_API void crossdsd_wave_reader_close(crossdsd_wave_reader* reader);

CROSSDSD_API crossdsd_result crossdsd_wave_reader_get_info(
    const crossdsd_wave_reader* reader,
    crossdsd_wave_reader_info* info);

CROSSDSD_API crossdsd_result crossdsd_wave_reader_read(
    crossdsd_wave_reader* reader,
    float* interleaved,
    uint64_t frame_capacity,
    uint64_t* frames_read,
    crossdsd_error* error);

CROSSDSD_API crossdsd_result crossdsd_wave_reader_read_metadata(
    crossdsd_wave_reader* reader,
    uint8_t* output,
    uint64_t output_capacity,
    uint64_t* bytes_read,
    crossdsd_error* error);

/*
 * Opens a streaming IEEE float32 WAVE writer. CROSSDSD_WAVE_AUTO selects RIFF
 * when the declared output fits safely, otherwise RF64. Existing files are
 * replaced; user-facing callers should implement their own overwrite policy.
 */
CROSSDSD_API crossdsd_result crossdsd_wave_writer_open_file(
    const char* path,
    const crossdsd_wave_writer_config* config,
    crossdsd_wave_writer** out_writer,
    crossdsd_error* error);

/*
 * Writes interleaved float32 frames without normalization, clipping or dither.
 * Every finite or non-finite IEEE value is preserved bit-for-bit.
 */
CROSSDSD_API crossdsd_result crossdsd_wave_writer_write(
    crossdsd_wave_writer* writer,
    const float* interleaved,
    uint64_t frame_count,
    crossdsd_error* error);

/* Appends an unmodified ID3v2 tag as a RIFF `id3 ` chunk. Call at most once. */
CROSSDSD_API crossdsd_result crossdsd_wave_writer_write_id3(
    crossdsd_wave_writer* writer,
    const uint8_t* id3_data,
    uint64_t id3_size,
    crossdsd_error* error);

CROSSDSD_API crossdsd_result crossdsd_wave_writer_get_info(
    const crossdsd_wave_writer* writer,
    crossdsd_wave_writer_info* info);

/*
 * Finalizes the size fields and closes the file. The handle is consumed even
 * when finalization fails and must not be used again.
 */
CROSSDSD_API crossdsd_result crossdsd_wave_writer_close(
    crossdsd_wave_writer* writer,
    crossdsd_error* error);

#ifdef __cplusplus
}
#endif

#endif
