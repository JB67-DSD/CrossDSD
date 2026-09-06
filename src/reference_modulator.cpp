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

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <span>
#include <vector>

#if defined(CROSSDSD_ENABLE_CHANNEL_THREADS)
#include <thread>
#endif

namespace {

constexpr uint32_t kDsd64Rate = 2'822'400;
constexpr uint32_t kDsd128Rate = 5'644'800;
constexpr uint32_t kDsd256Rate = 11'289'600;
constexpr uint32_t kDsd512Rate = 22'579'200;
constexpr double kMaximumPcmAmplitude = 1.6;
constexpr double kPcmToModulator = 0.5;
constexpr double kAbortState = 8.0;
constexpr double kAbortQuantizerInput = 8.0;

#include "reference_modulator_coefficients.inc"
#include "reference_modulator_dsd128_coefficients.inc"
#include "reference_modulator_dsd256_coefficients.inc"

/* Experimental CRFB realization under Phase 7 scientific validation. */
constexpr auto kMatrix = kOrder7H132;

struct ModulatorDesign {
    uint32_t sample_rate;
    uint32_t order;
    double h_inf;
    std::span<const double> matrix;
    crossdsd_modulator_profile profile;
};

const ModulatorDesign kDsd64Design{
    kDsd64Rate, 7, 1.32, kMatrix,
    CROSSDSD_MODULATOR_CRFB_ORDER7_HINF_1_32};
const ModulatorDesign kDsd128Design{
    kDsd128Rate, 5, 1.30, kDsd128Order5H13,
    CROSSDSD_MODULATOR_DSD128_CRFB_ORDER5_HINF_1_30};
const ModulatorDesign kDsd256Design{
    kDsd256Rate, 7, 1.20, kDsd256Order7H12,
    CROSSDSD_MODULATOR_DSD256_CRFB_ORDER7_HINF_1_20};
const ModulatorDesign kDsd512Design{
    kDsd512Rate, 7, 1.20, kDsd256Order7H12,
    CROSSDSD_MODULATOR_DSD512_CRFB_ORDER7_HINF_1_20};

void set_error(crossdsd_error* error, crossdsd_result code, const char* message) {
    if (!error || error->struct_size < sizeof(crossdsd_error)) return;
    error->code = code;
    error->byte_offset = 0;
    std::snprintf(error->message, sizeof(error->message), "%s", message);
}

struct ChannelState {
    std::array<double, 8> state{};
    double maximum_absolute_input = 0.0;
    double maximum_absolute_state = 0.0;
    double maximum_absolute_quantizer_input = 0.0;
    uint64_t maximum_identical_bit_run = 0;
    uint64_t current_identical_bit_run = 0;
    uint64_t one_count = 0;
    uint64_t sample_count = 0;
    int8_t previous_bit = 0;
};

template <uint32_t Order>
bool process_sample_order(
    const ModulatorDesign& design, ChannelState& channel, double pcm_input,
    int8_t& output) {
    static_assert(Order == 5u || Order == 7u);
    const double* matrix = design.matrix.data();
    constexpr uint32_t stride = Order + 2u;
    const double input = kPcmToModulator * pcm_input;
    double quantizer_input = matrix[Order * stride + Order] * input;
    for (uint32_t column = 0; column < Order; ++column) {
        quantizer_input += matrix[Order * stride + column] * channel.state[column];
    }
    const int8_t bit = quantizer_input >= 0.0 ? 1 : -1;

    std::array<double, 8> next{};
    for (uint32_t row = 0; row < Order; ++row) {
        const double* coefficients = matrix + row * stride;
        double value = coefficients[Order] * input +
                       coefficients[Order + 1u] * bit;
        for (uint32_t column = 0; column < Order; ++column) {
            value += coefficients[column] * channel.state[column];
        }
        next[row] = value;
    }
    for (uint32_t row = 0; row < Order; ++row) {
        const double value = next[row];
        if (!std::isfinite(value) || std::abs(value) > kAbortState) return false;
    }
    if (!std::isfinite(quantizer_input) ||
        std::abs(quantizer_input) > kAbortQuantizerInput) {
        return false;
    }

    channel.state = next;
    channel.maximum_absolute_input =
        std::max(channel.maximum_absolute_input, std::abs(pcm_input));
    channel.maximum_absolute_quantizer_input =
        std::max(channel.maximum_absolute_quantizer_input,
                 std::abs(quantizer_input));
    for (uint32_t row = 0; row < Order; ++row) {
        const double value = next[row];
        channel.maximum_absolute_state =
            std::max(channel.maximum_absolute_state, std::abs(value));
    }
    channel.current_identical_bit_run =
        bit == channel.previous_bit ? channel.current_identical_bit_run + 1u : 1u;
    channel.previous_bit = bit;
    channel.maximum_identical_bit_run =
        std::max(channel.maximum_identical_bit_run,
                 channel.current_identical_bit_run);
    channel.one_count += bit > 0;
    ++channel.sample_count;
    output = bit;
    return true;
}

}  // namespace

struct crossdsd_modulator {
    crossdsd_modulator_info info{};
    const ModulatorDesign* design = nullptr;
    std::vector<ChannelState> channels;
    bool parallel_channels = true;
};

extern "C" {

crossdsd_result crossdsd_modulator_create(
    const crossdsd_modulator_config* config,
    crossdsd_modulator** out_modulator,
    crossdsd_error* error) {
    if (!config || config->struct_size < sizeof(crossdsd_modulator_config) ||
        !out_modulator) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                  "invalid modulator configuration");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    *out_modulator = nullptr;
    const ModulatorDesign* design = nullptr;
    if (config->sample_rate_hz == kDsd64Rate &&
        config->profile == CROSSDSD_MODULATOR_CRFB_ORDER7_HINF_1_32) {
        design = &kDsd64Design;
    } else if (
        config->sample_rate_hz == kDsd128Rate &&
        config->profile ==
            CROSSDSD_MODULATOR_DSD128_CRFB_ORDER5_HINF_1_30) {
        design = &kDsd128Design;
    } else if (
        config->sample_rate_hz == kDsd256Rate &&
        config->profile ==
            CROSSDSD_MODULATOR_DSD256_CRFB_ORDER7_HINF_1_20) {
        design = &kDsd256Design;
    } else if (
        config->sample_rate_hz == kDsd512Rate &&
        config->profile ==
            CROSSDSD_MODULATOR_DSD512_CRFB_ORDER7_HINF_1_20) {
        design = &kDsd512Design;
    }
    if (!design) {
        set_error(error, CROSSDSD_ERROR_UNSUPPORTED_RATE,
                  "modulator profile does not match the DSD rate");
        return CROSSDSD_ERROR_UNSUPPORTED_RATE;
    }
    if (config->channel_count == 0 || config->channel_count > 256) {
        set_error(error, CROSSDSD_ERROR_UNSUPPORTED_CHANNELS,
                  "channel count must be between 1 and 256");
        return CROSSDSD_ERROR_UNSUPPORTED_CHANNELS;
    }
    try {
        auto modulator = std::make_unique<crossdsd_modulator>();
        modulator->parallel_channels =
            std::getenv("CROSSDSD_DISABLE_MODULATOR_CHANNEL_THREADS") == nullptr;
        modulator->design = design;
        modulator->channels.resize(config->channel_count);
        auto& info = modulator->info;
        info.struct_size = sizeof(info);
        info.api_version = CROSSDSD_DSP_API_VERSION;
        info.sample_rate_hz = config->sample_rate_hz;
        info.channel_count = config->channel_count;
        info.profile = config->profile;
        info.modulator_order = design->order;
        info.h_inf = design->h_inf;
        info.maximum_pcm_amplitude = kMaximumPcmAmplitude;
        *out_modulator = modulator.release();
    } catch (const std::bad_alloc&) {
        set_error(error, CROSSDSD_ERROR_OUT_OF_MEMORY, "out of memory");
        return CROSSDSD_ERROR_OUT_OF_MEMORY;
    }
    set_error(error, CROSSDSD_OK, "");
    return CROSSDSD_OK;
}

void crossdsd_modulator_destroy(crossdsd_modulator* modulator) {
    delete modulator;
}

crossdsd_result crossdsd_modulator_get_info(
    const crossdsd_modulator* modulator,
    crossdsd_modulator_info* info) {
    if (!modulator || !info ||
        info->struct_size < sizeof(crossdsd_modulator_info)) {
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    *info = modulator->info;
    return CROSSDSD_OK;
}

crossdsd_result crossdsd_modulator_get_channel_metrics(
    const crossdsd_modulator* modulator, uint32_t channel_index,
    crossdsd_modulator_channel_metrics* metrics) {
    if (!modulator || !metrics ||
        metrics->struct_size < sizeof(crossdsd_modulator_channel_metrics) ||
        channel_index >= modulator->channels.size()) {
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    const auto& channel = modulator->channels[channel_index];
    metrics->channel_index = channel_index;
    metrics->maximum_absolute_input = channel.maximum_absolute_input;
    metrics->maximum_absolute_state = channel.maximum_absolute_state;
    metrics->maximum_absolute_quantizer_input =
        channel.maximum_absolute_quantizer_input;
    metrics->maximum_identical_bit_run =
        channel.maximum_identical_bit_run;
    metrics->one_density =
        channel.sample_count == 0
            ? 0.0
            : static_cast<double>(channel.one_count) /
                  static_cast<double>(channel.sample_count);
    return CROSSDSD_OK;
}

crossdsd_result crossdsd_modulator_process(
    crossdsd_modulator* modulator, const double* input_planar,
    uint64_t input_count_per_channel, uint64_t input_stride,
    int8_t* output_planar, uint64_t output_capacity_per_channel,
    uint64_t output_stride, crossdsd_error* error) {
    if (!modulator ||
        (input_count_per_channel != 0 && (!input_planar || !output_planar)) ||
        input_stride < input_count_per_channel ||
        output_capacity_per_channel < input_count_per_channel ||
        output_stride < input_count_per_channel) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                  "invalid modulator process arguments");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    if (modulator->info.stability_failure) {
        set_error(error, CROSSDSD_ERROR_OVERFLOW,
                  "modulator is latched after a stability failure; reset required");
        return CROSSDSD_ERROR_OVERFLOW;
    }
    if (input_count_per_channel == 0) {
        set_error(error, CROSSDSD_OK, "");
        return CROSSDSD_OK;
    }

    for (uint32_t channel = 0; channel < modulator->info.channel_count; ++channel) {
        const double* input =
            input_planar + static_cast<uint64_t>(channel) * input_stride;
        for (uint64_t sample = 0; sample < input_count_per_channel; ++sample) {
            if (!std::isfinite(input[sample]) ||
                std::abs(input[sample]) > kMaximumPcmAmplitude) {
                set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                          "PCM input exceeds the +3.1 dB SACD domain");
                return CROSSDSD_ERROR_INVALID_ARGUMENT;
            }
        }
    }

    std::vector<uint8_t> stable(modulator->info.channel_count, 1u);
    const auto process_channel = [&](uint32_t channel) {
        for (uint64_t sample = 0; sample < input_count_per_channel; ++sample) {
            const auto offset =
                static_cast<uint64_t>(channel) * input_stride + sample;
            const auto output_offset =
                static_cast<uint64_t>(channel) * output_stride + sample;
            const bool ok = modulator->design->order == 5u
                ? process_sample_order<5u>(
                    *modulator->design, modulator->channels[channel],
                    input_planar[offset], output_planar[output_offset])
                : process_sample_order<7u>(
                    *modulator->design, modulator->channels[channel],
                    input_planar[offset], output_planar[output_offset]);
            if (!ok) {
                stable[channel] = 0u;
                return;
            }
        }
    };
#if defined(CROSSDSD_ENABLE_CHANNEL_THREADS)
    if (modulator->parallel_channels) {
        std::vector<std::thread> workers;
        workers.reserve(
            modulator->info.channel_count > 0
                ? modulator->info.channel_count - 1u
                : 0u);
        for (uint32_t channel = 1; channel < modulator->info.channel_count;
             ++channel) {
            workers.emplace_back(process_channel, channel);
        }
        process_channel(0);
        for (auto& worker : workers) worker.join();
    } else {
        for (uint32_t channel = 0; channel < modulator->info.channel_count;
             ++channel) process_channel(channel);
    }
#else
    for (uint32_t channel = 0; channel < modulator->info.channel_count;
         ++channel) {
        process_channel(channel);
    }
#endif
    if (std::find(stable.begin(), stable.end(), 0u) != stable.end()) {
        modulator->info.stability_failure = 1;
        set_error(error, CROSSDSD_ERROR_OVERFLOW,
                  "modulator stability limit exceeded");
        return CROSSDSD_ERROR_OVERFLOW;
    }
    modulator->info.input_samples_consumed_per_channel +=
        input_count_per_channel;
    modulator->info.output_samples_produced_per_channel +=
        input_count_per_channel;
    set_error(error, CROSSDSD_OK, "");
    return CROSSDSD_OK;
}

crossdsd_result crossdsd_modulator_reset(crossdsd_modulator* modulator) {
    if (!modulator) return CROSSDSD_ERROR_INVALID_ARGUMENT;
    std::fill(
        modulator->channels.begin(), modulator->channels.end(), ChannelState{});
    modulator->info.input_samples_consumed_per_channel = 0;
    modulator->info.output_samples_produced_per_channel = 0;
    modulator->info.stability_failure = 0;
    return CROSSDSD_OK;
}

}  // extern "C"
