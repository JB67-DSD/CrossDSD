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

#ifndef CROSSDSD_STREAM_H
#define CROSSDSD_STREAM_H

#include "crossdsd/dsp.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CROSSDSD_STREAM_API_VERSION 1u
#define CROSSDSD_STREAM_MAX_CHANNELS 256u

typedef struct crossdsd_dsd_to_dxd_stream crossdsd_dsd_to_dxd_stream;
typedef struct crossdsd_dxd_to_dsd_stream crossdsd_dxd_to_dsd_stream;
typedef struct crossdsd_dsd_to_dsd_stream crossdsd_dsd_to_dsd_stream;
typedef struct crossdsd_dsd_peak_stream crossdsd_dsd_peak_stream;

typedef struct crossdsd_dsd_to_dxd_stream_config {
    uint32_t struct_size;
    uint32_t input_sample_rate_hz;
    uint32_t channel_count;
    crossdsd_filter_profile filter_profile;
} crossdsd_dsd_to_dxd_stream_config;

typedef struct crossdsd_dsd_to_dxd_stream_info {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t input_sample_rate_hz;
    uint32_t output_sample_rate_hz;
    uint32_t channel_count;
    uint32_t decimation_factor;
    crossdsd_filter_profile filter_profile;
    double latency_output_samples;
    uint64_t input_dsd_samples_per_channel;
    uint64_t output_dxd_frames;
    uint32_t finished;
    double channel_peaks[CROSSDSD_STREAM_MAX_CHANNELS];
} crossdsd_dsd_to_dxd_stream_info;

/*
 * Creates a file-independent progressive DSD-to-DXD stream. The stream owns
 * all filter state and must be kept alive across successive blocks.
 */
CROSSDSD_API crossdsd_result crossdsd_dsd_to_dxd_stream_create(
    const crossdsd_dsd_to_dxd_stream_config* config,
    crossdsd_dsd_to_dxd_stream** out_stream,
    crossdsd_error* error);

CROSSDSD_API void crossdsd_dsd_to_dxd_stream_destroy(
    crossdsd_dsd_to_dxd_stream* stream);

CROSSDSD_API crossdsd_result crossdsd_dsd_to_dxd_stream_get_info(
    const crossdsd_dsd_to_dxd_stream* stream,
    crossdsd_dsd_to_dxd_stream_info* info);

/*
 * Consumes planar packed DSD, LSB-first, and produces interleaved float32 DXD
 * at 352.8 kHz. Channel N starts at packed_planar + N * packed_stride_bytes.
 * valid_bits_in_last_byte is 8 for ordinary blocks and 1..7 for the final
 * partial byte. A partial final byte seals the stream.
 *
 * output_capacity_frames must be at least
 * ceil(valid DSD samples / decimation_factor) + 1.
 */
CROSSDSD_API crossdsd_result crossdsd_dsd_to_dxd_stream_process(
    crossdsd_dsd_to_dxd_stream* stream,
    const uint8_t* packed_planar,
    uint64_t packed_bytes_per_channel,
    uint64_t packed_stride_bytes,
    uint8_t valid_bits_in_last_byte,
    float* output_interleaved,
    uint64_t output_capacity_frames,
    uint64_t* output_frames,
    crossdsd_error* error);

typedef struct crossdsd_dxd_to_dsd_stream_config {
    uint32_t struct_size;
    uint32_t output_sample_rate_hz;
    uint32_t channel_count;
    crossdsd_encoder_profile encoder_profile;
} crossdsd_dxd_to_dsd_stream_config;

typedef struct crossdsd_dxd_to_dsd_stream_info {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t input_sample_rate_hz;
    uint32_t output_sample_rate_hz;
    uint32_t channel_count;
    uint32_t interpolation_factor;
    crossdsd_encoder_profile encoder_profile;
    double latency_output_samples;
    double maximum_pcm_amplitude;
    uint64_t input_dxd_frames;
    uint64_t output_dsd_samples_per_channel;
    uint64_t output_packed_bytes_per_channel;
    uint32_t finished;
    uint32_t stability_failure;
    uint64_t enhanced_dsd_samples_per_channel;
    uint64_t enhanced_segment_count;
    double channel_input_peaks[CROSSDSD_STREAM_MAX_CHANNELS];
} crossdsd_dxd_to_dsd_stream_info;

/*
 * Creates a progressive DXD-to-DSD stream. Input is always interleaved DXD at
 * 352.8 kHz. Output is planar packed DSD, LSB-first.
 */
CROSSDSD_API crossdsd_result crossdsd_dxd_to_dsd_stream_create(
    const crossdsd_dxd_to_dsd_stream_config* config,
    crossdsd_dxd_to_dsd_stream** out_stream,
    crossdsd_error* error);

CROSSDSD_API void crossdsd_dxd_to_dsd_stream_destroy(
    crossdsd_dxd_to_dsd_stream* stream);

CROSSDSD_API crossdsd_result crossdsd_dxd_to_dsd_stream_get_info(
    const crossdsd_dxd_to_dsd_stream* stream,
    crossdsd_dxd_to_dsd_stream_info* info);

CROSSDSD_API crossdsd_result crossdsd_dxd_to_dsd_stream_get_channel_metrics(
    const crossdsd_dxd_to_dsd_stream* stream,
    uint32_t channel_index,
    crossdsd_modulator_channel_metrics* metrics);

/*
 * Every process call consumes all input frames. The output capacity and stride
 * are expressed in bytes per channel and must be at least:
 *
 *   input_frames * interpolation_factor / 8 + 1
 */
CROSSDSD_API crossdsd_result crossdsd_dxd_to_dsd_stream_process_f32(
    crossdsd_dxd_to_dsd_stream* stream,
    const float* input_interleaved,
    uint64_t input_frames,
    uint8_t* output_packed_planar,
    uint64_t output_capacity_bytes_per_channel,
    uint64_t output_stride_bytes,
    uint64_t* output_bytes_per_channel,
    crossdsd_error* error);

CROSSDSD_API crossdsd_result crossdsd_dxd_to_dsd_stream_process_f64(
    crossdsd_dxd_to_dsd_stream* stream,
    const double* input_interleaved,
    uint64_t input_frames,
    uint8_t* output_packed_planar,
    uint64_t output_capacity_bytes_per_channel,
    uint64_t output_stride_bytes,
    uint64_t* output_bytes_per_channel,
    crossdsd_error* error);

/*
 * Flushes the symmetric end extension. Query get_info before this call; the
 * exact remaining byte count per channel is:
 *
 *   input_dxd_frames * interpolation_factor / 8
 *       - output_packed_bytes_per_channel
 */
CROSSDSD_API crossdsd_result crossdsd_dxd_to_dsd_stream_finish(
    crossdsd_dxd_to_dsd_stream* stream,
    uint8_t* output_packed_planar,
    uint64_t output_capacity_bytes_per_channel,
    uint64_t output_stride_bytes,
    uint64_t* output_bytes_per_channel,
    crossdsd_error* error);

typedef struct crossdsd_dsd_to_dsd_stream_config {
    uint32_t struct_size;
    uint32_t input_sample_rate_hz;
    uint32_t output_sample_rate_hz;
    uint32_t channel_count;
    crossdsd_filter_profile filter_profile;
    crossdsd_encoder_profile encoder_profile;
} crossdsd_dsd_to_dsd_stream_config;

typedef struct crossdsd_dsd_to_dsd_stream_info {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t input_sample_rate_hz;
    uint32_t output_sample_rate_hz;
    uint32_t dxd_sample_rate_hz;
    uint32_t channel_count;
    uint32_t decimation_factor;
    uint32_t interpolation_factor;
    crossdsd_filter_profile filter_profile;
    crossdsd_encoder_profile encoder_profile;
    double decoder_latency_dxd_samples;
    double encoder_latency_dsd_samples;
    uint64_t input_dsd_samples_per_channel;
    uint64_t reconstructed_dxd_frames;
    uint64_t output_dsd_samples_per_channel;
    uint64_t output_packed_bytes_per_channel;
    uint32_t input_sealed;
    uint32_t finished;
    uint32_t stability_failure;
    uint64_t enhanced_dsd_samples_per_channel;
    uint64_t enhanced_segment_count;
    double channel_reconstructed_peaks[CROSSDSD_STREAM_MAX_CHANNELS];
} crossdsd_dsd_to_dsd_stream_info;

/*
 * Creates a direct progressive DSD-to-DSD path. The intermediate reconstructed
 * audio remains float64 in memory and is never materialized as a PCM file.
 */
CROSSDSD_API crossdsd_result crossdsd_dsd_to_dsd_stream_create(
    const crossdsd_dsd_to_dsd_stream_config* config,
    crossdsd_dsd_to_dsd_stream** out_stream,
    crossdsd_error* error);

CROSSDSD_API void crossdsd_dsd_to_dsd_stream_destroy(
    crossdsd_dsd_to_dsd_stream* stream);

CROSSDSD_API crossdsd_result crossdsd_dsd_to_dsd_stream_get_info(
    const crossdsd_dsd_to_dsd_stream* stream,
    crossdsd_dsd_to_dsd_stream_info* info);

CROSSDSD_API crossdsd_result crossdsd_dsd_to_dsd_stream_get_channel_metrics(
    const crossdsd_dsd_to_dsd_stream* stream,
    uint32_t channel_index,
    crossdsd_modulator_channel_metrics* metrics);

/*
 * Consumes planar packed DSD and returns planar packed DSD. Both are LSB-first.
 * A valid_bits_in_last_byte value below 8 seals the input side. The output
 * capacity and stride must be at least:
 *
 *   (ceil(valid_input_bits / decimation_factor) + 1)
 *       * interpolation_factor / 8 + 1
 */
CROSSDSD_API crossdsd_result crossdsd_dsd_to_dsd_stream_process(
    crossdsd_dsd_to_dsd_stream* stream,
    const uint8_t* input_packed_planar,
    uint64_t input_bytes_per_channel,
    uint64_t input_stride_bytes,
    uint8_t valid_bits_in_last_byte,
    uint8_t* output_packed_planar,
    uint64_t output_capacity_bytes_per_channel,
    uint64_t output_stride_bytes,
    uint64_t* output_bytes_per_channel,
    crossdsd_error* error);

CROSSDSD_API crossdsd_result crossdsd_dsd_to_dsd_stream_finish(
    crossdsd_dsd_to_dsd_stream* stream,
    uint8_t* output_packed_planar,
    uint64_t output_capacity_bytes_per_channel,
    uint64_t output_stride_bytes,
    uint64_t* output_bytes_per_channel,
    crossdsd_error* error);

typedef struct crossdsd_dsd_peak_stream_config {
    uint32_t struct_size;
    uint32_t input_sample_rate_hz;
    uint32_t channel_count;
    crossdsd_filter_profile filter_profile;
} crossdsd_dsd_peak_stream_config;

typedef struct crossdsd_dsd_peak_stream_info {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t input_sample_rate_hz;
    uint32_t reconstructed_sample_rate_hz;
    uint32_t channel_count;
    uint32_t decimation_factor;
    crossdsd_filter_profile filter_profile;
    double latency_reconstructed_samples;
    uint64_t input_dsd_samples_per_channel;
    uint64_t reconstructed_frames;
    uint32_t input_sealed;
    uint32_t finished;
    double reconstructed_peak;
    double reconstructed_peak_dbfs;
    double channel_reconstructed_peaks[CROSSDSD_STREAM_MAX_CHANNELS];
} crossdsd_dsd_peak_stream_info;

/*
 * Streaming equivalent of the 0.5.1 maxpeak analysis pass. It measures the
 * maximum absolute value of the reconstructed float64 audio after the selected
 * reconstruction filter. No limiter, interpolation or sample-peak prediction
 * is applied.
 */
CROSSDSD_API crossdsd_result crossdsd_dsd_peak_stream_create(
    const crossdsd_dsd_peak_stream_config* config,
    crossdsd_dsd_peak_stream** out_stream,
    crossdsd_error* error);

CROSSDSD_API void crossdsd_dsd_peak_stream_destroy(
    crossdsd_dsd_peak_stream* stream);

CROSSDSD_API crossdsd_result crossdsd_dsd_peak_stream_get_info(
    const crossdsd_dsd_peak_stream* stream,
    crossdsd_dsd_peak_stream_info* info);

CROSSDSD_API crossdsd_result crossdsd_dsd_peak_stream_process(
    crossdsd_dsd_peak_stream* stream,
    const uint8_t* input_packed_planar,
    uint64_t input_bytes_per_channel,
    uint64_t input_stride_bytes,
    uint8_t valid_bits_in_last_byte,
    crossdsd_error* error);

/* Seals a stream whose final DSD byte was complete. */
CROSSDSD_API crossdsd_result crossdsd_dsd_peak_stream_finish(
    crossdsd_dsd_peak_stream* stream,
    crossdsd_error* error);

#ifdef __cplusplus
}
#endif

#endif
