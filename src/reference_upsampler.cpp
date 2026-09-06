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
#include <complex>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <new>
#include <utility>
#include <vector>

#if defined(CROSSDSD_ENABLE_CHANNEL_THREADS)
#include <thread>
#endif

#if defined(CROSSDSD_USE_ACCELERATE)
#include <Accelerate/Accelerate.h>
#endif

namespace {

constexpr double kPassEdge = 25'000.0;
constexpr double kStopEdge = 40'000.0;
constexpr double kAttenuationDb = 110.0;
constexpr double kPi = 3.141592653589793238462643383279502884;

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
    double sample_rate, double pass_edge, double stop_edge,
    double attenuation_db, double dc_gain = 1.0) {
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
            bessel_i0(beta * std::sqrt(std::max(0.0, 1.0 - ratio * ratio))) /
            denominator;
        coefficients[static_cast<size_t>(index)] = ideal * window;
        sum += ideal * window;
    }
    for (double& coefficient : coefficients) {
        coefficient *= dc_gain / sum;
    }
    return coefficients;
}

void normalize_interpolator_polyphases(std::vector<double>& coefficients) {
    double sums[2]{};
    for (size_t index = 0; index < coefficients.size(); ++index) {
        sums[index & 1u] += coefficients[index];
    }
    for (size_t index = 0; index < coefficients.size(); ++index) {
        coefficients[index] /= sums[index & 1u];
    }
}

class FirFilter {
public:
    FirFilter() = default;
    explicit FirFilter(std::vector<double> coefficients)
        : coefficients_(std::move(coefficients)),
          history_(coefficients_.size() * 2u, 0.0),
          history_size_(coefficients_.size()) {}

    double push(double input) {
        if (head_ == 0) head_ = history_size_;
        --head_;
        history_[head_] = input;
        history_[head_ + history_size_] = input;
        double output = 0.0;
        const double* samples = history_.data() + head_;
#if defined(CROSSDSD_USE_ACCELERATE)
        vDSP_dotprD(coefficients_.data(), 1, samples, 1, &output,
                    static_cast<vDSP_Length>(history_size_));
#else
        for (size_t tap = 0; tap < history_size_; ++tap) {
            output += coefficients_[tap] * samples[tap];
        }
#endif
        return output;
    }

    void reset() {
        std::fill(history_.begin(), history_.end(), 0.0);
        head_ = 0;
    }

private:
    std::vector<double> coefficients_;
    std::vector<double> history_;
    size_t history_size_ = 0;
    size_t head_ = 0;
};

struct ChannelPipeline {
    ChannelPipeline(
        const std::vector<double>& conditioning,
        const std::vector<std::vector<double>>& interpolation)
        : conditioning(conditioning) {
        stages.reserve(interpolation.size());
        for (const auto& coefficients : interpolation) {
            stages.emplace_back(coefficients);
        }
    }

    void push(double input, double* output) {
        double stage0[2]{conditioning.push(input), 0.0};
        size_t count = 1;
        std::array<double, 64> first{};
        std::array<double, 64> second{};
        double* source = stage0;
        double* destination = first.data();
        for (size_t stage = 0; stage < stages.size(); ++stage) {
            for (size_t index = 0; index < count; ++index) {
                destination[index * 2u] = stages[stage].push(source[index]);
                destination[index * 2u + 1u] = stages[stage].push(0.0);
            }
            count *= 2u;
            source = destination;
            destination =
                destination == first.data() ? second.data() : first.data();
        }
        std::copy_n(source, count, output);
    }

    void reset() {
        conditioning.reset();
        for (auto& stage : stages) stage.reset();
    }

    FirFilter conditioning;
    std::vector<FirFilter> stages;
};

std::complex<double> response(
    const std::vector<double>& coefficients, double sample_rate,
    double frequency_hz) {
    std::complex<double> result(0.0, 0.0);
    const double omega = 2.0 * kPi * frequency_hz / sample_rate;
    for (size_t tap = 0; tap < coefficients.size(); ++tap) {
        result += coefficients[tap] *
                  std::exp(std::complex<double>(
                      0.0, -omega * static_cast<double>(tap)));
    }
    return result;
}

}  // namespace

struct crossdsd_upsampler {
    crossdsd_upsampler_info info{};
    std::vector<double> conditioning;
    std::vector<std::vector<double>> interpolation;
    std::vector<ChannelPipeline> channels;
    bool parallel_channels = true;
};

namespace {

template <typename Sample>
crossdsd_result process_samples(
    crossdsd_upsampler* upsampler, const Sample* input_planar,
    uint64_t input_count_per_channel, uint64_t input_stride,
    double* output_planar, uint64_t output_capacity_per_channel,
    uint64_t output_stride, uint64_t* output_count_per_channel,
    crossdsd_error* error) {
    if (!upsampler || !output_count_per_channel ||
        (input_count_per_channel != 0 && (!input_planar || !output_planar)) ||
        input_stride < input_count_per_channel ||
        input_count_per_channel >
            std::numeric_limits<uint64_t>::max() /
                upsampler->info.interpolation_factor) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                  "invalid upsampler process arguments");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    const uint64_t factor = upsampler->info.interpolation_factor;
    const uint64_t required = input_count_per_channel * factor;
    if (output_capacity_per_channel < required || output_stride < required) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                  "upsampler output buffer is too small");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    if (input_count_per_channel == 0) {
        *output_count_per_channel = 0;
        set_error(error, CROSSDSD_OK, "");
        return CROSSDSD_OK;
    }
    for (uint32_t channel = 0; channel < upsampler->info.channel_count;
         ++channel) {
        const Sample* input =
            input_planar + static_cast<uint64_t>(channel) * input_stride;
        for (uint64_t sample = 0; sample < input_count_per_channel; ++sample) {
            if (!std::isfinite(input[sample])) {
                set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                          "DXD input contains a non-finite sample");
                return CROSSDSD_ERROR_INVALID_ARGUMENT;
            }
        }
    }
    const auto process_channel = [&](uint32_t channel) {
        const Sample* input =
            input_planar + static_cast<uint64_t>(channel) * input_stride;
        double* output =
            output_planar + static_cast<uint64_t>(channel) * output_stride;
        for (uint64_t sample = 0; sample < input_count_per_channel; ++sample) {
            upsampler->channels[channel].push(
                static_cast<double>(input[sample]), output + sample * factor);
        }
    };
#if defined(CROSSDSD_ENABLE_CHANNEL_THREADS)
    if (upsampler->parallel_channels) {
        std::vector<std::thread> workers;
        workers.reserve(
            upsampler->info.channel_count > 0
                ? upsampler->info.channel_count - 1u
                : 0u);
        for (uint32_t channel = 1; channel < upsampler->info.channel_count;
             ++channel) {
            workers.emplace_back(process_channel, channel);
        }
        process_channel(0);
        for (auto& worker : workers) worker.join();
    } else {
        for (uint32_t channel = 0; channel < upsampler->info.channel_count;
             ++channel) process_channel(channel);
    }
#else
    for (uint32_t channel = 0; channel < upsampler->info.channel_count;
         ++channel) {
        process_channel(channel);
    }
#endif
    upsampler->info.input_samples_consumed_per_channel +=
        input_count_per_channel;
    upsampler->info.output_samples_produced_per_channel += required;
    *output_count_per_channel = required;
    set_error(error, CROSSDSD_OK, "");
    return CROSSDSD_OK;
}

}  // namespace

extern "C" {

crossdsd_result crossdsd_upsampler_create(
    const crossdsd_upsampler_config* config,
    crossdsd_upsampler** out_upsampler, crossdsd_error* error) {
    if (!config || config->struct_size < sizeof(crossdsd_upsampler_config) ||
        !out_upsampler) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                  "invalid upsampler configuration");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    *out_upsampler = nullptr;
    const bool supported_input =
        config->input_sample_rate_hz == 352'800u ||
        config->input_sample_rate_hz == 705'600u;
    const uint32_t factor = supported_input
        ? config->output_sample_rate_hz / config->input_sample_rate_hz
        : 0u;
    const bool exact_power_of_two =
        factor != 0u && (factor & (factor - 1u)) == 0u &&
        static_cast<uint64_t>(config->input_sample_rate_hz) * factor ==
            config->output_sample_rate_hz;
    if (!supported_input || !exact_power_of_two ||
        (config->output_sample_rate_hz != 2'822'400u &&
         config->output_sample_rate_hz != 5'644'800u &&
         config->output_sample_rate_hz != 11'289'600u &&
         config->output_sample_rate_hz != 22'579'200u)) {
        set_error(error, CROSSDSD_ERROR_UNSUPPORTED_RATE,
                  "upsampler requires 352.8 or 705.6 kHz PCM input and a matching DSD output");
        return CROSSDSD_ERROR_UNSUPPORTED_RATE;
    }
    if (config->channel_count == 0 || config->channel_count > 256) {
        set_error(error, CROSSDSD_ERROR_UNSUPPORTED_CHANNELS,
                  "channel count must be between 1 and 256");
        return CROSSDSD_ERROR_UNSUPPORTED_CHANNELS;
    }
    try {
        auto upsampler = std::make_unique<crossdsd_upsampler>();
        upsampler->parallel_channels =
            std::getenv("CROSSDSD_DISABLE_UPSAMPLER_CHANNEL_THREADS") == nullptr;
        upsampler->conditioning = design_lowpass(
            config->input_sample_rate_hz, kPassEdge, kStopEdge,
            kAttenuationDb);
        uint32_t stage_input_rate = config->input_sample_rate_hz;
        uint32_t stage_count = 0;
        for (uint32_t value = factor; value > 1u; value /= 2u) ++stage_count;
        for (uint32_t stage = 0; stage < stage_count; ++stage) {
            const uint32_t stage_output_rate = stage_input_rate * 2u;
            auto coefficients = design_lowpass(
                stage_output_rate, kStopEdge,
                static_cast<double>(stage_input_rate) - kStopEdge,
                kAttenuationDb, 2.0);
            normalize_interpolator_polyphases(coefficients);
            upsampler->interpolation.push_back(std::move(coefficients));
            stage_input_rate = stage_output_rate;
        }
        upsampler->channels.reserve(config->channel_count);
        for (uint32_t channel = 0; channel < config->channel_count; ++channel) {
            upsampler->channels.emplace_back(
                upsampler->conditioning, upsampler->interpolation);
        }

        auto& info = upsampler->info;
        info.struct_size = sizeof(info);
        info.api_version = CROSSDSD_DSP_API_VERSION;
        info.input_sample_rate_hz = config->input_sample_rate_hz;
        info.output_sample_rate_hz = config->output_sample_rate_hz;
        info.channel_count = config->channel_count;
        info.interpolation_factor = factor;
        info.interpolation_stage_count = stage_count;
        info.conditioning_filter_taps =
            static_cast<uint32_t>(upsampler->conditioning.size());
        double latency = 0.5 * (upsampler->conditioning.size() - 1u) * factor;
        uint32_t remaining_factor = factor / 2u;
        for (size_t stage = 0; stage < upsampler->interpolation.size(); ++stage) {
            info.interpolation_filter_taps[stage] = static_cast<uint32_t>(
                upsampler->interpolation[stage].size());
            latency += 0.5 * (upsampler->interpolation[stage].size() - 1u) *
                       remaining_factor;
            remaining_factor /= 2u;
        }
        info.passband_edge_hz = kPassEdge;
        info.stopband_edge_hz = kStopEdge;
        info.stopband_attenuation_db = kAttenuationDb;
        info.latency_output_samples = latency;
        *out_upsampler = upsampler.release();
    } catch (const std::bad_alloc&) {
        set_error(error, CROSSDSD_ERROR_OUT_OF_MEMORY, "out of memory");
        return CROSSDSD_ERROR_OUT_OF_MEMORY;
    }
    set_error(error, CROSSDSD_OK, "");
    return CROSSDSD_OK;
}

void crossdsd_upsampler_destroy(crossdsd_upsampler* upsampler) {
    delete upsampler;
}

crossdsd_result crossdsd_upsampler_get_info(
    const crossdsd_upsampler* upsampler, crossdsd_upsampler_info* info) {
    if (!upsampler || !info ||
        info->struct_size < sizeof(crossdsd_upsampler_info)) {
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    *info = upsampler->info;
    return CROSSDSD_OK;
}

crossdsd_result crossdsd_upsampler_frequency_response(
    const crossdsd_upsampler* upsampler, double frequency_hz,
    double* magnitude, double* phase_radians) {
    if (!upsampler || !magnitude || !phase_radians ||
        !std::isfinite(frequency_hz) || frequency_hz < 0.0 ||
        frequency_hz > upsampler->info.input_sample_rate_hz / 2.0) {
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    auto cascade =
        response(upsampler->conditioning,
                 upsampler->info.input_sample_rate_hz, frequency_hz);
    double rate = upsampler->info.input_sample_rate_hz * 2.0;
    for (const auto& coefficients : upsampler->interpolation) {
        cascade *= response(coefficients, rate, frequency_hz) / 2.0;
        rate *= 2.0;
    }
    *magnitude = std::abs(cascade);
    *phase_radians = std::arg(cascade);
    return CROSSDSD_OK;
}

crossdsd_result crossdsd_upsampler_process(
    crossdsd_upsampler* upsampler, const float* input_planar,
    uint64_t input_count_per_channel, uint64_t input_stride,
    double* output_planar, uint64_t output_capacity_per_channel,
    uint64_t output_stride, uint64_t* output_count_per_channel,
    crossdsd_error* error) {
    return process_samples(
        upsampler, input_planar, input_count_per_channel, input_stride,
        output_planar, output_capacity_per_channel, output_stride,
        output_count_per_channel, error);
}

crossdsd_result crossdsd_upsampler_process_f64(
    crossdsd_upsampler* upsampler, const double* input_planar,
    uint64_t input_count_per_channel, uint64_t input_stride,
    double* output_planar, uint64_t output_capacity_per_channel,
    uint64_t output_stride, uint64_t* output_count_per_channel,
    crossdsd_error* error) {
    return process_samples(
        upsampler, input_planar, input_count_per_channel, input_stride,
        output_planar, output_capacity_per_channel, output_stride,
        output_count_per_channel, error);
}

crossdsd_result crossdsd_upsampler_reset(crossdsd_upsampler* upsampler) {
    if (!upsampler) return CROSSDSD_ERROR_INVALID_ARGUMENT;
    for (auto& channel : upsampler->channels) channel.reset();
    upsampler->info.input_samples_consumed_per_channel = 0;
    upsampler->info.output_samples_produced_per_channel = 0;
    return CROSSDSD_OK;
}

}  // extern "C"
