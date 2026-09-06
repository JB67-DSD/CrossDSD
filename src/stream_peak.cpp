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

void update_global_peak(crossdsd_dsd_peak_stream_info& info) {
    info.reconstructed_peak = *std::max_element(
        info.channel_reconstructed_peaks,
        info.channel_reconstructed_peaks + info.channel_count);
    info.reconstructed_peak_dbfs =
        info.reconstructed_peak > 0.0
            ? 20.0 * std::log10(info.reconstructed_peak)
            : -std::numeric_limits<double>::infinity();
}

}  // namespace

struct crossdsd_dsd_peak_stream {
    crossdsd_decoder* decoder = nullptr;
    crossdsd_dsd_peak_stream_info info{};
    std::vector<int8_t> expanded;
    std::vector<double> reconstructed;

    ~crossdsd_dsd_peak_stream() {
        crossdsd_decoder_destroy(decoder);
    }
};

extern "C" {

crossdsd_result crossdsd_dsd_peak_stream_create(
    const crossdsd_dsd_peak_stream_config* config,
    crossdsd_dsd_peak_stream** out_stream, crossdsd_error* error) {
    if (!config ||
        config->struct_size < sizeof(crossdsd_dsd_peak_stream_config) ||
        !out_stream) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                  "invalid DSD peak stream configuration");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    *out_stream = nullptr;
    try {
        auto stream = std::make_unique<crossdsd_dsd_peak_stream>();
        crossdsd_decoder_config decoder_config{};
        decoder_config.struct_size = sizeof(decoder_config);
        decoder_config.input_sample_rate_hz = config->input_sample_rate_hz;
        decoder_config.channel_count = config->channel_count;
        decoder_config.level_mode = CROSSDSD_LEVEL_PCM_CONTINUITY;
        decoder_config.filter_profile = config->filter_profile;
        auto result = crossdsd_decoder_create(
            &decoder_config, &stream->decoder, error);
        if (result != CROSSDSD_OK) return result;
        crossdsd_decoder_info decoder_info{};
        decoder_info.struct_size = sizeof(decoder_info);
        result = crossdsd_decoder_get_info(
            stream->decoder, &decoder_info);
        if (result != CROSSDSD_OK) return result;
        auto& info = stream->info;
        info.struct_size = sizeof(info);
        info.api_version = CROSSDSD_STREAM_API_VERSION;
        info.input_sample_rate_hz = decoder_info.input_sample_rate_hz;
        info.reconstructed_sample_rate_hz =
            decoder_info.output_sample_rate_hz;
        info.channel_count = decoder_info.channel_count;
        info.decimation_factor = decoder_info.decimation_factor;
        info.filter_profile = decoder_info.filter_profile;
        info.latency_reconstructed_samples =
            decoder_info.latency_output_samples;
        info.reconstructed_peak_dbfs =
            -std::numeric_limits<double>::infinity();
        *out_stream = stream.release();
    } catch (const std::bad_alloc&) {
        set_error(error, CROSSDSD_ERROR_OUT_OF_MEMORY, "out of memory");
        return CROSSDSD_ERROR_OUT_OF_MEMORY;
    }
    set_error(error, CROSSDSD_OK, "");
    return CROSSDSD_OK;
}

void crossdsd_dsd_peak_stream_destroy(
    crossdsd_dsd_peak_stream* stream) {
    delete stream;
}

crossdsd_result crossdsd_dsd_peak_stream_get_info(
    const crossdsd_dsd_peak_stream* stream,
    crossdsd_dsd_peak_stream_info* info) {
    if (!stream || !info ||
        info->struct_size < sizeof(crossdsd_dsd_peak_stream_info)) {
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    *info = stream->info;
    return CROSSDSD_OK;
}

crossdsd_result crossdsd_dsd_peak_stream_process(
    crossdsd_dsd_peak_stream* stream,
    const uint8_t* input_packed_planar, uint64_t input_bytes_per_channel,
    uint64_t input_stride_bytes, uint8_t valid_bits_in_last_byte,
    crossdsd_error* error) {
    if (!stream || !input_packed_planar || input_bytes_per_channel == 0 ||
        input_stride_bytes < input_bytes_per_channel ||
        valid_bits_in_last_byte == 0 || valid_bits_in_last_byte > 8 ||
        stream->info.input_sealed || stream->info.finished) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                  "invalid DSD peak stream process arguments");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    if (input_bytes_per_channel >
        (std::numeric_limits<uint64_t>::max() - 7u) / 8u) {
        set_error(error, CROSSDSD_ERROR_OVERFLOW,
                  "DSD peak analysis block is too large");
        return CROSSDSD_ERROR_OVERFLOW;
    }
    const uint64_t input_samples =
        (input_bytes_per_channel - 1u) * 8u + valid_bits_in_last_byte;
    const uint64_t output_capacity =
        input_samples / stream->info.decimation_factor + 1u;
    if (input_samples > std::numeric_limits<size_t>::max() /
                            stream->info.channel_count ||
        output_capacity > std::numeric_limits<size_t>::max() /
                              stream->info.channel_count) {
        set_error(error, CROSSDSD_ERROR_OVERFLOW,
                  "DSD peak analysis block is too large");
        return CROSSDSD_ERROR_OVERFLOW;
    }
    try {
        stream->expanded.resize(
            static_cast<size_t>(
                input_samples * stream->info.channel_count));
        stream->reconstructed.resize(
            static_cast<size_t>(
                output_capacity * stream->info.channel_count));
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
    uint64_t produced = 0;
    const auto result = crossdsd_decoder_process(
        stream->decoder, stream->expanded.data(), input_samples,
        input_samples, stream->reconstructed.data(), output_capacity,
        output_capacity, &produced, error);
    if (result != CROSSDSD_OK) return result;
    for (uint32_t channel = 0; channel < stream->info.channel_count; ++channel) {
        const double* values =
            stream->reconstructed.data() +
            static_cast<uint64_t>(channel) * output_capacity;
        for (uint64_t frame = 0; frame < produced; ++frame) {
            stream->info.channel_reconstructed_peaks[channel] = std::max(
                stream->info.channel_reconstructed_peaks[channel],
                std::abs(values[frame]));
        }
    }
    stream->info.input_dsd_samples_per_channel += input_samples;
    stream->info.reconstructed_frames += produced;
    stream->info.input_sealed =
        valid_bits_in_last_byte < 8 ? 1u : 0u;
    update_global_peak(stream->info);
    set_error(error, CROSSDSD_OK, "");
    return CROSSDSD_OK;
}

crossdsd_result crossdsd_dsd_peak_stream_finish(
    crossdsd_dsd_peak_stream* stream, crossdsd_error* error) {
    if (!stream || stream->info.finished ||
        stream->info.input_dsd_samples_per_channel == 0) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                  "invalid DSD peak stream finish arguments");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    stream->info.input_sealed = 1;
    stream->info.finished = 1;
    update_global_peak(stream->info);
    set_error(error, CROSSDSD_OK, "");
    return CROSSDSD_OK;
}

}  // extern "C"
