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
#include <cmath>
#include <complex>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <utility>
#include <vector>

#if defined(CROSSDSD_ENABLE_CHANNEL_THREADS)
#include <thread>
#endif

#if defined(CROSSDSD_USE_ACCELERATE)
#include <Accelerate/Accelerate.h>
#endif

namespace {

constexpr uint32_t kOutputRate = 352'800;
constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kAttenuationDb = 110.0;
constexpr double kRawPassEdge = 150'000.0;
constexpr double kRawStopEdge = 176'400.0;

void set_error(crossdsd_error* error, crossdsd_result code, const char* message) {
    if (!error || error->struct_size < sizeof(crossdsd_error)) return;
    error->code = code;
    error->byte_offset = 0;
    std::snprintf(error->message, sizeof(error->message), "%s", message);
}

double bessel_i0(double value) {
    double sum = 1.0;
    double term = 1.0;
    const double quarter = value * value / 4.0;
    for (unsigned k = 1; k < 80; ++k) {
        term *= quarter / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term <= sum * 1e-17) break;
    }
    return sum;
}

double kaiser_beta(double attenuation_db) {
    if (attenuation_db > 50.0) return 0.1102 * (attenuation_db - 8.7);
    if (attenuation_db >= 21.0) {
        const double value = attenuation_db - 21.0;
        return 0.5842 * std::pow(value, 0.4) + 0.07886 * value;
    }
    return 0.0;
}

std::vector<double> design_lowpass(
    double sample_rate, double pass_edge, double stop_edge, double attenuation_db) {
    const double transition = stop_edge - pass_edge;
    const double delta_omega = 2.0 * kPi * transition / sample_rate;
    uint64_t order = static_cast<uint64_t>(
        std::ceil((attenuation_db - 8.0) / (2.285 * delta_omega)));
    if (order % 2u != 0) ++order;
    const uint64_t taps = order + 1u;
    const double middle = static_cast<double>(order) / 2.0;
    const double cutoff = (pass_edge + stop_edge) / 2.0;
    const double normalized = cutoff / sample_rate;
    const double beta = kaiser_beta(attenuation_db);
    const double denominator = bessel_i0(beta);
    std::vector<double> coefficients(static_cast<size_t>(taps));
    double sum = 0.0;
    for (uint64_t index = 0; index < taps; ++index) {
        const double offset = static_cast<double>(index) - middle;
        const double ideal = offset == 0.0
                                 ? 2.0 * normalized
                                 : std::sin(2.0 * kPi * normalized * offset) /
                                       (kPi * offset);
        const double ratio = offset / middle;
        const double window =
            bessel_i0(beta * std::sqrt(std::max(0.0, 1.0 - ratio * ratio))) / denominator;
        coefficients[static_cast<size_t>(index)] = ideal * window;
        sum += ideal * window;
    }
    for (double& coefficient : coefficients) coefficient /= sum;
    return coefficients;
}

class FirDecimator {
public:
    explicit FirDecimator(std::vector<double> coefficients)
        : coefficients_(std::move(coefficients)),
          history_(coefficients_.size() * 2u, 0.0),
          history_size_(coefficients_.size()) {}

    std::optional<double> push(double input) {
        if (head_ == 0) head_ = history_size_;
        --head_;
        history_[head_] = input;
        history_[head_ + history_size_] = input;
        ++phase_;
        if (phase_ != 2) return std::nullopt;
        phase_ = 0;
        double output = 0.0;
        const double* samples = history_.data() + head_;
#if defined(CROSSDSD_USE_ACCELERATE)
        vDSP_dotprD(coefficients_.data(), 1, samples, 1, &output,
                    static_cast<vDSP_Length>(history_size_));
#else
#if defined(__clang__)
#pragma clang loop vectorize(disable) interleave(disable)
#endif
        for (size_t tap = 0; tap < history_size_; ++tap) {
            output += coefficients_[tap] * samples[tap];
        }
#endif
        return output;
    }

    uint64_t group_delay_input_samples() const {
        return static_cast<uint64_t>((coefficients_.size() - 1u) / 2u);
    }

private:
    std::vector<double> coefficients_;
    std::vector<double> history_;
    size_t history_size_;
    size_t head_ = 0;
    unsigned phase_ = 0;
};

struct ChannelPipeline {
    explicit ChannelPipeline(const std::vector<std::vector<double>>& coefficients) {
        stages.reserve(coefficients.size());
        for (const auto& stage : coefficients) stages.emplace_back(stage);
    }

    std::optional<double> push(double value) {
        std::optional<double> current = value;
        for (auto& stage : stages) {
            if (!current) return std::nullopt;
            current = stage.push(*current);
        }
        return current;
    }

    std::vector<FirDecimator> stages;
};

bool valid_rate(uint32_t rate) {
    return rate == 2'822'400u || rate == 5'644'800u ||
           rate == 11'289'600u || rate == 22'579'200u;
}

}  // namespace

struct crossdsd_decoder {
    crossdsd_decoder_info info{};
    std::vector<std::vector<double>> coefficients;
    std::vector<ChannelPipeline> channels;
    bool parallel_channels = true;
};

extern "C" {

crossdsd_result crossdsd_decoder_create(
    const crossdsd_decoder_config* config, crossdsd_decoder** out_decoder,
    crossdsd_error* error) {
    if (!config || config->struct_size < sizeof(crossdsd_decoder_config) || !out_decoder) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT, "invalid decoder configuration");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    *out_decoder = nullptr;
    if (!valid_rate(config->input_sample_rate_hz)) {
        set_error(error, CROSSDSD_ERROR_UNSUPPORTED_RATE, "decoder input must be DSD64, DSD128, DSD256 or DSD512");
        return CROSSDSD_ERROR_UNSUPPORTED_RATE;
    }
    if (config->channel_count == 0 || config->channel_count > 256) {
        set_error(error, CROSSDSD_ERROR_UNSUPPORTED_CHANNELS, "channel count must be between 1 and 256");
        return CROSSDSD_ERROR_UNSUPPORTED_CHANNELS;
    }
    if (config->level_mode != CROSSDSD_LEVEL_DSD_RAW &&
        config->level_mode != CROSSDSD_LEVEL_PCM_CONTINUITY) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT, "unknown level mode");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    if (config->filter_profile != CROSSDSD_FILTER_STANDARD &&
        config->filter_profile != CROSSDSD_FILTER_RAW) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT, "unknown filter profile");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }

    try {
        auto decoder = std::make_unique<crossdsd_decoder>();
        decoder->parallel_channels =
            std::getenv("CROSSDSD_DISABLE_DECODER_CHANNEL_THREADS") == nullptr;
        uint32_t rate = config->input_sample_rate_hz;
        std::vector<std::vector<double>> coefficients;
        double latency_seconds = 0.0;
        while (rate > 705'600u) {
            const double stop_edge =
                config->filter_profile == CROSSDSD_FILTER_RAW
                    ? 0.25 * rate
                    : 0.30 * rate;
            auto stage = design_lowpass(
                static_cast<double>(rate), 0.20 * rate, stop_edge, kAttenuationDb);
            latency_seconds +=
                static_cast<double>((stage.size() - 1u) / 2u) / static_cast<double>(rate);
            coefficients.push_back(std::move(stage));
            rate /= 2u;
        }
        auto safe =
            config->filter_profile == CROSSDSD_FILTER_RAW
                ? design_lowpass(
                      705'600.0, kRawPassEdge, kRawStopEdge, kAttenuationDb)
                : design_lowpass(
                      705'600.0, 20'000.0, 40'000.0, kAttenuationDb);
        latency_seconds +=
            static_cast<double>((safe.size() - 1u) / 2u) / 705'600.0;
        coefficients.push_back(std::move(safe));

        decoder->coefficients = coefficients;
        decoder->channels.reserve(config->channel_count);
        for (uint32_t channel = 0; channel < config->channel_count; ++channel) {
            decoder->channels.emplace_back(coefficients);
        }
        auto& info = decoder->info;
        info.struct_size = sizeof(info);
        info.api_version = CROSSDSD_DSP_API_VERSION;
        info.input_sample_rate_hz = config->input_sample_rate_hz;
        info.output_sample_rate_hz = kOutputRate;
        info.channel_count = config->channel_count;
        info.decimation_factor = config->input_sample_rate_hz / kOutputRate;
        info.stage_count = static_cast<uint32_t>(coefficients.size());
        info.filter_profile = config->filter_profile;
        info.applied_gain =
            config->level_mode == CROSSDSD_LEVEL_PCM_CONTINUITY ? 2.0 : 1.0;
        info.latency_output_samples = latency_seconds * kOutputRate;
        *out_decoder = decoder.release();
    } catch (const std::bad_alloc&) {
        set_error(error, CROSSDSD_ERROR_OUT_OF_MEMORY, "out of memory");
        return CROSSDSD_ERROR_OUT_OF_MEMORY;
    }
    set_error(error, CROSSDSD_OK, "");
    return CROSSDSD_OK;
}

void crossdsd_decoder_destroy(crossdsd_decoder* decoder) { delete decoder; }

crossdsd_result crossdsd_decoder_get_info(
    const crossdsd_decoder* decoder, crossdsd_decoder_info* info) {
    if (!decoder || !info || info->struct_size < sizeof(crossdsd_decoder_info)) {
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    *info = decoder->info;
    return CROSSDSD_OK;
}

crossdsd_result crossdsd_decoder_frequency_response(
    const crossdsd_decoder* decoder, double frequency_hz, double* magnitude,
    double* phase_radians) {
    if (!decoder || !magnitude || !phase_radians || !std::isfinite(frequency_hz) ||
        frequency_hz < 0.0 || frequency_hz > kOutputRate / 2.0) {
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    std::complex<double> cascade(1.0, 0.0);
    double rate = decoder->info.input_sample_rate_hz;
    for (const auto& coefficients : decoder->coefficients) {
        std::complex<double> response(0.0, 0.0);
        const double omega = 2.0 * kPi * frequency_hz / rate;
        for (size_t tap = 0; tap < coefficients.size(); ++tap) {
            response += coefficients[tap] *
                        std::exp(std::complex<double>(
                            0.0, -omega * static_cast<double>(tap)));
        }
        cascade *= response;
        rate /= 2.0;
    }
    *magnitude = std::abs(cascade);
    *phase_radians = std::arg(cascade);
    return CROSSDSD_OK;
}

crossdsd_result crossdsd_decoder_process(
    crossdsd_decoder* decoder, const int8_t* input_planar,
    uint64_t input_count_per_channel, uint64_t input_stride,
    double* output_planar, uint64_t output_capacity_per_channel,
    uint64_t output_stride, uint64_t* output_count_per_channel,
    crossdsd_error* error) {
    if (!decoder || !output_count_per_channel ||
        (input_count_per_channel != 0 && (!input_planar || !output_planar)) ||
        input_stride < input_count_per_channel) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT, "invalid process arguments");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    const uint64_t factor = decoder->info.decimation_factor;
    const uint64_t maximum_output = input_count_per_channel / factor + 1u;
    if (input_count_per_channel != 0 &&
        (output_capacity_per_channel < maximum_output || output_stride < maximum_output)) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT, "output buffer is too small");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    std::vector<uint64_t> produced_counts(decoder->info.channel_count, 0);
    std::vector<uint8_t> valid_channels(decoder->info.channel_count, 1u);
    const auto process_channel = [&](uint32_t channel) {
        uint64_t produced = 0;
        for (uint64_t sample = 0; sample < input_count_per_channel; ++sample) {
            const int8_t input = input_planar[static_cast<uint64_t>(channel) * input_stride + sample];
            if (input != -1 && input != 1) {
                valid_channels[channel] = 0u;
                return;
            }
            const auto value = decoder->channels[channel].push(static_cast<double>(input));
            if (value) {
                output_planar[static_cast<uint64_t>(channel) * output_stride + produced] =
                    *value * decoder->info.applied_gain;
                ++produced;
            }
        }
        produced_counts[channel] = produced;
    };
#if defined(CROSSDSD_ENABLE_CHANNEL_THREADS)
    if (decoder->parallel_channels) {
        std::vector<std::thread> workers;
        workers.reserve(decoder->info.channel_count > 0 ? decoder->info.channel_count - 1u : 0u);
        for (uint32_t channel = 1; channel < decoder->info.channel_count; ++channel) {
            workers.emplace_back(process_channel, channel);
        }
        process_channel(0);
        for (auto& worker : workers) worker.join();
    } else {
        for (uint32_t channel = 0; channel < decoder->info.channel_count; ++channel)
            process_channel(channel);
    }
#else
    for (uint32_t channel = 0; channel < decoder->info.channel_count; ++channel) {
        process_channel(channel);
    }
#endif
    for (uint32_t channel = 0; channel < decoder->info.channel_count; ++channel) {
        if (!valid_channels[channel]) {
            set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                      "DSD input values must be -1 or +1");
            return CROSSDSD_ERROR_INVALID_ARGUMENT;
        }
        if (produced_counts[channel] != produced_counts[0]) {
            set_error(error, CROSSDSD_ERROR_INVALID_CHUNK, "channel decoder states diverged");
            return CROSSDSD_ERROR_INVALID_CHUNK;
        }
    }
    const uint64_t common_count = produced_counts[0];
    decoder->info.input_samples_consumed_per_channel += input_count_per_channel;
    decoder->info.output_samples_produced_per_channel += common_count;
    *output_count_per_channel = common_count;
    set_error(error, CROSSDSD_OK, "");
    return CROSSDSD_OK;
}

}  // extern "C"
