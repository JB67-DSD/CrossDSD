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
#include <vector>

namespace {

void set_error(crossdsd_error* error, crossdsd_result code, const char* message) {
    if (!error || error->struct_size < sizeof(crossdsd_error)) return;
    error->code = code;
    error->byte_offset = 0;
    std::snprintf(error->message, sizeof(error->message), "%s", message);
}

bool valid_rate(uint32_t rate) {
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

struct crossdsd_dsd_to_dsd_stream {
    crossdsd_decoder* decoder = nullptr;
    crossdsd_encoder* encoder = nullptr;
    crossdsd_dsd_to_dsd_stream_info info{};
    std::vector<int8_t> expanded;
    std::vector<double> reconstructed;

    ~crossdsd_dsd_to_dsd_stream() {
        crossdsd_decoder_destroy(decoder);
        crossdsd_encoder_destroy(encoder);
    }
};

namespace {

void refresh_encoder_info(crossdsd_dsd_to_dsd_stream& stream) {
    crossdsd_encoder_info encoder_info{};
    encoder_info.struct_size = sizeof(encoder_info);
    if (crossdsd_encoder_get_info(stream.encoder, &encoder_info) !=
        CROSSDSD_OK) {
        return;
    }
    stream.info.output_dsd_samples_per_channel =
        encoder_info.dsd_samples_produced_per_channel;
    stream.info.output_packed_bytes_per_channel =
        encoder_info.packed_bytes_produced_per_channel;
    stream.info.finished = encoder_info.finished;
    stream.info.stability_failure = encoder_info.stability_failure;
    stream.info.enhanced_dsd_samples_per_channel =
        encoder_info.enhanced_dsd_samples_per_channel;
    stream.info.enhanced_segment_count =
        encoder_info.enhanced_segment_count;
}

}  // namespace

extern "C" {

crossdsd_result crossdsd_dsd_to_dsd_stream_create(
    const crossdsd_dsd_to_dsd_stream_config* config,
    crossdsd_dsd_to_dsd_stream** out_stream, crossdsd_error* error) {
    if (!config ||
        config->struct_size < sizeof(crossdsd_dsd_to_dsd_stream_config) ||
        !out_stream || !valid_rate(config->input_sample_rate_hz) ||
        !valid_rate(config->output_sample_rate_hz) ||
        config->channel_count == 0 ||
        config->channel_count > CROSSDSD_STREAM_MAX_CHANNELS ||
        (config->filter_profile != CROSSDSD_FILTER_STANDARD &&
         config->filter_profile != CROSSDSD_FILTER_RAW) ||
        (config->encoder_profile != CROSSDSD_ENCODER_STANDARD &&
         config->encoder_profile != CROSSDSD_ENCODER_DSD64_ENHANCED) ||
        (config->encoder_profile == CROSSDSD_ENCODER_DSD64_ENHANCED &&
         config->output_sample_rate_hz != 2'822'400u)) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                  "invalid DSD-to-DSD stream configuration");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    *out_stream = nullptr;
    try {
        auto stream = std::make_unique<crossdsd_dsd_to_dsd_stream>();
        crossdsd_decoder_config decoder_config{};
        decoder_config.struct_size = sizeof(decoder_config);
        decoder_config.input_sample_rate_hz = config->input_sample_rate_hz;
        decoder_config.channel_count = config->channel_count;
        decoder_config.level_mode = CROSSDSD_LEVEL_PCM_CONTINUITY;
        decoder_config.filter_profile = config->filter_profile;
        auto result = crossdsd_decoder_create(
            &decoder_config, &stream->decoder, error);
        if (result != CROSSDSD_OK) return result;

        crossdsd_encoder_config encoder_config{};
        encoder_config.struct_size = sizeof(encoder_config);
        encoder_config.input_sample_rate_hz = 352'800u;
        encoder_config.output_sample_rate_hz = config->output_sample_rate_hz;
        encoder_config.channel_count = config->channel_count;
        encoder_config.modulator_profile =
            profile_for_rate(config->output_sample_rate_hz);
        encoder_config.encoder_profile = config->encoder_profile;
        result = crossdsd_encoder_create(
            &encoder_config, &stream->encoder, error);
        if (result != CROSSDSD_OK) return result;

        crossdsd_decoder_info decoder_info{};
        decoder_info.struct_size = sizeof(decoder_info);
        result = crossdsd_decoder_get_info(
            stream->decoder, &decoder_info);
        if (result != CROSSDSD_OK) return result;
        crossdsd_encoder_info encoder_info{};
        encoder_info.struct_size = sizeof(encoder_info);
        result = crossdsd_encoder_get_info(
            stream->encoder, &encoder_info);
        if (result != CROSSDSD_OK) return result;

        auto& info = stream->info;
        info.struct_size = sizeof(info);
        info.api_version = CROSSDSD_STREAM_API_VERSION;
        info.input_sample_rate_hz = config->input_sample_rate_hz;
        info.output_sample_rate_hz = config->output_sample_rate_hz;
        info.dxd_sample_rate_hz = 352'800u;
        info.channel_count = config->channel_count;
        info.decimation_factor = decoder_info.decimation_factor;
        info.interpolation_factor = encoder_info.interpolation_factor;
        info.filter_profile = config->filter_profile;
        info.encoder_profile = config->encoder_profile;
        info.decoder_latency_dxd_samples =
            decoder_info.latency_output_samples;
        info.encoder_latency_dsd_samples =
            encoder_info.latency_output_samples;
        *out_stream = stream.release();
    } catch (const std::bad_alloc&) {
        set_error(error, CROSSDSD_ERROR_OUT_OF_MEMORY, "out of memory");
        return CROSSDSD_ERROR_OUT_OF_MEMORY;
    }
    set_error(error, CROSSDSD_OK, "");
    return CROSSDSD_OK;
}

void crossdsd_dsd_to_dsd_stream_destroy(
    crossdsd_dsd_to_dsd_stream* stream) {
    delete stream;
}

crossdsd_result crossdsd_dsd_to_dsd_stream_get_info(
    const crossdsd_dsd_to_dsd_stream* stream,
    crossdsd_dsd_to_dsd_stream_info* info) {
    if (!stream || !info ||
        info->struct_size < sizeof(crossdsd_dsd_to_dsd_stream_info)) {
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    *info = stream->info;
    return CROSSDSD_OK;
}

crossdsd_result crossdsd_dsd_to_dsd_stream_get_channel_metrics(
    const crossdsd_dsd_to_dsd_stream* stream, uint32_t channel_index,
    crossdsd_modulator_channel_metrics* metrics) {
    if (!stream) return CROSSDSD_ERROR_INVALID_ARGUMENT;
    return crossdsd_encoder_get_channel_metrics(
        stream->encoder, channel_index, metrics);
}

crossdsd_result crossdsd_dsd_to_dsd_stream_process(
    crossdsd_dsd_to_dsd_stream* stream,
    const uint8_t* input_packed_planar, uint64_t input_bytes_per_channel,
    uint64_t input_stride_bytes, uint8_t valid_bits_in_last_byte,
    uint8_t* output_packed_planar,
    uint64_t output_capacity_bytes_per_channel, uint64_t output_stride_bytes,
    uint64_t* output_bytes_per_channel, crossdsd_error* error) {
    if (!stream || !input_packed_planar || input_bytes_per_channel == 0 ||
        input_stride_bytes < input_bytes_per_channel ||
        valid_bits_in_last_byte == 0 || valid_bits_in_last_byte > 8 ||
        !output_packed_planar || !output_bytes_per_channel ||
        stream->info.input_sealed || stream->info.finished) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                  "invalid DSD-to-DSD stream process arguments");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    *output_bytes_per_channel = 0;
    if (input_bytes_per_channel >
        (std::numeric_limits<uint64_t>::max() - 7u) / 8u) {
        set_error(error, CROSSDSD_ERROR_OVERFLOW,
                  "DSD input block is too large");
        return CROSSDSD_ERROR_OVERFLOW;
    }
    const uint64_t input_samples =
        (input_bytes_per_channel - 1u) * 8u + valid_bits_in_last_byte;
    const uint64_t dxd_capacity =
        input_samples / stream->info.decimation_factor + 1u;
    const uint64_t output_bytes_per_dxd =
        stream->info.interpolation_factor / 8u;
    if (dxd_capacity >
        (std::numeric_limits<uint64_t>::max() - 1u) /
            output_bytes_per_dxd) {
        set_error(error, CROSSDSD_ERROR_OVERFLOW,
                  "DSD output block is too large");
        return CROSSDSD_ERROR_OVERFLOW;
    }
    const uint64_t required =
        dxd_capacity * output_bytes_per_dxd + 1u;
    if (output_capacity_bytes_per_channel < required ||
        output_stride_bytes < required ||
        input_samples > std::numeric_limits<size_t>::max() /
                            stream->info.channel_count ||
        dxd_capacity > std::numeric_limits<size_t>::max() /
                           stream->info.channel_count) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                  "DSD transcode output buffer is too small");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    try {
        stream->expanded.resize(
            static_cast<size_t>(
                input_samples * stream->info.channel_count));
        stream->reconstructed.resize(
            static_cast<size_t>(
                dxd_capacity * stream->info.channel_count));
    } catch (const std::bad_alloc&) {
        set_error(error, CROSSDSD_ERROR_OUT_OF_MEMORY, "out of memory");
        return CROSSDSD_ERROR_OUT_OF_MEMORY;
    }
    for (uint32_t channel = 0; channel < stream->info.channel_count; ++channel) {
        const auto result = crossdsd_expand_dsd_lsb(
            input_packed_planar +
                static_cast<uint64_t>(channel) * input_stride_bytes,
            input_samples,
            stream->expanded.data() +
                static_cast<uint64_t>(channel) * input_samples,
            input_samples);
        if (result != CROSSDSD_OK) {
            set_error(error, result, "cannot expand packed DSD");
            return result;
        }
    }
    uint64_t reconstructed_frames = 0;
    auto result = crossdsd_decoder_process(
        stream->decoder, stream->expanded.data(), input_samples,
        input_samples, stream->reconstructed.data(), dxd_capacity,
        dxd_capacity, &reconstructed_frames, error);
    if (result != CROSSDSD_OK) return result;
    for (uint32_t channel = 0; channel < stream->info.channel_count; ++channel) {
        const double* values =
            stream->reconstructed.data() +
            static_cast<uint64_t>(channel) * dxd_capacity;
        for (uint64_t frame = 0; frame < reconstructed_frames; ++frame) {
            stream->info.channel_reconstructed_peaks[channel] = std::max(
                stream->info.channel_reconstructed_peaks[channel],
                std::abs(values[frame]));
        }
    }
    if (reconstructed_frames != 0) {
        result = crossdsd_encoder_process_f64(
            stream->encoder, stream->reconstructed.data(),
            reconstructed_frames, dxd_capacity, output_packed_planar,
            output_capacity_bytes_per_channel, output_stride_bytes,
            output_bytes_per_channel, error);
        if (result != CROSSDSD_OK) return result;
    }
    stream->info.input_dsd_samples_per_channel += input_samples;
    stream->info.reconstructed_dxd_frames += reconstructed_frames;
    stream->info.input_sealed =
        valid_bits_in_last_byte < 8 ? 1u : 0u;
    refresh_encoder_info(*stream);
    set_error(error, CROSSDSD_OK, "");
    return CROSSDSD_OK;
}

crossdsd_result crossdsd_dsd_to_dsd_stream_finish(
    crossdsd_dsd_to_dsd_stream* stream, uint8_t* output_packed_planar,
    uint64_t output_capacity_bytes_per_channel, uint64_t output_stride_bytes,
    uint64_t* output_bytes_per_channel, crossdsd_error* error) {
    if (!stream || !output_bytes_per_channel || stream->info.finished ||
        stream->info.input_dsd_samples_per_channel == 0) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                  "invalid DSD-to-DSD stream finish arguments");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    const auto result = crossdsd_encoder_finish(
        stream->encoder, output_packed_planar,
        output_capacity_bytes_per_channel, output_stride_bytes,
        output_bytes_per_channel, error);
    if (result == CROSSDSD_OK) {
        stream->info.input_sealed = 1;
        refresh_encoder_info(*stream);
    }
    return result;
}

}  // extern "C"
