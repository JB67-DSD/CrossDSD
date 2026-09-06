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

#ifndef CROSSDSD_DSP_H
#define CROSSDSD_DSP_H

#include "crossdsd/formats.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CROSSDSD_DSP_API_VERSION 7u

typedef struct crossdsd_decoder crossdsd_decoder;
typedef struct crossdsd_modulator crossdsd_modulator;
typedef struct crossdsd_upsampler crossdsd_upsampler;
typedef struct crossdsd_encoder crossdsd_encoder;

typedef enum crossdsd_level_mode {
    CROSSDSD_LEVEL_DSD_RAW = 0,
    CROSSDSD_LEVEL_PCM_CONTINUITY = 1
} crossdsd_level_mode;

typedef enum crossdsd_filter_profile {
    CROSSDSD_FILTER_STANDARD = 0,
    CROSSDSD_FILTER_RAW = 1
} crossdsd_filter_profile;

typedef struct crossdsd_decoder_config {
    uint32_t struct_size;
    uint32_t input_sample_rate_hz;
    uint32_t channel_count;
    crossdsd_level_mode level_mode;
    crossdsd_filter_profile filter_profile;
} crossdsd_decoder_config;

typedef struct crossdsd_decoder_info {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t input_sample_rate_hz;
    uint32_t output_sample_rate_hz;
    uint32_t channel_count;
    uint32_t decimation_factor;
    uint32_t stage_count;
    crossdsd_filter_profile filter_profile;
    double applied_gain;
    double latency_output_samples;
    uint64_t input_samples_consumed_per_channel;
    uint64_t output_samples_produced_per_channel;
} crossdsd_decoder_info;

typedef enum crossdsd_modulator_profile {
    CROSSDSD_MODULATOR_CRFB_ORDER7_HINF_1_32 = 0,
    CROSSDSD_MODULATOR_DSD128_CRFB_ORDER5_HINF_1_30 = 1,
    CROSSDSD_MODULATOR_DSD256_CRFB_ORDER7_HINF_1_20 = 2,
    CROSSDSD_MODULATOR_DSD512_CRFB_ORDER7_HINF_1_20 = 3
} crossdsd_modulator_profile;

typedef struct crossdsd_modulator_config {
    uint32_t struct_size;
    uint32_t sample_rate_hz;
    uint32_t channel_count;
    crossdsd_modulator_profile profile;
} crossdsd_modulator_config;

typedef struct crossdsd_modulator_info {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t sample_rate_hz;
    uint32_t channel_count;
    crossdsd_modulator_profile profile;
    uint32_t modulator_order;
    double h_inf;
    double maximum_pcm_amplitude;
    uint64_t input_samples_consumed_per_channel;
    uint64_t output_samples_produced_per_channel;
    uint32_t stability_failure;
} crossdsd_modulator_info;

typedef struct crossdsd_modulator_channel_metrics {
    uint32_t struct_size;
    uint32_t channel_index;
    double maximum_absolute_input;
    double maximum_absolute_state;
    double maximum_absolute_quantizer_input;
    uint64_t maximum_identical_bit_run;
    double one_density;
} crossdsd_modulator_channel_metrics;

typedef struct crossdsd_upsampler_config {
    uint32_t struct_size;
    uint32_t input_sample_rate_hz;
    uint32_t output_sample_rate_hz;
    uint32_t channel_count;
} crossdsd_upsampler_config;

typedef struct crossdsd_upsampler_info {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t input_sample_rate_hz;
    uint32_t output_sample_rate_hz;
    uint32_t channel_count;
    uint32_t interpolation_factor;
    uint32_t interpolation_stage_count;
    uint32_t conditioning_filter_taps;
    uint32_t interpolation_filter_taps[6];
    double passband_edge_hz;
    double stopband_edge_hz;
    double stopband_attenuation_db;
    double latency_output_samples;
    uint64_t input_samples_consumed_per_channel;
    uint64_t output_samples_produced_per_channel;
} crossdsd_upsampler_info;

typedef enum crossdsd_encoder_profile {
    CROSSDSD_ENCODER_STANDARD = 0,
    CROSSDSD_ENCODER_DSD64_ENHANCED = 1
} crossdsd_encoder_profile;

typedef struct crossdsd_encoder_config {
    uint32_t struct_size;
    uint32_t input_sample_rate_hz;
    uint32_t output_sample_rate_hz;
    uint32_t channel_count;
    crossdsd_modulator_profile modulator_profile;
    crossdsd_encoder_profile encoder_profile;
} crossdsd_encoder_config;

typedef struct crossdsd_encoder_info {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t input_sample_rate_hz;
    uint32_t output_sample_rate_hz;
    uint32_t channel_count;
    uint32_t interpolation_factor;
    uint32_t edge_extension_input_samples;
    double latency_output_samples;
    double maximum_pcm_amplitude;
    uint64_t input_samples_consumed_per_channel;
    uint64_t dsd_samples_produced_per_channel;
    uint64_t packed_bytes_produced_per_channel;
    uint32_t finished;
    uint32_t stability_failure;
    crossdsd_encoder_profile encoder_profile;
    uint64_t enhanced_dsd_samples_per_channel;
    uint64_t enhanced_segment_count;
} crossdsd_encoder_info;

/*
 * Creates the deterministic float64 reference decoder. Config and info structs
 * are versioned by struct_size, as in the formats API.
 */
CROSSDSD_API crossdsd_result crossdsd_decoder_create(
    const crossdsd_decoder_config* config,
    crossdsd_decoder** out_decoder,
    crossdsd_error* error);

CROSSDSD_API void crossdsd_decoder_destroy(crossdsd_decoder* decoder);

CROSSDSD_API crossdsd_result crossdsd_decoder_get_info(
    const crossdsd_decoder* decoder,
    crossdsd_decoder_info* info);

/*
 * Returns the cascade's filter-only response at an analogue frequency.
 * The separately reported applied_gain is intentionally not included.
 */
CROSSDSD_API crossdsd_result crossdsd_decoder_frequency_response(
    const crossdsd_decoder* decoder,
    double frequency_hz,
    double* magnitude,
    double* phase_radians);

/*
 * Consumes planar signed DSD samples (-1 or +1) and returns planar float64 DXD.
 * Channel strides are expressed in samples. output_capacity_per_channel must
 * be at least ceil(input_count_per_channel / decimation_factor) + 1.
 */
CROSSDSD_API crossdsd_result crossdsd_decoder_process(
    crossdsd_decoder* decoder,
    const int8_t* input_planar,
    uint64_t input_count_per_channel,
    uint64_t input_stride,
    double* output_planar,
    uint64_t output_capacity_per_channel,
    uint64_t output_stride,
    uint64_t* output_count_per_channel,
    crossdsd_error* error);

/*
 * Progressive DSD64 reference modulator. Input is planar float64 at the DSD
 * rate in PCM-continuity units: 1.0 is 0 dB SACD. This low-level stage has
 * limited internal headroom for interpolation transients; the complete
 * encoder enforces 10^(3.1/20) on the source DXD. Output is planar signed
 * DSD (-1 or +1).
 *
 * This low-level stage intentionally does not perform interpolation. The
 * progressive DXD encoder feeds its oversampled output into this API.
 */
CROSSDSD_API crossdsd_result crossdsd_modulator_create(
    const crossdsd_modulator_config* config,
    crossdsd_modulator** out_modulator,
    crossdsd_error* error);

CROSSDSD_API void crossdsd_modulator_destroy(crossdsd_modulator* modulator);

CROSSDSD_API crossdsd_result crossdsd_modulator_get_info(
    const crossdsd_modulator* modulator,
    crossdsd_modulator_info* info);

CROSSDSD_API crossdsd_result crossdsd_modulator_get_channel_metrics(
    const crossdsd_modulator* modulator,
    uint32_t channel_index,
    crossdsd_modulator_channel_metrics* metrics);

CROSSDSD_API crossdsd_result crossdsd_modulator_process(
    crossdsd_modulator* modulator,
    const double* input_planar,
    uint64_t input_count_per_channel,
    uint64_t input_stride,
    int8_t* output_planar,
    uint64_t output_capacity_per_channel,
    uint64_t output_stride,
    crossdsd_error* error);

CROSSDSD_API crossdsd_result crossdsd_modulator_reset(
    crossdsd_modulator* modulator);

/*
 * Progressive DSD64 interpolation stage. It accepts planar float32 DXD at
 * 352.8 kHz and produces planar float64 at 2.8224 MHz. Every input sample
 * produces exactly eight output samples; filter latency remains in the stream
 * and is reported explicitly.
 */
CROSSDSD_API crossdsd_result crossdsd_upsampler_create(
    const crossdsd_upsampler_config* config,
    crossdsd_upsampler** out_upsampler,
    crossdsd_error* error);

CROSSDSD_API void crossdsd_upsampler_destroy(crossdsd_upsampler* upsampler);

CROSSDSD_API crossdsd_result crossdsd_upsampler_get_info(
    const crossdsd_upsampler* upsampler,
    crossdsd_upsampler_info* info);

CROSSDSD_API crossdsd_result crossdsd_upsampler_frequency_response(
    const crossdsd_upsampler* upsampler,
    double frequency_hz,
    double* magnitude,
    double* phase_radians);

CROSSDSD_API crossdsd_result crossdsd_upsampler_process(
    crossdsd_upsampler* upsampler,
    const float* input_planar,
    uint64_t input_count_per_channel,
    uint64_t input_stride,
    double* output_planar,
    uint64_t output_capacity_per_channel,
    uint64_t output_stride,
    uint64_t* output_count_per_channel,
    crossdsd_error* error);

/*
 * Float64 variant used by lossless internal processing paths such as direct
 * DSD-to-DSD transcoding. State and output are identical to the float32 entry
 * point when its input values are exactly representable as float32.
 */
CROSSDSD_API crossdsd_result crossdsd_upsampler_process_f64(
    crossdsd_upsampler* upsampler,
    const double* input_planar,
    uint64_t input_count_per_channel,
    uint64_t input_stride,
    double* output_planar,
    uint64_t output_capacity_per_channel,
    uint64_t output_stride,
    uint64_t* output_count_per_channel,
    crossdsd_error* error);

CROSSDSD_API crossdsd_result crossdsd_upsampler_reset(
    crossdsd_upsampler* upsampler);

/*
 * Complete progressive DSD encoder. Input is planar float32 DXD at
 * 352.8 kHz. Output is planar packed DSD, LSB-first. DSD64 Enhanced uses a
 * persistent segmented look-ahead trajectory; it is not available at DSD128
 * or DSD256. A process call consumes
 * the complete input block and may retain an incomplete output byte.
 * output_capacity_per_channel and output_stride must be at least
 * input_count_per_channel + 1 for non-empty process calls.
 */
CROSSDSD_API crossdsd_result crossdsd_encoder_create(
    const crossdsd_encoder_config* config,
    crossdsd_encoder** out_encoder,
    crossdsd_error* error);

CROSSDSD_API void crossdsd_encoder_destroy(crossdsd_encoder* encoder);

CROSSDSD_API crossdsd_result crossdsd_encoder_get_info(
    const crossdsd_encoder* encoder,
    crossdsd_encoder_info* info);

CROSSDSD_API crossdsd_result crossdsd_encoder_get_channel_metrics(
    const crossdsd_encoder* encoder,
    uint32_t channel_index,
    crossdsd_modulator_channel_metrics* metrics);

CROSSDSD_API crossdsd_result crossdsd_encoder_process(
    crossdsd_encoder* encoder,
    const float* input_planar,
    uint64_t input_count_per_channel,
    uint64_t input_stride,
    uint8_t* output_planar,
    uint64_t output_capacity_per_channel,
    uint64_t output_stride,
    uint64_t* output_bytes_per_channel,
    crossdsd_error* error);

/*
 * Float64 DXD entry point for internal high-precision pipelines. It preserves
 * the same level domain, filtering, alignment and packed output contract as
 * crossdsd_encoder_process.
 */
CROSSDSD_API crossdsd_result crossdsd_encoder_process_f64(
    crossdsd_encoder* encoder,
    const double* input_planar,
    uint64_t input_count_per_channel,
    uint64_t input_stride,
    uint8_t* output_planar,
    uint64_t output_capacity_per_channel,
    uint64_t output_stride,
    uint64_t* output_bytes_per_channel,
    crossdsd_error* error);

/*
 * Completes the symmetric end extension and returns all remaining packed
 * bytes. The required capacity per channel is
 * input_samples_consumed_per_channel - packed_bytes_produced_per_channel.
 * The handle remains valid for inspection or reset after this call.
 */
CROSSDSD_API crossdsd_result crossdsd_encoder_finish(
    crossdsd_encoder* encoder,
    uint8_t* output_planar,
    uint64_t output_capacity_per_channel,
    uint64_t output_stride,
    uint64_t* output_bytes_per_channel,
    crossdsd_error* error);

CROSSDSD_API crossdsd_result crossdsd_encoder_reset(
    crossdsd_encoder* encoder);

#ifdef __cplusplus
}
#endif

#endif
