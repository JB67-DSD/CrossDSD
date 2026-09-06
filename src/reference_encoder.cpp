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

#include "crossdsd/dsp.h"
#include "enhanced_modulator.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <memory>
#include <new>
#include <vector>

namespace {

constexpr uint32_t kDxdInputRate = 352'800;
constexpr uint32_t kDxdEdgeExtension = 94;
constexpr double kMaximumSourcePcmAmplitude = 1.428893958511103;
constexpr uint64_t kInternalChunk = 4096;

void set_error(crossdsd_error* error, crossdsd_result code, const char* message) {
    if (!error || error->struct_size < sizeof(crossdsd_error)) return;
    error->code = code;
    error->byte_offset = 0;
    std::snprintf(error->message, sizeof(error->message), "%s", message);
}

struct BitPacker {
    uint8_t byte = 0;
    uint8_t bit_count = 0;

    bool push(int8_t bit, uint8_t* output, uint64_t capacity, uint64_t& count) {
        if (bit > 0) byte |= static_cast<uint8_t>(1u << bit_count);
        ++bit_count;
        if (bit_count != 8) return true;
        if (count >= capacity) return false;
        output[count++] = byte;
        byte = 0;
        bit_count = 0;
        return true;
    }

    void reset() {
        byte = 0;
        bit_count = 0;
    }
};

std::vector<double> make_extension(
    const std::vector<double>& samples, bool left, uint32_t extension) {
    std::vector<double> result(extension);
    const size_t count = samples.size();
    for (size_t index = 0; index < result.size(); ++index) {
        const size_t distance = index % count;
        result[index] =
            left ? samples[(extension - 1u - index) % count]
                 : samples[count - 1u - distance];
    }
    return result;
}

}  // namespace

struct crossdsd_encoder {
    crossdsd_encoder_info info{};
    crossdsd_upsampler* upsampler = nullptr;
    crossdsd_modulator* modulator = nullptr;
    crossdsd::enhanced::Modulator* enhanced_modulator = nullptr;
    std::vector<std::vector<double>> initial;
    std::vector<std::vector<double>> tail;
    std::vector<BitPacker> packers;
    uint64_t target_start = 0;
    uint64_t upsampled_generated = 0;
    uint64_t aligned_bits_emitted = 0;
    bool started = false;
    bool profile_enabled = false;
    uint32_t edge_extension = kDxdEdgeExtension;
    double profile_upsample_seconds = 0.0;
    double profile_modulate_seconds = 0.0;
    double profile_pack_seconds = 0.0;

    ~crossdsd_encoder() {
        crossdsd_upsampler_destroy(upsampler);
        crossdsd_modulator_destroy(modulator);
        crossdsd::enhanced::destroy(enhanced_modulator);
    }
};

namespace {

double profile_elapsed(std::chrono::steady_clock::time_point begin) {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now() - begin).count();
}

void write_encoder_profile(const crossdsd_encoder* encoder) {
    const char* path = std::getenv("CROSSDSD_ENCODER_PROFILE_PATH");
    if (!path || !*path) return;
    std::FILE* output = std::fopen(path, "wb");
    if (!output) return;
    std::fprintf(output,
        "{\n"
        "  \"upsample_seconds\": %.9f,\n"
        "  \"modulate_seconds\": %.9f,\n"
        "  \"pack_seconds\": %.9f\n"
        "}\n",
        encoder->profile_upsample_seconds,
        encoder->profile_modulate_seconds,
        encoder->profile_pack_seconds);
    std::fclose(output);
}

void destroy_components(crossdsd_encoder* encoder) {
    if (!encoder) return;
    crossdsd_upsampler_destroy(encoder->upsampler);
    crossdsd_modulator_destroy(encoder->modulator);
    crossdsd::enhanced::destroy(encoder->enhanced_modulator);
    encoder->upsampler = nullptr;
    encoder->modulator = nullptr;
    encoder->enhanced_modulator = nullptr;
}

crossdsd_result pack_bits(
    crossdsd_encoder* encoder, const int8_t* bits, uint64_t count,
    uint64_t bits_stride, uint8_t* output, uint64_t output_capacity,
    uint64_t output_stride, uint64_t& bytes_written,
    crossdsd_error* error) {
    const uint64_t bytes_before = bytes_written;
    for (uint32_t channel = 0; channel < encoder->info.channel_count;
         ++channel) {
        uint64_t channel_count = bytes_before;
        uint8_t* channel_output =
            output + static_cast<uint64_t>(channel) * output_stride;
        const int8_t* channel_bits =
            bits + static_cast<uint64_t>(channel) * bits_stride;
        for (uint64_t sample = 0; sample < count; ++sample) {
            if (!encoder->packers[channel].push(
                    channel_bits[sample], channel_output, output_capacity,
                    channel_count)) {
                set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                          "encoder output buffer is too small");
                return CROSSDSD_ERROR_INVALID_ARGUMENT;
            }
        }
        if (channel == 0) {
            bytes_written = channel_count;
        } else if (channel_count != bytes_written) {
            set_error(error, CROSSDSD_ERROR_INVALID_CHUNK,
                      "encoder channel packers diverged");
            return CROSSDSD_ERROR_INVALID_CHUNK;
        }
    }
    encoder->aligned_bits_emitted += count;
    return CROSSDSD_OK;
}

void update_tail(
    crossdsd_encoder* encoder, const double* input, uint64_t count,
    uint64_t stride) {
    for (uint32_t channel = 0; channel < encoder->info.channel_count; ++channel) {
        auto& tail = encoder->tail[channel];
        const double* source =
            input + static_cast<uint64_t>(channel) * stride;
        if (count >= encoder->edge_extension) {
            tail.assign(
                source + count - encoder->edge_extension, source + count);
            continue;
        }
        if (tail.size() + count > encoder->edge_extension) {
            const size_t remove =
                tail.size() + static_cast<size_t>(count) -
                encoder->edge_extension;
            tail.erase(tail.begin(), tail.begin() + static_cast<ptrdiff_t>(remove));
        }
        tail.insert(tail.end(), source, source + count);
    }
}

crossdsd_result run_input(
    crossdsd_encoder* encoder, const double* input, uint64_t count,
    uint64_t stride, uint64_t target_limit, uint8_t* output,
    uint64_t output_capacity, uint64_t output_stride,
    uint64_t& bytes_written, crossdsd_error* error) {
    uint64_t position = 0;
    while (position < count) {
        const uint64_t current = std::min(kInternalChunk, count - position);
        std::vector<double> oversampled;
        std::vector<int8_t> bits;
        try {
            oversampled.resize(
                static_cast<size_t>(
                    current * encoder->info.interpolation_factor) *
                encoder->info.channel_count);
        } catch (const std::bad_alloc&) {
            set_error(error, CROSSDSD_ERROR_OUT_OF_MEMORY, "out of memory");
            return CROSSDSD_ERROR_OUT_OF_MEMORY;
        }
        uint64_t produced = 0;
        auto profile_start = std::chrono::steady_clock::time_point{};
        if (encoder->profile_enabled)
            profile_start = std::chrono::steady_clock::now();
        const auto upsample_result = crossdsd_upsampler_process_f64(
            encoder->upsampler, input + position, current, stride,
            oversampled.data(),
            current * encoder->info.interpolation_factor,
            current * encoder->info.interpolation_factor,
            &produced, error);
        if (encoder->profile_enabled)
            encoder->profile_upsample_seconds += profile_elapsed(profile_start);
        if (upsample_result != CROSSDSD_OK) return upsample_result;

        const uint64_t generated_begin = encoder->upsampled_generated;
        const uint64_t generated_end = generated_begin + produced;
        encoder->upsampled_generated = generated_end;
        const uint64_t selected_begin =
            std::max(generated_begin, encoder->target_start);
        const uint64_t selected_end =
            std::min(generated_end, target_limit);
        if (selected_begin >= selected_end) {
            position += current;
            continue;
        }
        const uint64_t skip = selected_begin - generated_begin;
        const uint64_t selected = selected_end - selected_begin;
        try {
            bits.resize(
                static_cast<size_t>(selected) * encoder->info.channel_count);
        } catch (const std::bad_alloc&) {
            set_error(error, CROSSDSD_ERROR_OUT_OF_MEMORY, "out of memory");
            return CROSSDSD_ERROR_OUT_OF_MEMORY;
        }
        uint64_t modulated = selected;
        if (encoder->profile_enabled)
            profile_start = std::chrono::steady_clock::now();
        const auto modulation_result =
            encoder->info.encoder_profile == CROSSDSD_ENCODER_DSD64_ENHANCED
                ? crossdsd::enhanced::process(
                    encoder->enhanced_modulator, oversampled.data() + skip,
                    selected, produced, bits.data(), selected, selected,
                    &modulated)
                : crossdsd_modulator_process(
                    encoder->modulator, oversampled.data() + skip, selected,
                    produced, bits.data(), selected, selected, error);
        if (encoder->profile_enabled)
            encoder->profile_modulate_seconds += profile_elapsed(profile_start);
        if (modulation_result != CROSSDSD_OK) {
            if (modulation_result == CROSSDSD_ERROR_INVALID_ARGUMENT) {
                double peak = 0.0;
                for (uint32_t channel = 0;
                     channel < encoder->info.channel_count; ++channel) {
                    const double* channel_input =
                        oversampled.data() +
                        static_cast<uint64_t>(channel) * produced + skip;
                    for (uint64_t sample = 0; sample < selected; ++sample) {
                        peak = std::max(peak, std::abs(channel_input[sample]));
                    }
                }
                if (error && error->struct_size >= sizeof(crossdsd_error)) {
                    std::snprintf(
                        error->message, sizeof(error->message),
                        "post-filter PCM peak %.12g exceeds the +3.1 dB SACD domain",
                        peak);
                }
            }
            if (encoder->enhanced_modulator) {
                encoder->info.stability_failure =
                    crossdsd::enhanced::stability_failure(
                        encoder->enhanced_modulator);
            } else {
                crossdsd_modulator_info modulator_info{};
                modulator_info.struct_size = sizeof(modulator_info);
                if (crossdsd_modulator_get_info(
                        encoder->modulator, &modulator_info) == CROSSDSD_OK) {
                    encoder->info.stability_failure =
                        modulator_info.stability_failure;
                }
            }
            return modulation_result;
        }
        if (encoder->profile_enabled)
            profile_start = std::chrono::steady_clock::now();
        const auto pack_result = pack_bits(
            encoder, bits.data(), modulated, selected, output,
            output_capacity, output_stride, bytes_written, error);
        if (encoder->profile_enabled)
            encoder->profile_pack_seconds += profile_elapsed(profile_start);
        if (pack_result != CROSSDSD_OK) return pack_result;
        position += current;
    }
    return CROSSDSD_OK;
}

crossdsd_result start_encoder(
    crossdsd_encoder* encoder, uint8_t* output, uint64_t output_capacity,
    uint64_t output_stride, uint64_t& bytes_written, crossdsd_error* error) {
    const uint64_t buffered = encoder->initial[0].size();
    std::vector<double> extended;
    try {
        extended.resize(
            static_cast<size_t>(encoder->edge_extension + buffered) *
            encoder->info.channel_count);
    } catch (const std::bad_alloc&) {
        set_error(error, CROSSDSD_ERROR_OUT_OF_MEMORY, "out of memory");
        return CROSSDSD_ERROR_OUT_OF_MEMORY;
    }
    const uint64_t stride = encoder->edge_extension + buffered;
    for (uint32_t channel = 0; channel < encoder->info.channel_count; ++channel) {
        const auto prefix = make_extension(
            encoder->initial[channel], true, encoder->edge_extension);
        double* destination =
            extended.data() + static_cast<uint64_t>(channel) * stride;
        std::copy(prefix.begin(), prefix.end(), destination);
        std::copy(
            encoder->initial[channel].begin(), encoder->initial[channel].end(),
            destination + encoder->edge_extension);
    }
    encoder->started = true;
    const auto result = run_input(
        encoder, extended.data(), stride, stride,
        std::numeric_limits<uint64_t>::max(), output, output_capacity,
        output_stride, bytes_written, error);
    for (auto& channel : encoder->initial) channel.clear();
    return result;
}

}  // namespace

extern "C" {

crossdsd_result crossdsd_encoder_create(
    const crossdsd_encoder_config* config, crossdsd_encoder** out_encoder,
    crossdsd_error* error) {
    if (!config || config->struct_size < sizeof(crossdsd_encoder_config) ||
        !out_encoder) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                  "invalid encoder configuration");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    *out_encoder = nullptr;
    const bool dsd64 =
        config->output_sample_rate_hz == 2'822'400u &&
        config->modulator_profile ==
            CROSSDSD_MODULATOR_CRFB_ORDER7_HINF_1_32;
    const bool dsd128 =
        config->output_sample_rate_hz == 5'644'800u &&
        config->modulator_profile ==
            CROSSDSD_MODULATOR_DSD128_CRFB_ORDER5_HINF_1_30;
    const bool dsd256 =
        config->output_sample_rate_hz == 11'289'600u &&
        config->modulator_profile ==
            CROSSDSD_MODULATOR_DSD256_CRFB_ORDER7_HINF_1_20;
    const bool dsd512 =
        config->output_sample_rate_hz == 22'579'200u &&
        config->modulator_profile ==
            CROSSDSD_MODULATOR_DSD512_CRFB_ORDER7_HINF_1_20;
    const bool enhanced =
        config->encoder_profile == CROSSDSD_ENCODER_DSD64_ENHANCED;
    const bool supported_input =
        config->input_sample_rate_hz == 352'800u ||
        (config->input_sample_rate_hz == 705'600u && dsd512);
    if (!supported_input ||
        (!dsd64 && !dsd128 && !dsd256 && !dsd512) ||
        (enhanced && !dsd64) ||
        (config->encoder_profile != CROSSDSD_ENCODER_STANDARD &&
         !enhanced)) {
        set_error(error, CROSSDSD_ERROR_UNSUPPORTED_RATE,
                  "encoder requires a matching DSD64/128/256/512 profile");
        return CROSSDSD_ERROR_UNSUPPORTED_RATE;
    }
    if (config->channel_count == 0 || config->channel_count > 256) {
        set_error(error, CROSSDSD_ERROR_UNSUPPORTED_CHANNELS,
                  "channel count must be between 1 and 256");
        return CROSSDSD_ERROR_UNSUPPORTED_CHANNELS;
    }
    try {
        auto encoder = std::make_unique<crossdsd_encoder>();
        encoder->edge_extension = static_cast<uint32_t>(
            static_cast<uint64_t>(kDxdEdgeExtension) *
            config->input_sample_rate_hz / kDxdInputRate);
        const char* profile_path = std::getenv("CROSSDSD_ENCODER_PROFILE_PATH");
        encoder->profile_enabled = profile_path && *profile_path;
        crossdsd_upsampler_config upsampler_config{};
        upsampler_config.struct_size = sizeof(upsampler_config);
        upsampler_config.input_sample_rate_hz = config->input_sample_rate_hz;
        upsampler_config.output_sample_rate_hz =
            config->output_sample_rate_hz;
        upsampler_config.channel_count = config->channel_count;
        auto result = crossdsd_upsampler_create(
            &upsampler_config, &encoder->upsampler, error);
        if (result != CROSSDSD_OK) return result;

        crossdsd_modulator_config modulator_config{};
        modulator_config.struct_size = sizeof(modulator_config);
        modulator_config.sample_rate_hz = config->output_sample_rate_hz;
        modulator_config.channel_count = config->channel_count;
        modulator_config.profile = config->modulator_profile;
        if (enhanced) {
            encoder->enhanced_modulator =
                crossdsd::enhanced::create(config->channel_count);
            if (!encoder->enhanced_modulator) {
                destroy_components(encoder.get());
                set_error(error, CROSSDSD_ERROR_OUT_OF_MEMORY, "out of memory");
                return CROSSDSD_ERROR_OUT_OF_MEMORY;
            }
        } else {
            result = crossdsd_modulator_create(
                &modulator_config, &encoder->modulator, error);
            if (result != CROSSDSD_OK) {
                destroy_components(encoder.get());
                return result;
            }
        }
        encoder->initial.resize(config->channel_count);
        encoder->tail.resize(config->channel_count);
        encoder->packers.resize(config->channel_count);

        auto& info = encoder->info;
        info.struct_size = sizeof(info);
        info.api_version = CROSSDSD_DSP_API_VERSION;
        info.input_sample_rate_hz = config->input_sample_rate_hz;
        info.output_sample_rate_hz = config->output_sample_rate_hz;
        info.channel_count = config->channel_count;
        info.interpolation_factor =
            config->output_sample_rate_hz / config->input_sample_rate_hz;
        info.edge_extension_input_samples = encoder->edge_extension;
        crossdsd_upsampler_info upsampler_info{};
        upsampler_info.struct_size = sizeof(upsampler_info);
        crossdsd_upsampler_get_info(encoder->upsampler, &upsampler_info);
        info.latency_output_samples =
            upsampler_info.latency_output_samples;
        encoder->target_start =
            static_cast<uint64_t>(encoder->edge_extension) *
                info.interpolation_factor +
            static_cast<uint64_t>(std::llround(
                info.latency_output_samples));
        info.maximum_pcm_amplitude = kMaximumSourcePcmAmplitude;
        info.encoder_profile = config->encoder_profile;
        *out_encoder = encoder.release();
    } catch (const std::bad_alloc&) {
        set_error(error, CROSSDSD_ERROR_OUT_OF_MEMORY, "out of memory");
        return CROSSDSD_ERROR_OUT_OF_MEMORY;
    }
    set_error(error, CROSSDSD_OK, "");
    return CROSSDSD_OK;
}

void crossdsd_encoder_destroy(crossdsd_encoder* encoder) {
    destroy_components(encoder);
    delete encoder;
}

crossdsd_result crossdsd_encoder_get_info(
    const crossdsd_encoder* encoder, crossdsd_encoder_info* info) {
    if (!encoder || !info ||
        info->struct_size < sizeof(crossdsd_encoder_info)) {
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    *info = encoder->info;
    return CROSSDSD_OK;
}

crossdsd_result crossdsd_encoder_get_channel_metrics(
    const crossdsd_encoder* encoder, uint32_t channel_index,
    crossdsd_modulator_channel_metrics* metrics) {
    if (!encoder) return CROSSDSD_ERROR_INVALID_ARGUMENT;
    return encoder->enhanced_modulator
        ? crossdsd::enhanced::get_channel_metrics(
            encoder->enhanced_modulator, channel_index, metrics)
        : crossdsd_modulator_get_channel_metrics(
            encoder->modulator, channel_index, metrics);
}

crossdsd_result crossdsd_encoder_process(
    crossdsd_encoder* encoder, const float* input_planar,
    uint64_t input_count_per_channel, uint64_t input_stride,
    uint8_t* output_planar, uint64_t output_capacity_per_channel,
    uint64_t output_stride, uint64_t* output_bytes_per_channel,
    crossdsd_error* error) {
    if (input_count_per_channel == 0) {
        return crossdsd_encoder_process_f64(
            encoder, nullptr, 0, 0, output_planar,
            output_capacity_per_channel, output_stride,
            output_bytes_per_channel, error);
    }
    if (!encoder || !input_planar ||
        input_stride < input_count_per_channel ||
        input_count_per_channel >
            std::numeric_limits<size_t>::max() /
                std::max<uint32_t>(encoder->info.channel_count, 1u)) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                  "invalid encoder process arguments");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    try {
        std::vector<double> input_f64(
            static_cast<size_t>(input_count_per_channel) *
            encoder->info.channel_count);
        for (uint32_t channel = 0; channel < encoder->info.channel_count;
             ++channel) {
            const float* source =
                input_planar + static_cast<uint64_t>(channel) * input_stride;
            double* destination =
                input_f64.data() +
                static_cast<uint64_t>(channel) * input_count_per_channel;
            std::copy(
                source, source + input_count_per_channel, destination);
        }
        return crossdsd_encoder_process_f64(
            encoder, input_f64.data(), input_count_per_channel,
            input_count_per_channel, output_planar,
            output_capacity_per_channel, output_stride,
            output_bytes_per_channel, error);
    } catch (const std::bad_alloc&) {
        set_error(error, CROSSDSD_ERROR_OUT_OF_MEMORY, "out of memory");
        return CROSSDSD_ERROR_OUT_OF_MEMORY;
    }
}

crossdsd_result crossdsd_encoder_process_f64(
    crossdsd_encoder* encoder, const double* input_planar,
    uint64_t input_count_per_channel, uint64_t input_stride,
    uint8_t* output_planar, uint64_t output_capacity_per_channel,
    uint64_t output_stride, uint64_t* output_bytes_per_channel,
    crossdsd_error* error) {
    if (!encoder || !output_bytes_per_channel ||
        (input_count_per_channel != 0 && (!input_planar || !output_planar)) ||
        input_stride < input_count_per_channel ||
        input_count_per_channel == std::numeric_limits<uint64_t>::max()) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                  "invalid encoder process arguments");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    *output_bytes_per_channel = 0;
    if (encoder->info.finished) {
        set_error(error, CROSSDSD_ERROR_END_OF_STREAM,
                  "encoder has already been finished");
        return CROSSDSD_ERROR_END_OF_STREAM;
    }
    if (input_count_per_channel == 0) {
        set_error(error, CROSSDSD_OK, "");
        return CROSSDSD_OK;
    }
    const uint64_t bytes_per_input =
        encoder->info.interpolation_factor / 8u;
    if (input_count_per_channel >
            (std::numeric_limits<uint64_t>::max() - 1u) /
                bytes_per_input ||
        output_capacity_per_channel <
            input_count_per_channel * bytes_per_input + 1u ||
        output_stride < input_count_per_channel * bytes_per_input + 1u) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                  "encoder process output buffer is too small");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    if (input_count_per_channel >
        std::numeric_limits<uint64_t>::max() -
            encoder->info.input_samples_consumed_per_channel ||
        encoder->info.input_samples_consumed_per_channel +
                input_count_per_channel >
            std::numeric_limits<uint64_t>::max() /
                encoder->info.interpolation_factor) {
        set_error(error, CROSSDSD_ERROR_OVERFLOW,
                  "encoder sample count overflow");
        return CROSSDSD_ERROR_OVERFLOW;
    }
    for (uint32_t channel = 0; channel < encoder->info.channel_count; ++channel) {
        const double* input =
            input_planar + static_cast<uint64_t>(channel) * input_stride;
        for (uint64_t sample = 0; sample < input_count_per_channel; ++sample) {
            if (!std::isfinite(input[sample]) ||
                std::abs(input[sample]) > kMaximumSourcePcmAmplitude) {
                set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                          "DXD input exceeds the +3.1 dB SACD source domain");
                return CROSSDSD_ERROR_INVALID_ARGUMENT;
            }
        }
    }

    try {
        encoder->info.input_samples_consumed_per_channel +=
            input_count_per_channel;
        update_tail(encoder, input_planar, input_count_per_channel, input_stride);
        uint64_t bytes_written = 0;
        crossdsd_result result = CROSSDSD_OK;
        if (!encoder->started) {
            for (uint32_t channel = 0; channel < encoder->info.channel_count;
                 ++channel) {
                const double* source =
                    input_planar +
                    static_cast<uint64_t>(channel) * input_stride;
                encoder->initial[channel].insert(
                    encoder->initial[channel].end(), source,
                    source + input_count_per_channel);
            }
            if (encoder->initial[0].size() >= encoder->edge_extension) {
                result = start_encoder(
                    encoder, output_planar, output_capacity_per_channel,
                    output_stride, bytes_written, error);
            }
        } else {
            result = run_input(
                encoder, input_planar, input_count_per_channel, input_stride,
                std::numeric_limits<uint64_t>::max(), output_planar,
                output_capacity_per_channel, output_stride, bytes_written,
                error);
        }
        if (result != CROSSDSD_OK) return result;
        encoder->info.dsd_samples_produced_per_channel =
            encoder->aligned_bits_emitted;
        encoder->info.packed_bytes_produced_per_channel += bytes_written;
        *output_bytes_per_channel = bytes_written;
    } catch (const std::bad_alloc&) {
        set_error(error, CROSSDSD_ERROR_OUT_OF_MEMORY, "out of memory");
        return CROSSDSD_ERROR_OUT_OF_MEMORY;
    }
    set_error(error, CROSSDSD_OK, "");
    return CROSSDSD_OK;
}

crossdsd_result crossdsd_encoder_finish(
    crossdsd_encoder* encoder, uint8_t* output_planar,
    uint64_t output_capacity_per_channel, uint64_t output_stride,
    uint64_t* output_bytes_per_channel, crossdsd_error* error) {
    if (!encoder || !output_bytes_per_channel) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                  "invalid encoder finish arguments");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    *output_bytes_per_channel = 0;
    if (encoder->info.finished) {
        set_error(error, CROSSDSD_ERROR_END_OF_STREAM,
                  "encoder has already been finished");
        return CROSSDSD_ERROR_END_OF_STREAM;
    }
    const uint64_t required =
        encoder->info.input_samples_consumed_per_channel *
            (encoder->info.interpolation_factor / 8u) -
        encoder->info.packed_bytes_produced_per_channel;
    if (required != 0 &&
        (!output_planar || output_capacity_per_channel < required ||
         output_stride < required)) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                  "encoder finish output buffer is too small");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    if (encoder->info.input_samples_consumed_per_channel == 0) {
        encoder->info.finished = 1;
        set_error(error, CROSSDSD_OK, "");
        return CROSSDSD_OK;
    }

    try {
        uint64_t bytes_written = 0;
        const uint64_t target_end =
            encoder->target_start +
            encoder->info.input_samples_consumed_per_channel *
                encoder->info.interpolation_factor;
        crossdsd_result result = CROSSDSD_OK;
        if (!encoder->started) {
            const uint64_t source_count = encoder->initial[0].size();
            const uint64_t extended_count =
                encoder->edge_extension + source_count +
                encoder->edge_extension;
            std::vector<double> extended;
            extended.resize(
                static_cast<size_t>(extended_count) *
                encoder->info.channel_count);
            for (uint32_t channel = 0; channel < encoder->info.channel_count;
                 ++channel) {
                const auto prefix =
                    make_extension(encoder->initial[channel], true,
                                   encoder->edge_extension);
                const auto suffix =
                    make_extension(encoder->initial[channel], false,
                                   encoder->edge_extension);
                double* destination =
                    extended.data() +
                    static_cast<uint64_t>(channel) * extended_count;
                std::copy(prefix.begin(), prefix.end(), destination);
                std::copy(
                    encoder->initial[channel].begin(),
                    encoder->initial[channel].end(),
                    destination + encoder->edge_extension);
                std::copy(
                    suffix.begin(), suffix.end(),
                    destination + encoder->edge_extension + source_count);
            }
            encoder->started = true;
            result = run_input(
                encoder, extended.data(), extended_count, extended_count,
                target_end, output_planar, output_capacity_per_channel,
                output_stride, bytes_written, error);
        } else {
            std::vector<double> suffix(
                static_cast<size_t>(encoder->edge_extension) *
                encoder->info.channel_count);
            for (uint32_t channel = 0; channel < encoder->info.channel_count;
                 ++channel) {
                const auto extension =
                    make_extension(encoder->tail[channel], false,
                                   encoder->edge_extension);
                std::copy(
                    extension.begin(), extension.end(),
                    suffix.begin() +
                        static_cast<ptrdiff_t>(
                            static_cast<uint64_t>(channel) *
                            encoder->edge_extension));
            }
            result = run_input(
                encoder, suffix.data(), encoder->edge_extension,
                encoder->edge_extension,
                target_end, output_planar, output_capacity_per_channel,
                output_stride, bytes_written, error);
        }
        if (result != CROSSDSD_OK) return result;
        if (encoder->enhanced_modulator) {
            constexpr uint64_t capacity = 8;
            std::vector<int8_t> final_bits(
                static_cast<size_t>(capacity) *
                encoder->info.channel_count);
            uint64_t final_count = 0;
            result = crossdsd::enhanced::finish(
                encoder->enhanced_modulator, final_bits.data(), capacity,
                capacity, &final_count);
            if (result != CROSSDSD_OK) {
                encoder->info.stability_failure =
                    crossdsd::enhanced::stability_failure(
                        encoder->enhanced_modulator);
                set_error(
                    error, result,
                    "DSD64 Enhanced look-ahead finalization failed");
                return result;
            }
            result = pack_bits(
                encoder, final_bits.data(), final_count, capacity,
                output_planar, output_capacity_per_channel, output_stride,
                bytes_written, error);
            if (result != CROSSDSD_OK) return result;
            encoder->info.enhanced_dsd_samples_per_channel =
                crossdsd::enhanced::enhanced_samples(
                    encoder->enhanced_modulator);
            encoder->info.enhanced_segment_count =
                crossdsd::enhanced::enhanced_segments(
                    encoder->enhanced_modulator);
        }
        if (encoder->aligned_bits_emitted !=
            encoder->info.input_samples_consumed_per_channel *
                encoder->info.interpolation_factor) {
            char message[160]{};
            std::snprintf(
                message, sizeof(message),
                "encoder final sample count is inconsistent (%llu emitted, %llu expected; %llu generated, start %llu)",
                static_cast<unsigned long long>(encoder->aligned_bits_emitted),
                static_cast<unsigned long long>(
                    encoder->info.input_samples_consumed_per_channel *
                    encoder->info.interpolation_factor),
                static_cast<unsigned long long>(encoder->upsampled_generated),
                static_cast<unsigned long long>(encoder->target_start));
            set_error(error, CROSSDSD_ERROR_INVALID_CHUNK, message);
            return CROSSDSD_ERROR_INVALID_CHUNK;
        }
        for (const auto& packer : encoder->packers) {
            if (packer.bit_count != 0) {
                set_error(error, CROSSDSD_ERROR_INVALID_CHUNK,
                          "encoder final packed byte is incomplete");
                return CROSSDSD_ERROR_INVALID_CHUNK;
            }
        }
        encoder->info.dsd_samples_produced_per_channel =
            encoder->aligned_bits_emitted;
        encoder->info.packed_bytes_produced_per_channel += bytes_written;
        encoder->info.finished = 1;
        *output_bytes_per_channel = bytes_written;
    } catch (const std::bad_alloc&) {
        set_error(error, CROSSDSD_ERROR_OUT_OF_MEMORY, "out of memory");
        return CROSSDSD_ERROR_OUT_OF_MEMORY;
    }
    write_encoder_profile(encoder);
    set_error(error, CROSSDSD_OK, "");
    return CROSSDSD_OK;
}

crossdsd_result crossdsd_encoder_reset(crossdsd_encoder* encoder) {
    if (!encoder) return CROSSDSD_ERROR_INVALID_ARGUMENT;
    const auto upsampler_result =
        crossdsd_upsampler_reset(encoder->upsampler);
    const auto modulator_result = encoder->enhanced_modulator
        ? crossdsd::enhanced::reset(encoder->enhanced_modulator)
        : crossdsd_modulator_reset(encoder->modulator);
    if (upsampler_result != CROSSDSD_OK) return upsampler_result;
    if (modulator_result != CROSSDSD_OK) return modulator_result;
    for (auto& channel : encoder->initial) channel.clear();
    for (auto& channel : encoder->tail) channel.clear();
    for (auto& packer : encoder->packers) packer.reset();
    encoder->upsampled_generated = 0;
    encoder->aligned_bits_emitted = 0;
    encoder->started = false;
    encoder->info.input_samples_consumed_per_channel = 0;
    encoder->info.dsd_samples_produced_per_channel = 0;
    encoder->info.packed_bytes_produced_per_channel = 0;
    encoder->info.finished = 0;
    encoder->info.stability_failure = 0;
    encoder->info.enhanced_dsd_samples_per_channel = 0;
    encoder->info.enhanced_segment_count = 0;
    encoder->profile_upsample_seconds = 0.0;
    encoder->profile_modulate_seconds = 0.0;
    encoder->profile_pack_seconds = 0.0;
    return CROSSDSD_OK;
}

}  // extern "C"
