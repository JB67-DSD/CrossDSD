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

#ifndef CROSSDSD_CONVERT_H
#define CROSSDSD_CONVERT_H

#include "crossdsd/audio.h"
#include "crossdsd/dsp.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CROSSDSD_CONVERT_API_VERSION 9u
#define CROSSDSD_MAX_CHANNELS 256u

typedef enum crossdsd_backend {
    CROSSDSD_BACKEND_AUTO = 0,
    CROSSDSD_BACKEND_CPU = 1,
    CROSSDSD_BACKEND_METAL = 2
} crossdsd_backend;

/*
 * Reports a completed fraction in [0.0, 1.0]. Return non-zero to continue or
 * zero to cancel. The callback runs synchronously on the conversion's calling
 * thread.
 */
typedef int (*crossdsd_progress_callback)(
    void* user_data, double completed_fraction);

typedef struct crossdsd_decode_file_config {
    uint32_t struct_size;
    crossdsd_filter_profile filter_profile;
    crossdsd_wave_container container;
    crossdsd_backend backend;
    uint64_t packed_chunk_bytes_per_channel;
    crossdsd_progress_callback progress_callback;
    void* progress_user_data;
    uint32_t fixed_gain_enabled;
    double fixed_gain_db;
} crossdsd_decode_file_config;

typedef struct crossdsd_decode_file_report {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t input_sample_rate_hz;
    uint32_t output_sample_rate_hz;
    uint32_t channel_count;
    uint32_t stage_count;
    crossdsd_filter_profile filter_profile;
    crossdsd_wave_container container;
    crossdsd_backend backend;
    uint64_t input_samples_per_channel;
    uint64_t output_frames;
    uint64_t output_size_bytes;
    uint64_t metadata_size_bytes;
    crossdsd_metadata_status metadata_status;
    double duration_seconds;
    double decode_seconds;
    double preparation_seconds;
    double processing_seconds;
    double writing_seconds;
    double realtime_factor;
    double latency_output_samples;
    double channel_peaks[CROSSDSD_MAX_CHANNELS];
    uint32_t fixed_gain_enabled;
    double requested_fixed_gain_db;
} crossdsd_decode_file_report;

typedef struct crossdsd_encode_file_config {
    uint32_t struct_size;
    crossdsd_dsd_rate dsd_rate;
    uint32_t channel_type;
    uint64_t frame_chunk_count;
    crossdsd_progress_callback progress_callback;
    void* progress_user_data;
    crossdsd_encoder_profile encoder_profile;
    uint32_t fixed_gain_enabled;
    double fixed_gain_db;
} crossdsd_encode_file_config;

typedef struct crossdsd_encode_file_report {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t input_sample_rate_hz;
    uint32_t output_sample_rate_hz;
    uint32_t channel_count;
    uint32_t channel_type;
    crossdsd_dsd_rate dsd_rate;
    crossdsd_wave_container input_container;
    uint64_t input_frames;
    uint64_t output_dsd_samples_per_channel;
    uint64_t output_packed_bytes_per_channel;
    uint64_t output_size_bytes;
    uint64_t metadata_size_bytes;
    crossdsd_metadata_status metadata_status;
    double duration_seconds;
    double encode_seconds;
    double processing_seconds;
    double writing_seconds;
    double realtime_factor;
    double latency_output_samples;
    double channel_input_peaks[CROSSDSD_MAX_CHANNELS];
    double channel_maximum_states[CROSSDSD_MAX_CHANNELS];
    double channel_maximum_quantizer_inputs[CROSSDSD_MAX_CHANNELS];
    uint64_t channel_maximum_identical_bit_runs[CROSSDSD_MAX_CHANNELS];
    double channel_one_densities[CROSSDSD_MAX_CHANNELS];
    crossdsd_encoder_profile encoder_profile;
    uint64_t enhanced_dsd_samples_per_channel;
    uint64_t enhanced_segment_count;
    uint32_t fixed_gain_enabled;
    double requested_fixed_gain_db;
} crossdsd_encode_file_report;

typedef struct crossdsd_transcode_file_config {
    uint32_t struct_size;
    crossdsd_dsd_rate dsd_rate;
    uint32_t channel_type;
    uint64_t packed_chunk_bytes_per_channel;
    crossdsd_progress_callback progress_callback;
    void* progress_user_data;
    crossdsd_encoder_profile encoder_profile;
    /*
     * When non-zero, max_peak_dbfs must be finite and <= 0.0. CrossDSD first
     * analyses the complete reconstructed audio signal, then applies one
     * constant attenuation during a second streaming pass. It never adds gain.
     */
    uint32_t max_peak_enabled;
    double max_peak_dbfs;
    /* Zero-initialization selects the recommended adaptive search. */
    uint32_t full_peak_search;
    /*
     * Applies an already measured, uniform attenuation without a peak-search
     * pass. fixed_gain_db must be finite and <= 0.0. It is mutually exclusive
     * with max_peak_enabled.
     */
    uint32_t fixed_gain_enabled;
    double fixed_gain_db;
} crossdsd_transcode_file_config;

typedef struct crossdsd_transcode_file_report {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t input_sample_rate_hz;
    uint32_t output_sample_rate_hz;
    uint32_t channel_count;
    uint32_t channel_type;
    crossdsd_dsd_rate input_dsd_rate;
    crossdsd_dsd_rate output_dsd_rate;
    crossdsd_encoder_profile encoder_profile;
    uint64_t input_dsd_samples_per_channel;
    uint64_t reconstructed_frames;
    uint64_t output_dsd_samples_per_channel;
    uint64_t output_packed_bytes_per_channel;
    uint64_t output_size_bytes;
    uint64_t metadata_size_bytes;
    crossdsd_metadata_status metadata_status;
    uint32_t max_peak_enabled;
    double requested_max_peak_dbfs;
    double reconstructed_peak;
    double reconstructed_peak_dbfs;
    double applied_gain;
    double applied_gain_db;
    double duration_seconds;
    double analysis_seconds;
    double transcode_seconds;
    double realtime_factor;
    double channel_reconstructed_peaks[CROSSDSD_MAX_CHANNELS];
    double channel_maximum_states[CROSSDSD_MAX_CHANNELS];
    double channel_maximum_quantizer_inputs[CROSSDSD_MAX_CHANNELS];
    uint64_t channel_maximum_identical_bit_runs[CROSSDSD_MAX_CHANNELS];
    double channel_one_densities[CROSSDSD_MAX_CHANNELS];
    uint64_t enhanced_dsd_samples_per_channel;
    uint64_t enhanced_segment_count;
    uint32_t full_peak_search;
    double quick_scan_seconds;
    double analysis_input_fraction;
    uint32_t fixed_gain_enabled;
    double requested_fixed_gain_db;
} crossdsd_transcode_file_report;

typedef struct crossdsd_analyze_file_config {
    uint32_t struct_size;
    uint64_t packed_chunk_bytes_per_channel;
    /* Zero selects the recommended adaptive search; non-zero is exhaustive. */
    uint32_t full_peak_search;
    crossdsd_progress_callback progress_callback;
    void* progress_user_data;
} crossdsd_analyze_file_config;

typedef struct crossdsd_analyze_file_report {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t input_sample_rate_hz;
    uint32_t reconstructed_sample_rate_hz;
    uint32_t channel_count;
    uint32_t channel_type;
    crossdsd_dsd_rate input_dsd_rate;
    uint64_t input_dsd_samples_per_channel;
    uint64_t reconstructed_frames;
    double duration_seconds;
    double analysis_seconds;
    double realtime_factor;
    uint32_t full_peak_search;
    double quick_scan_seconds;
    double analyzed_input_fraction;
    uint32_t worker_count;
    double reconstructed_peak;
    double reconstructed_peak_dbfs;
    uint32_t peak_channel;
    uint64_t peak_frame;
    double peak_seconds;
    double channel_reconstructed_peaks[CROSSDSD_MAX_CHANNELS];
    double channel_reconstructed_peak_dbfs[CROSSDSD_MAX_CHANNELS];
    uint64_t channel_peak_frames[CROSSDSD_MAX_CHANNELS];
    double channel_peak_seconds[CROSSDSD_MAX_CHANNELS];
} crossdsd_analyze_file_report;

typedef struct crossdsd_analyze_batch_config {
    uint32_t struct_size;
    uint64_t packed_chunk_bytes_per_channel;
    uint32_t full_peak_search;
    /* Finite album/individual recommendation target, always <= 0 dBFS. */
    double target_peak_dbfs;
    crossdsd_progress_callback progress_callback;
    void* progress_user_data;
} crossdsd_analyze_batch_config;

typedef struct crossdsd_analyze_batch_report {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t input_file_count;
    uint32_t completed_file_count;
    uint32_t full_peak_search;
    double target_peak_dbfs;
    double common_gain_db;
    double maximum_reconstructed_peak;
    double maximum_reconstructed_peak_dbfs;
    uint32_t maximum_file_index;
    double total_duration_seconds;
    double analysis_seconds;
    double realtime_factor;
} crossdsd_analyze_batch_report;

typedef struct crossdsd_aiff_to_dxd_config {
    uint32_t struct_size;
    crossdsd_wave_container container;
    /* Zero selects the streaming default. */
    uint64_t input_chunk_frames;
    crossdsd_progress_callback progress_callback;
    void* progress_user_data;
    /* Optional uniform attenuation, finite and <= 0 dB. */
    uint32_t fixed_gain_enabled;
    double fixed_gain_db;
} crossdsd_aiff_to_dxd_config;

typedef struct crossdsd_aiff_to_dxd_report {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t input_sample_rate_hz;
    uint32_t output_sample_rate_hz;
    uint32_t channel_count;
    uint32_t input_bits_per_sample;
    crossdsd_wave_container container;
    uint64_t input_frames;
    uint64_t output_frames;
    uint64_t output_size_bytes;
    uint64_t metadata_size_bytes;
    crossdsd_metadata_status metadata_status;
    double duration_seconds;
    double conversion_seconds;
    double realtime_factor;
    double latency_output_samples;
    double channel_input_peaks[CROSSDSD_MAX_CHANNELS];
    double channel_output_peaks[CROSSDSD_MAX_CHANNELS];
} crossdsd_aiff_to_dxd_report;

/*
 * Decodes a DSF file to 352.8 kHz interleaved IEEE float32 WAV/RF64.
 * No normalization, clipping or dither is ever applied.
 */
CROSSDSD_API crossdsd_result crossdsd_decode_file(
    const char* input_path,
    const char* output_path,
    const crossdsd_decode_file_config* config,
    crossdsd_decode_file_report* report,
    crossdsd_error* error);

/*
 * Encodes a 352.8 kHz IEEE float32 WAV/RF64 file to DSF. DSD512 additionally
 * accepts native 705.6 kHz IEEE float32 input and uses a direct x32 path. The
 * DSD rate must be explicit; the reference implementation accepts DSD64,
 * DSD128, DSD256 and DSD512.
 * CROSSDSD_ENCODER_DSD64_ENHANCED is accepted only with DSD64.
 * Encoding progress is monotonic, starts at 0.0 and reaches 1.0 only after
 * the output file has been finalized successfully.
 */
CROSSDSD_API crossdsd_result crossdsd_encode_file(
    const char* input_path,
    const char* output_path,
    const crossdsd_encode_file_config* config,
    crossdsd_encode_file_report* report,
    crossdsd_error* error);

/*
 * Transcodes DSF to DSF through the float64 reconstructed-audio path without
 * creating a temporary PCM file. DSD64, DSD128, DSD256 and DSD512 are accepted in
 * every input/output combination. With max_peak_enabled, processing uses two
 * constant-memory passes so the requested attenuation is uniform over the
 * complete programme.
 */
CROSSDSD_API crossdsd_result crossdsd_transcode_file(
    const char* input_path,
    const char* output_path,
    const crossdsd_transcode_file_config* config,
    crossdsd_transcode_file_report* report,
    crossdsd_error* error);

/*
 * Measures the reconstructed audio peak of a DSF file without creating an
 * output audio file. The adaptive search is the default; full_peak_search
 * selects exhaustive reconstruction. Peak positions use the 352.8 kHz
 * reconstructed-audio timeline.
 */
CROSSDSD_API crossdsd_result crossdsd_analyze_file(
    const char* input_path,
    const crossdsd_analyze_file_config* config,
    crossdsd_analyze_file_report* report,
    crossdsd_error* error);

/*
 * Analyzes an ordered set of DSF files sequentially. The caller supplies one
 * file report and one individual gain slot per input path. common_gain_db is
 * the uniform attenuation that keeps every file at or below target_peak_dbfs;
 * it preserves album-level relationships and never adds gain.
 */
CROSSDSD_API crossdsd_result crossdsd_analyze_batch(
    const char* const* input_paths,
    uint32_t input_count,
    const crossdsd_analyze_batch_config* config,
    crossdsd_analyze_file_report* file_reports,
    double* individual_gain_db,
    crossdsd_analyze_batch_report* report,
    crossdsd_error* error);

/*
 * Converts uncompressed 44.1 kHz integer PCM AIFF/AIFC (`NONE` or `sowt`)
 * to 352.8 kHz
 * interleaved IEEE float32 WAV/RF64 with a reference-quality x8 streaming
 * FIR. The source level and duration are preserved; no normalization,
 * clipping or dither is applied. An embedded ID3 tag is copied unchanged.
 */
CROSSDSD_API crossdsd_result crossdsd_convert_aiff_to_dxd_file(
    const char* input_path,
    const char* output_path,
    const crossdsd_aiff_to_dxd_config* config,
    crossdsd_aiff_to_dxd_report* report,
    crossdsd_error* error);

#ifdef __cplusplus
}
#endif

#endif
