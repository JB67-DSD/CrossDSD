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

#include "crossdsd/convert.h"
#include "metadata_trace.h"
#include "platform_file.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace {

void set_error(crossdsd_error* error, crossdsd_result code, const char* message) {
    if (!error || error->struct_size < sizeof(crossdsd_error)) return;
    error->code = code;
    error->byte_offset = 0;
    std::snprintf(error->message, sizeof(error->message), "%s", message);
}

struct ReaderOwner {
    crossdsd_wave_reader* value = nullptr;
    ~ReaderOwner() { crossdsd_wave_reader_close(value); }
};

struct EncoderOwner {
    crossdsd_encoder* value = nullptr;
    ~EncoderOwner() { crossdsd_encoder_destroy(value); }
};

struct WriterOwner {
    crossdsd_dsf_writer* value = nullptr;
    bool closed = false;
    bool committed = false;
    const char* path = nullptr;
    ~WriterOwner() {
        if (value && !closed) crossdsd_dsf_writer_close(value, nullptr);
        if (!committed && path) crossdsd::platform::remove_file(path);
    }
};

uint32_t infer_dsf_channel_type(uint32_t channel_count) {
    switch (channel_count) {
    case 1: return 1;  // Center
    case 2: return 2;  // Front left, front right
    case 3: return 3;  // Front left, front right, center
    case 5: return 6;  // Front left, front right, center, back left, back right
    case 6: return 7;  // 5.1: front left/right, center, LFE, back left/right
    default: return 0;
    }
}

}  // namespace

extern "C" {

crossdsd_result crossdsd_encode_file(
    const char* input_path, const char* output_path,
    const crossdsd_encode_file_config* config,
    crossdsd_encode_file_report* report, crossdsd_error* error) {
    if (!input_path || !output_path || !config || !report ||
        config->struct_size < sizeof(crossdsd_encode_file_config) ||
        report->struct_size < sizeof(crossdsd_encode_file_report) ||
        (config->dsd_rate != CROSSDSD_DSD64 &&
         config->dsd_rate != CROSSDSD_DSD128 &&
         config->dsd_rate != CROSSDSD_DSD256 &&
         config->dsd_rate != CROSSDSD_DSD512) ||
        (config->encoder_profile != CROSSDSD_ENCODER_STANDARD &&
         config->encoder_profile != CROSSDSD_ENCODER_DSD64_ENHANCED) ||
        (config->fixed_gain_enabled &&
         (!std::isfinite(config->fixed_gain_db) || config->fixed_gain_db > 0.0))) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                  "encode requires an explicit DSD64, DSD128, DSD256 or DSD512 rate");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    if (config->encoder_profile == CROSSDSD_ENCODER_DSD64_ENHANCED &&
        config->dsd_rate != CROSSDSD_DSD64) {
        set_error(error, CROSSDSD_ERROR_UNSUPPORTED_RATE,
                  "Enhanced quality is available for DSD64 only");
        return CROSSDSD_ERROR_UNSUPPORTED_RATE;
    }
    const float fixed_gain = config->fixed_gain_enabled
        ? static_cast<float>(std::pow(10.0, config->fixed_gain_db / 20.0))
        : 1.0f;
    const uint32_t output_rate =
        config->dsd_rate == CROSSDSD_DSD512 ? 22'579'200u
        : config->dsd_rate == CROSSDSD_DSD256 ? 11'289'600u
        : config->dsd_rate == CROSSDSD_DSD128 ? 5'644'800u
                                               : 2'822'400u;
    const uint64_t chunk =
        config->frame_chunk_count == 0 ? 65'536u
                                       : config->frame_chunk_count;

    ReaderOwner reader;
    auto result =
        crossdsd_wave_reader_open_file(input_path, &reader.value, error);
    if (result != CROSSDSD_OK) return result;
    crossdsd_wave_reader_info input{};
    input.struct_size = sizeof(input);
    result = crossdsd_wave_reader_get_info(reader.value, &input);
    if (result != CROSSDSD_OK) return result;
    const bool native_dsd512_pcm =
        config->dsd_rate == CROSSDSD_DSD512 &&
        input.sample_rate_hz == 705'600u;
    if (input.sample_rate_hz != CROSSDSD_DXD_SAMPLE_RATE &&
        !native_dsd512_pcm) {
        set_error(error, CROSSDSD_ERROR_UNSUPPORTED_RATE,
                  "encode input must be 352.8 kHz DXD, or 705.6 kHz PCM for DSD512");
        return CROSSDSD_ERROR_UNSUPPORTED_RATE;
    }
    if (input.frame_count == 0) {
        set_error(error, CROSSDSD_ERROR_INVALID_SAMPLE_COUNT,
                  "encode input contains no audio frames");
        return CROSSDSD_ERROR_INVALID_SAMPLE_COUNT;
    }
    const uint64_t interpolation_factor = output_rate / input.sample_rate_hz;
    const uint64_t bytes_per_input = interpolation_factor / 8u;
    if (interpolation_factor == 0u || interpolation_factor % 8u != 0u ||
        chunk > (std::numeric_limits<uint64_t>::max() - 1u) /
                    bytes_per_input) {
        set_error(error, CROSSDSD_ERROR_OVERFLOW,
                  "encode block is too large or has an invalid rate ratio");
        return CROSSDSD_ERROR_OVERFLOW;
    }
    if (config->progress_callback &&
        !config->progress_callback(config->progress_user_data, 0.0)) {
        set_error(error, CROSSDSD_ERROR_CANCELLED,
                  "conversion cancelled");
        return CROSSDSD_ERROR_CANCELLED;
    }

    crossdsd_encoder_config encoder_config{};
    encoder_config.struct_size = sizeof(encoder_config);
    encoder_config.input_sample_rate_hz = input.sample_rate_hz;
    encoder_config.output_sample_rate_hz = output_rate;
    encoder_config.channel_count = input.channel_count;
    encoder_config.modulator_profile =
        config->dsd_rate == CROSSDSD_DSD512
            ? CROSSDSD_MODULATOR_DSD512_CRFB_ORDER7_HINF_1_20
        : config->dsd_rate == CROSSDSD_DSD256
            ? CROSSDSD_MODULATOR_DSD256_CRFB_ORDER7_HINF_1_20
        : config->dsd_rate == CROSSDSD_DSD128
            ? CROSSDSD_MODULATOR_DSD128_CRFB_ORDER5_HINF_1_30
            : CROSSDSD_MODULATOR_CRFB_ORDER7_HINF_1_32;
    encoder_config.encoder_profile = config->encoder_profile;
    EncoderOwner encoder;
    result = crossdsd_encoder_create(
        &encoder_config, &encoder.value, error);
    if (result != CROSSDSD_OK) return result;
    crossdsd_encoder_info encoder_info{};
    encoder_info.struct_size = sizeof(encoder_info);
    crossdsd_encoder_get_info(encoder.value, &encoder_info);

    crossdsd_dsf_writer_config writer_config{};
    writer_config.struct_size = sizeof(writer_config);
    writer_config.sample_rate_hz = output_rate;
    writer_config.channel_count = input.channel_count;
    writer_config.channel_type =
        config->channel_type == 0
            ? (input.channel_type == 0
                   ? infer_dsf_channel_type(input.channel_count)
                   : input.channel_type)
            : config->channel_type;
    WriterOwner writer;
    result = crossdsd_dsf_writer_open_file(
        output_path, &writer_config, &writer.value, error);
    if (result != CROSSDSD_OK) return result;
    writer.path = output_path;

    try {
        if (chunk >
                std::numeric_limits<size_t>::max() / input.channel_count ||
            chunk * bytes_per_input + 1u >
                std::numeric_limits<size_t>::max() / input.channel_count) {
            set_error(error, CROSSDSD_ERROR_OVERFLOW,
                      "encode buffers are too large");
            return CROSSDSD_ERROR_OVERFLOW;
        }
        std::vector<float> interleaved(
            static_cast<size_t>(chunk * input.channel_count));
        std::vector<float> planar(
            static_cast<size_t>(chunk * input.channel_count));
        std::vector<uint8_t> packed(
            static_cast<size_t>(
                (chunk * bytes_per_input + 1u) * input.channel_count));
        const uint64_t packed_stride = chunk * bytes_per_input + 1u;
        double peaks[CROSSDSD_MAX_CHANNELS]{};
        double processing_seconds = 0.0;
        double writing_seconds = 0.0;
        const auto start = std::chrono::steady_clock::now();

        while (true) {
            uint64_t frames = 0;
            result = crossdsd_wave_reader_read(
                reader.value, interleaved.data(), chunk, &frames, error);
            if (result == CROSSDSD_ERROR_END_OF_STREAM) break;
            if (result != CROSSDSD_OK) return result;
            for (uint64_t frame = 0; frame < frames; ++frame) {
                for (uint32_t channel = 0; channel < input.channel_count;
                     ++channel) {
                    const float value = fixed_gain *
                        interleaved[frame * input.channel_count + channel];
                    planar[static_cast<uint64_t>(channel) * frames + frame] =
                        value;
                    peaks[channel] =
                        std::max(peaks[channel], std::abs(static_cast<double>(value)));
                }
            }
            uint64_t bytes = 0;
            const auto processing_start =
                std::chrono::steady_clock::now();
            result = crossdsd_encoder_process(
                encoder.value, planar.data(), frames, frames, packed.data(),
                packed_stride, packed_stride, &bytes, error);
            processing_seconds += std::chrono::duration<double>(
                std::chrono::steady_clock::now() - processing_start).count();
            if (result != CROSSDSD_OK) return result;
            if (bytes != 0) {
                for (uint32_t channel = 1; channel < input.channel_count;
                     ++channel) {
                    std::memmove(
                        packed.data() + static_cast<uint64_t>(channel) * bytes,
                        packed.data() +
                            static_cast<uint64_t>(channel) * packed_stride,
                        static_cast<size_t>(bytes));
                }
                const auto writing_start =
                    std::chrono::steady_clock::now();
                result = crossdsd_dsf_writer_write_packed(
                    writer.value, packed.data(), bytes, 8, error);
                writing_seconds += std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - writing_start).count();
                if (result != CROSSDSD_OK) return result;
            }
            if (config->progress_callback) {
                crossdsd_encoder_get_info(encoder.value, &encoder_info);
                const double fraction = std::min(
                    0.99,
                    static_cast<double>(
                        encoder_info.input_samples_consumed_per_channel) /
                        static_cast<double>(input.frame_count));
                if (!config->progress_callback(
                        config->progress_user_data, fraction)) {
                    set_error(error, CROSSDSD_ERROR_CANCELLED,
                              "conversion cancelled");
                    return CROSSDSD_ERROR_CANCELLED;
                }
            }
        }

        crossdsd_encoder_get_info(encoder.value, &encoder_info);
        const uint64_t remaining =
            encoder_info.input_samples_consumed_per_channel *
                bytes_per_input -
            encoder_info.packed_bytes_produced_per_channel;
        if (remaining >
            std::numeric_limits<size_t>::max() / input.channel_count) {
            set_error(error, CROSSDSD_ERROR_OVERFLOW,
                      "encode final buffer is too large");
            return CROSSDSD_ERROR_OVERFLOW;
        }
        std::vector<uint8_t> final_packed(
            static_cast<size_t>(remaining * input.channel_count));
        uint64_t final_bytes = 0;
        const auto processing_start = std::chrono::steady_clock::now();
        result = crossdsd_encoder_finish(
            encoder.value, final_packed.data(), remaining, remaining,
            &final_bytes, error);
        processing_seconds += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - processing_start).count();
        if (result != CROSSDSD_OK) return result;
        if (final_bytes != 0) {
            const auto writing_start = std::chrono::steady_clock::now();
            result = crossdsd_dsf_writer_write_packed(
                writer.value, final_packed.data(), final_bytes, 8, error);
            writing_seconds += std::chrono::duration<double>(
                std::chrono::steady_clock::now() - writing_start).count();
            if (result != CROSSDSD_OK) return result;
        }

        uint64_t metadata_written = 0;
        crossdsd_metadata_status metadata_status = CROSSDSD_METADATA_NONE;
        std::vector<uint8_t> metadata;
        if (input.metadata_status == CROSSDSD_METADATA_ID3 &&
            input.metadata_size_bytes != 0) {
            if (input.metadata_size_bytes >
                std::numeric_limits<size_t>::max()) {
                set_error(error, CROSSDSD_ERROR_OVERFLOW,
                          "metadata is too large");
                return CROSSDSD_ERROR_OVERFLOW;
            }
            metadata.resize(static_cast<size_t>(input.metadata_size_bytes));
            uint64_t metadata_read = 0;
            result = crossdsd_wave_reader_read_metadata(
                reader.value, metadata.data(), metadata.size(),
                &metadata_read, error);
            if (result != CROSSDSD_OK) return result;
            metadata.resize(static_cast<size_t>(metadata_read));
        }
        const char* rate_name = config->dsd_rate == CROSSDSD_DSD64
            ? "DSD64" : config->dsd_rate == CROSSDSD_DSD128
                ? "DSD128" : config->dsd_rate == CROSSDSD_DSD256
                    ? "DSD256" : "DSD512";
        char settings[128]{};
        std::snprintf(settings, sizeof(settings), "CrossDSD %s - %s%s",
                      CROSSDSD_VERSION, rate_name,
                      config->encoder_profile ==
                              CROSSDSD_ENCODER_DSD64_ENHANCED
                          ? " Enhanced" : "");
        metadata = crossdsd_id3_with_encoder_settings(
            metadata, settings, rate_name);
        result = crossdsd_dsf_writer_write_id3(
            writer.value, metadata.data(), metadata.size(), error);
        if (result != CROSSDSD_OK) return result;
        metadata_written = metadata.size();
        metadata_status = CROSSDSD_METADATA_ID3;

        crossdsd_dsf_writer_info writer_info{};
        writer_info.struct_size = sizeof(writer_info);
        crossdsd_dsf_writer_get_info(writer.value, &writer_info);
        result = crossdsd_dsf_writer_close(writer.value, error);
        writer.closed = true;
        writer.value = nullptr;
        if (result != CROSSDSD_OK) return result;
        writer.committed = true;

        const double seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
        crossdsd_encoder_get_info(encoder.value, &encoder_info);
        crossdsd_encode_file_report value{};
        value.struct_size = sizeof(value);
        value.api_version = CROSSDSD_CONVERT_API_VERSION;
        value.input_sample_rate_hz = input.sample_rate_hz;
        value.output_sample_rate_hz = output_rate;
        value.channel_count = input.channel_count;
        value.channel_type = writer_info.channel_type;
        value.dsd_rate = config->dsd_rate;
        value.encoder_profile = config->encoder_profile;
        value.input_container = input.container;
        value.input_frames = input.frame_count;
        value.output_dsd_samples_per_channel =
            encoder_info.dsd_samples_produced_per_channel;
        value.output_packed_bytes_per_channel =
            encoder_info.packed_bytes_produced_per_channel;
        const uint64_t stored_blocks =
            (encoder_info.packed_bytes_produced_per_channel +
             writer_info.block_size_per_channel - 1u) /
            writer_info.block_size_per_channel;
        value.output_size_bytes =
            92u +
            stored_blocks * writer_info.block_size_per_channel *
                input.channel_count +
            metadata_written;
        value.metadata_size_bytes = metadata_written;
        value.metadata_status = metadata_status;
        value.duration_seconds =
            static_cast<double>(input.frame_count) / input.sample_rate_hz;
        value.encode_seconds = seconds;
        value.processing_seconds = processing_seconds;
        value.writing_seconds = writing_seconds;
        value.realtime_factor =
            seconds > 0.0 ? value.duration_seconds / seconds : 0.0;
        value.latency_output_samples =
            encoder_info.latency_output_samples;
        value.enhanced_dsd_samples_per_channel =
            encoder_info.enhanced_dsd_samples_per_channel;
        value.enhanced_segment_count =
            encoder_info.enhanced_segment_count;
        value.fixed_gain_enabled = config->fixed_gain_enabled;
        value.requested_fixed_gain_db = config->fixed_gain_enabled
            ? config->fixed_gain_db : 0.0;
        for (uint32_t channel = 0; channel < input.channel_count; ++channel) {
            value.channel_input_peaks[channel] = peaks[channel];
            crossdsd_modulator_channel_metrics metrics{};
            metrics.struct_size = sizeof(metrics);
            crossdsd_encoder_get_channel_metrics(
                encoder.value, channel, &metrics);
            value.channel_maximum_states[channel] =
                metrics.maximum_absolute_state;
            value.channel_maximum_quantizer_inputs[channel] =
                metrics.maximum_absolute_quantizer_input;
            value.channel_maximum_identical_bit_runs[channel] =
                metrics.maximum_identical_bit_run;
            value.channel_one_densities[channel] = metrics.one_density;
        }
        *report = value;
        if (config->progress_callback) {
            config->progress_callback(config->progress_user_data, 1.0);
        }
    } catch (const std::bad_alloc&) {
        set_error(error, CROSSDSD_ERROR_OUT_OF_MEMORY, "out of memory");
        return CROSSDSD_ERROR_OUT_OF_MEMORY;
    }
    set_error(error, CROSSDSD_OK, "");
    return CROSSDSD_OK;
}

}  // extern "C"
