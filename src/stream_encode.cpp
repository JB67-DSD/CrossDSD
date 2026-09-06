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

#include "crossdsd/stream.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include <vector>

namespace {

void set_error(crossdsd_error* error, crossdsd_result code, const char* message) {
    if (!error || error->struct_size < sizeof(crossdsd_error)) return;
    error->code = code;
    error->byte_offset = 0;
    std::snprintf(error->message, sizeof(error->message), "%s", message);
}

bool valid_output_rate(uint32_t rate) {
    return rate == 2'822'400u || rate == 5'644'800u ||
           rate == 11'289'600u || rate == 22'579'200u;
}

crossdsd_modulator_profile profile_for_rate(uint32_t rate) {
    if (rate == 22'579'200u) {
        return CROSSDSD_MODULATOR_DSD512_CRFB_ORDER7_HINF_1_20;
    }
    if (rate == 11'289'600u) {
        return CROSSDSD_MODULATOR_DSD256_CRFB_ORDER7_HINF_1_20;
    }
    if (rate == 5'644'800u) {
        return CROSSDSD_MODULATOR_DSD128_CRFB_ORDER5_HINF_1_30;
    }
    return CROSSDSD_MODULATOR_CRFB_ORDER7_HINF_1_32;
}

}  // namespace

struct crossdsd_dxd_to_dsd_stream {
    crossdsd_encoder* encoder = nullptr;
    crossdsd_dxd_to_dsd_stream_info info{};
    std::vector<float> planar_f32;
    std::vector<double> planar_f64;

    ~crossdsd_dxd_to_dsd_stream() {
        crossdsd_encoder_destroy(encoder);
    }
};

namespace {

void refresh_info(crossdsd_dxd_to_dsd_stream& stream) {
    crossdsd_encoder_info encoder_info{};
    encoder_info.struct_size = sizeof(encoder_info);
    if (crossdsd_encoder_get_info(stream.encoder, &encoder_info) !=
        CROSSDSD_OK) {
        return;
    }
    auto& info = stream.info;
    info.input_dxd_frames =
        encoder_info.input_samples_consumed_per_channel;
    info.output_dsd_samples_per_channel =
        encoder_info.dsd_samples_produced_per_channel;
    info.output_packed_bytes_per_channel =
        encoder_info.packed_bytes_produced_per_channel;
    info.finished = encoder_info.finished;
    info.stability_failure = encoder_info.stability_failure;
    info.enhanced_dsd_samples_per_channel =
        encoder_info.enhanced_dsd_samples_per_channel;
    info.enhanced_segment_count = encoder_info.enhanced_segment_count;
}

template <typename Sample>
crossdsd_result process(
    crossdsd_dxd_to_dsd_stream* stream, const Sample* input_interleaved,
    uint64_t input_frames, uint8_t* output_packed_planar,
    uint64_t output_capacity_bytes_per_channel, uint64_t output_stride_bytes,
    uint64_t* output_bytes_per_channel, crossdsd_error* error) {
    if (!stream || !input_interleaved || input_frames == 0 ||
        !output_packed_planar || !output_bytes_per_channel ||
        stream->info.finished ||
        input_frames > std::numeric_limits<size_t>::max() /
                           stream->info.channel_count) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                  "invalid DXD-to-DSD stream process arguments");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    *output_bytes_per_channel = 0;
    const uint64_t bytes_per_input =
        stream->info.interpolation_factor / 8u;
    if (input_frames >
            (std::numeric_limits<uint64_t>::max() - 1u) /
                bytes_per_input) {
        set_error(error, CROSSDSD_ERROR_OVERFLOW,
                  "DXD input block is too large");
        return CROSSDSD_ERROR_OVERFLOW;
    }
    const uint64_t required = input_frames * bytes_per_input + 1u;
    if (
        output_capacity_bytes_per_channel < required ||
        output_stride_bytes < required) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                  "packed DSD output buffer is too small");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    try {
        if constexpr (std::is_same_v<Sample, float>) {
            stream->planar_f32.resize(
                static_cast<size_t>(input_frames * stream->info.channel_count));
        } else {
            stream->planar_f64.resize(
                static_cast<size_t>(input_frames * stream->info.channel_count));
        }
    } catch (const std::bad_alloc&) {
        set_error(error, CROSSDSD_ERROR_OUT_OF_MEMORY, "out of memory");
        return CROSSDSD_ERROR_OUT_OF_MEMORY;
    }
    for (uint64_t frame = 0; frame < input_frames; ++frame) {
        for (uint32_t channel = 0; channel < stream->info.channel_count;
             ++channel) {
            const Sample value =
                input_interleaved[
                    frame * stream->info.channel_count + channel];
            if (!std::isfinite(value)) {
                set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                          "DXD input must contain finite samples");
                return CROSSDSD_ERROR_INVALID_ARGUMENT;
            }
            stream->info.channel_input_peaks[channel] = std::max(
                stream->info.channel_input_peaks[channel],
                std::abs(static_cast<double>(value)));
            if constexpr (std::is_same_v<Sample, float>) {
                stream->planar_f32[
                    static_cast<uint64_t>(channel) * input_frames + frame] =
                    value;
            } else {
                stream->planar_f64[
                    static_cast<uint64_t>(channel) * input_frames + frame] =
                    value;
            }
        }
    }
    crossdsd_result result;
    if constexpr (std::is_same_v<Sample, float>) {
        result = crossdsd_encoder_process(
            stream->encoder, stream->planar_f32.data(), input_frames,
            input_frames, output_packed_planar,
            output_capacity_bytes_per_channel, output_stride_bytes,
            output_bytes_per_channel, error);
    } else {
        result = crossdsd_encoder_process_f64(
            stream->encoder, stream->planar_f64.data(), input_frames,
            input_frames, output_packed_planar,
            output_capacity_bytes_per_channel, output_stride_bytes,
            output_bytes_per_channel, error);
    }
    if (result == CROSSDSD_OK) refresh_info(*stream);
    return result;
}

}  // namespace

extern "C" {

crossdsd_result crossdsd_dxd_to_dsd_stream_create(
    const crossdsd_dxd_to_dsd_stream_config* config,
    crossdsd_dxd_to_dsd_stream** out_stream, crossdsd_error* error) {
    if (!config ||
        config->struct_size < sizeof(crossdsd_dxd_to_dsd_stream_config) ||
        !out_stream || !valid_output_rate(config->output_sample_rate_hz) ||
        config->channel_count == 0 ||
        config->channel_count > CROSSDSD_STREAM_MAX_CHANNELS ||
        (config->encoder_profile != CROSSDSD_ENCODER_STANDARD &&
         config->encoder_profile != CROSSDSD_ENCODER_DSD64_ENHANCED) ||
        (config->encoder_profile == CROSSDSD_ENCODER_DSD64_ENHANCED &&
         config->output_sample_rate_hz != 2'822'400u)) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                  "invalid DXD-to-DSD stream configuration");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    *out_stream = nullptr;
    try {
        auto stream = std::make_unique<crossdsd_dxd_to_dsd_stream>();
        crossdsd_encoder_config encoder_config{};
        encoder_config.struct_size = sizeof(encoder_config);
        encoder_config.input_sample_rate_hz = 352'800u;
        encoder_config.output_sample_rate_hz = config->output_sample_rate_hz;
        encoder_config.channel_count = config->channel_count;
        encoder_config.modulator_profile =
            profile_for_rate(config->output_sample_rate_hz);
        encoder_config.encoder_profile = config->encoder_profile;
        auto result = crossdsd_encoder_create(
            &encoder_config, &stream->encoder, error);
        if (result != CROSSDSD_OK) return result;
        crossdsd_encoder_info encoder_info{};
        encoder_info.struct_size = sizeof(encoder_info);
        result = crossdsd_encoder_get_info(stream->encoder, &encoder_info);
        if (result != CROSSDSD_OK) return result;
        auto& info = stream->info;
        info.struct_size = sizeof(info);
        info.api_version = CROSSDSD_STREAM_API_VERSION;
        info.input_sample_rate_hz = encoder_info.input_sample_rate_hz;
        info.output_sample_rate_hz = encoder_info.output_sample_rate_hz;
        info.channel_count = encoder_info.channel_count;
        info.interpolation_factor = encoder_info.interpolation_factor;
        info.encoder_profile = encoder_info.encoder_profile;
        info.latency_output_samples = encoder_info.latency_output_samples;
        info.maximum_pcm_amplitude = encoder_info.maximum_pcm_amplitude;
        *out_stream = stream.release();
    } catch (const std::bad_alloc&) {
        set_error(error, CROSSDSD_ERROR_OUT_OF_MEMORY, "out of memory");
        return CROSSDSD_ERROR_OUT_OF_MEMORY;
    }
    set_error(error, CROSSDSD_OK, "");
    return CROSSDSD_OK;
}

void crossdsd_dxd_to_dsd_stream_destroy(
    crossdsd_dxd_to_dsd_stream* stream) {
    delete stream;
}

crossdsd_result crossdsd_dxd_to_dsd_stream_get_info(
    const crossdsd_dxd_to_dsd_stream* stream,
    crossdsd_dxd_to_dsd_stream_info* info) {
    if (!stream || !info ||
        info->struct_size < sizeof(crossdsd_dxd_to_dsd_stream_info)) {
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    *info = stream->info;
    return CROSSDSD_OK;
}

crossdsd_result crossdsd_dxd_to_dsd_stream_get_channel_metrics(
    const crossdsd_dxd_to_dsd_stream* stream, uint32_t channel_index,
    crossdsd_modulator_channel_metrics* metrics) {
    if (!stream) return CROSSDSD_ERROR_INVALID_ARGUMENT;
    return crossdsd_encoder_get_channel_metrics(
        stream->encoder, channel_index, metrics);
}

crossdsd_result crossdsd_dxd_to_dsd_stream_process_f32(
    crossdsd_dxd_to_dsd_stream* stream, const float* input_interleaved,
    uint64_t input_frames, uint8_t* output_packed_planar,
    uint64_t output_capacity_bytes_per_channel, uint64_t output_stride_bytes,
    uint64_t* output_bytes_per_channel, crossdsd_error* error) {
    return process(
        stream, input_interleaved, input_frames, output_packed_planar,
        output_capacity_bytes_per_channel, output_stride_bytes,
        output_bytes_per_channel, error);
}

crossdsd_result crossdsd_dxd_to_dsd_stream_process_f64(
    crossdsd_dxd_to_dsd_stream* stream, const double* input_interleaved,
    uint64_t input_frames, uint8_t* output_packed_planar,
    uint64_t output_capacity_bytes_per_channel, uint64_t output_stride_bytes,
    uint64_t* output_bytes_per_channel, crossdsd_error* error) {
    return process(
        stream, input_interleaved, input_frames, output_packed_planar,
        output_capacity_bytes_per_channel, output_stride_bytes,
        output_bytes_per_channel, error);
}

crossdsd_result crossdsd_dxd_to_dsd_stream_finish(
    crossdsd_dxd_to_dsd_stream* stream, uint8_t* output_packed_planar,
    uint64_t output_capacity_bytes_per_channel, uint64_t output_stride_bytes,
    uint64_t* output_bytes_per_channel, crossdsd_error* error) {
    if (!stream || !output_bytes_per_channel || stream->info.finished) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                  "invalid DXD-to-DSD stream finish arguments");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    const auto result = crossdsd_encoder_finish(
        stream->encoder, output_packed_planar,
        output_capacity_bytes_per_channel, output_stride_bytes,
        output_bytes_per_channel, error);
    if (result == CROSSDSD_OK) refresh_info(*stream);
    return result;
}

}  // extern "C"
