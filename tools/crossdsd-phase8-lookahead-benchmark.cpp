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

#include "crossdsd/audio.h"
#include "crossdsd/dsp.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <numbers>
#include <span>
#include <string_view>
#include <vector>

namespace {

constexpr uint32_t kDxdRate = 352'800;
#if defined(CROSSDSD_PHASE8_DSD128)
constexpr uint32_t kDsdRate = 5'644'800;
constexpr uint32_t kFactor = 16;
constexpr uint32_t kOrder = 5;
constexpr const char* kRateName = "DSD128";
constexpr const char* kProfileName =
    "phase8-studio-dsd128-selective-envelope-1-width12-8-4-1";
constexpr double kWholeProgramSlopeThreshold = 0.001;
constexpr double kWholeProgramPeakThreshold = 0.0;
#elif defined(CROSSDSD_PHASE8_DSD256)
constexpr uint32_t kDsdRate = 11'289'600;
constexpr uint32_t kFactor = 32;
constexpr uint32_t kOrder = 7;
constexpr const char* kRateName = "DSD256";
constexpr const char* kProfileName =
    "phase8-studio-dsd256-selective-envelope-1-width6";
constexpr double kWholeProgramSlopeThreshold = 0.0005;
constexpr double kWholeProgramPeakThreshold = 0.25;
#else
constexpr uint32_t kDsdRate = 2'822'400;
constexpr uint32_t kFactor = 8;
constexpr uint32_t kOrder = 7;
constexpr const char* kRateName = "DSD64";
constexpr const char* kProfileName =
    "phase8-studio-dsd64-envelope-16-width16-8-4-1";
constexpr double kWholeProgramSlopeThreshold = 0.0;
constexpr double kWholeProgramPeakThreshold = 0.0;
#endif
constexpr uint32_t kMaximumDepth = 16;
constexpr uint32_t kEdgeExtension = 93;
constexpr double kPcmToModulator = 0.5;
constexpr double kAbortState = 8.0;
constexpr double kAbortQuantizerInput = 8.0;
constexpr double kAudibleEdge = 25'000.0;
const double kPerceptualPole =
    std::exp(-2.0 * std::numbers::pi * kAudibleEdge / kDsdRate);
const double kEnvelopeRelease =
    std::exp(-1.0 / (0.002 * static_cast<double>(kDsdRate)));

#if defined(CROSSDSD_PHASE8_DSD128)
#include "../src/reference_modulator_dsd128_coefficients.inc"
constexpr auto& kMatrix = kDsd128Order5H13;
#elif defined(CROSSDSD_PHASE8_DSD256)
#include "../src/reference_modulator_dsd256_coefficients.inc"
constexpr auto& kMatrix = kDsd256Order7H12;
#else
#include "../src/reference_modulator_coefficients.inc"
constexpr auto& kMatrix = kOrder7H132;
#endif

double matrix_at(uint32_t row, uint32_t column) {
    return kMatrix[
        static_cast<size_t>(row) * (kOrder + 2u) + column];
}

struct SearchConfig {
    const char* name;
    uint32_t depth;
    uint32_t width;
    double quantizer_weight;
    double state_weight;
    double audible_weight;
    double switching_weight;
    double adaptive_disagreement_weight;
    double hybrid_pcm_threshold;
    uint32_t dynamic_width_profile = 0;
    double hybrid_slope_threshold = 0.0;
};

struct SearchState {
    std::array<double, kOrder> modulator{};
    std::array<double, 2> audible_error{};
    double level_envelope = 0.0;
    double slope_envelope = 0.0;
    double previous_input = 0.0;
    int8_t previous_bit = 0;
};

struct Transition {
    SearchState state;
    double quantizer_input = 0.0;
    double maximum_state = 0.0;
    double cost = 0.0;
    bool stable = true;
};

Transition transition(
    const SearchConfig& config, const SearchState& current, double pcm_input,
    int8_t bit) {
    Transition result;
    const double input = kPcmToModulator * pcm_input;
    double quantizer_input = matrix_at(kOrder, kOrder) * input;
    for (uint32_t column = 0; column < kOrder; ++column) {
        quantizer_input +=
            matrix_at(kOrder, column) * current.modulator[column];
    }
    result.quantizer_input = quantizer_input;

    double state_energy = 0.0;
    for (uint32_t row = 0; row < kOrder; ++row) {
        double value =
            matrix_at(row, kOrder) * input +
            matrix_at(row, kOrder + 1u) * bit;
        for (uint32_t column = 0; column < kOrder; ++column) {
            value += matrix_at(row, column) * current.modulator[column];
        }
        result.state.modulator[row] = value;
        result.maximum_state =
            std::max(result.maximum_state, std::abs(value));
        if (config.state_weight != 0.0) state_energy += value * value;
        if (!std::isfinite(value) || std::abs(value) > kAbortState) {
            result.stable = false;
        }
    }
    if (!std::isfinite(quantizer_input) ||
        std::abs(quantizer_input) > kAbortQuantizerInput) {
        result.stable = false;
    }

    if (config.audible_weight != 0.0) {
        const double error = static_cast<double>(bit) - input;
        result.state.audible_error[0] =
            kPerceptualPole * current.audible_error[0] +
            (1.0 - kPerceptualPole) * error;
        result.state.audible_error[1] =
            kPerceptualPole * current.audible_error[1] +
            (1.0 - kPerceptualPole) * result.state.audible_error[0];
    }
    if (config.adaptive_disagreement_weight != 0.0 ||
        config.dynamic_width_profile != 0u ||
        config.hybrid_slope_threshold != 0.0) {
        result.state.level_envelope =
            std::max(std::abs(pcm_input),
                     kEnvelopeRelease * current.level_envelope);
        result.state.slope_envelope =
            std::max(
                std::abs(pcm_input - current.previous_input),
                kEnvelopeRelease * current.slope_envelope);
    }
    result.state.previous_input = pcm_input;
    result.state.previous_bit = bit;

    const double quantizer_error =
        static_cast<double>(bit) - quantizer_input;
    const double switched =
        config.switching_weight != 0.0 &&
                current.previous_bit != 0 && current.previous_bit != bit
            ? 1.0
            : 0.0;
    const bool disagrees_with_greedy =
        bit != (quantizer_input >= 0.0 ? int8_t{1} : int8_t{-1});
    double adaptive_penalty = 0.0;
    if (config.adaptive_disagreement_weight != 0.0) {
        const double normalized_level =
            std::min(result.state.level_envelope / 1.428893958511103, 1.0);
        adaptive_penalty =
            config.adaptive_disagreement_weight *
            normalized_level * normalized_level * normalized_level *
            normalized_level * disagrees_with_greedy;
    }
    result.cost =
        config.quantizer_weight * quantizer_error * quantizer_error +
        config.state_weight * state_energy +
        config.audible_weight * result.state.audible_error[1] *
            result.state.audible_error[1] +
        config.switching_weight * switched + adaptive_penalty;
    return result;
}

struct Path {
    SearchState endpoint;
    uint16_t positive_bits = 0;
    uint32_t length = 0;
    double cost = 0.0;
};

struct Expansion {
    SearchState endpoint;
    double cost = 0.0;
    uint8_t parent = 0;
    int8_t bit = 0;
};

struct Simulation {
    std::vector<int8_t> output;
    double maximum_state = 0.0;
    double maximum_quantizer_input = 0.0;
    uint64_t maximum_run = 0;
    double one_density = 0.0;
    double seconds = 0.0;
    bool stable = true;
};

void update_metrics(
    Simulation& result, int8_t bit, const Transition& committed,
    int8_t& previous, uint64_t& run, uint64_t& ones) {
    result.maximum_state =
        std::max(result.maximum_state, committed.maximum_state);
    result.maximum_quantizer_input =
        std::max(
            result.maximum_quantizer_input,
            std::abs(committed.quantizer_input));
    run = bit == previous ? run + 1u : 1u;
    previous = bit;
    result.maximum_run = std::max(result.maximum_run, run);
    ones += bit > 0;
}

template <typename Input>
Simulation simulate_conventional(uint64_t count, const Input& input) {
    const SearchConfig conventional{
        "conventional", 1, 1, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    Simulation result;
    result.output.reserve(static_cast<size_t>(count));
    SearchState state;
    int8_t previous = 0;
    uint64_t run = 0;
    uint64_t ones = 0;
    const auto start = std::chrono::steady_clock::now();
    for (uint64_t index = 0; index < count; ++index) {
        const double pcm = input(index);
        double quantizer_input = matrix_at(kOrder, kOrder) *
                                 (kPcmToModulator * pcm);
        for (uint32_t column = 0; column < kOrder; ++column) {
            quantizer_input +=
                matrix_at(kOrder, column) * state.modulator[column];
        }
        const int8_t bit = quantizer_input >= 0.0 ? 1 : -1;
        const auto committed = transition(conventional, state, pcm, bit);
        if (!committed.stable) {
            result.stable = false;
            break;
        }
        state = committed.state;
        result.output.push_back(bit);
        update_metrics(result, bit, committed, previous, run, ones);
    }
    result.seconds =
        std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
    result.one_density =
        static_cast<double>(ones) /
        static_cast<double>(std::max<size_t>(result.output.size(), 1u));
    return result;
}

template <typename Input>
Simulation simulate_beam(
    const SearchConfig& config, uint64_t count, const Input& input) {
    Simulation result;
    result.output.reserve(static_cast<size_t>(count));
    constexpr size_t kMaximumWidth = 32;
    std::array<Path, kMaximumWidth> beam{};
    std::array<Path, kMaximumWidth> next_beam;
    std::array<Expansion, 2u * kMaximumWidth> expanded;
    size_t beam_count = 1;
    SearchState committed_state;
    int8_t previous = 0;
    uint64_t run = 0;
    uint64_t ones = 0;
    const auto start = std::chrono::steady_clock::now();

    for (uint64_t index = 0; index < count; ++index) {
        size_t expanded_count = 0;
        for (size_t path_index = 0; path_index < beam_count; ++path_index) {
            const auto& path = beam[path_index];
            for (const int8_t bit : {int8_t{-1}, int8_t{1}}) {
                if (config.hybrid_pcm_threshold > 0.0 &&
                    std::abs(input(index)) >= config.hybrid_pcm_threshold) {
                    double quantizer_input =
                        matrix_at(kOrder, kOrder) *
                        (kPcmToModulator * input(index));
                    for (uint32_t column = 0; column < kOrder; ++column) {
                        quantizer_input +=
                            matrix_at(kOrder, column) *
                            path.endpoint.modulator[column];
                    }
                    const int8_t greedy =
                        quantizer_input >= 0.0 ? int8_t{1} : int8_t{-1};
                    if (bit != greedy) continue;
                }
                if (config.hybrid_slope_threshold > 0.0 &&
                    path.endpoint.slope_envelope >=
                        config.hybrid_slope_threshold) {
                    double quantizer_input =
                        matrix_at(kOrder, kOrder) *
                        (kPcmToModulator * input(index));
                    for (uint32_t column = 0; column < kOrder; ++column) {
                        quantizer_input +=
                            matrix_at(kOrder, column) *
                            path.endpoint.modulator[column];
                    }
                    const int8_t greedy =
                        quantizer_input >= 0.0 ? int8_t{1} : int8_t{-1};
                    if (bit != greedy) continue;
                }
                const auto step = transition(config, path.endpoint, input(index), bit);
                if (!step.stable) continue;
                auto& next = expanded[expanded_count++];
                next.endpoint = step.state;
                next.cost = path.cost + step.cost;
                next.parent = static_cast<uint8_t>(path_index);
                next.bit = bit;
            }
        }
        if (expanded_count == 0u) {
            result.stable = false;
            break;
        }
        uint32_t retained_width = config.width;
        if (config.dynamic_width_profile != 0u) {
            const double level = std::min(
                expanded[0].endpoint.level_envelope /
                    1.428893958511103,
                1.0);
            if (config.dynamic_width_profile == 1u) {
                retained_width =
                    level >= 0.85 ? 1u
                    : level >= 0.60 ? 4u
                    : level >= 0.35 ? 8u
                                    : config.width;
            } else if (config.dynamic_width_profile == 2u) {
                retained_width =
                    level >= 0.70 ? 1u
                    : level >= 0.40 ? 4u
                    : level >= 0.20 ? 8u
                                    : config.width;
            } else {
                retained_width =
                    level >= 0.85 ? 1u
                    : level >= 0.60 ? 4u
                    : level >= 0.02 ? 8u
                                    : config.width;
            }
        }
        beam_count = std::min<size_t>(retained_width, expanded_count);
        std::partial_sort(
            expanded.begin(), expanded.begin() + beam_count,
            expanded.begin() + expanded_count,
            [](const Expansion& left, const Expansion& right) {
                return left.cost < right.cost;
            });
        for (size_t retained = 0; retained < beam_count; ++retained) {
            const auto& candidate = expanded[retained];
            Path& next = next_beam[retained];
            next = beam[candidate.parent];
            next.endpoint = candidate.endpoint;
            if (candidate.bit > 0) {
                next.positive_bits |=
                    static_cast<uint16_t>(1u << next.length);
            } else {
                next.positive_bits &=
                    static_cast<uint16_t>(~(1u << next.length));
            }
            ++next.length;
            next.cost = candidate.cost;
        }
        beam = std::move(next_beam);

        if (beam[0].length < config.depth) continue;
        const int8_t selected =
            (beam[0].positive_bits & 1u) != 0u ? int8_t{1} : int8_t{-1};
        const double pcm = input(index + 1u - config.depth);
        const auto committed =
            transition(config, committed_state, pcm, selected);
        if (!committed.stable) {
            result.stable = false;
            break;
        }
        committed_state = committed.state;
        result.output.push_back(selected);
        update_metrics(result, selected, committed, previous, run, ones);

        size_t rebased_count = 0;
        for (size_t path_index = 0; path_index < beam_count; ++path_index) {
            auto& path = beam[path_index];
            const int8_t first =
                (path.positive_bits & 1u) != 0u ? int8_t{1} : int8_t{-1};
            if (first != selected) continue;
            path.cost -= committed.cost;
            --path.length;
            path.positive_bits >>= 1u;
            if (rebased_count != path_index) {
                beam[rebased_count] = std::move(path);
            }
            ++rebased_count;
        }
        beam_count = rebased_count;
    }

    if (result.stable && result.output.size() < count) {
        if (beam_count == 0u) {
            result.stable = false;
        } else {
            const Path& best = beam[0];
            for (uint32_t offset = 0;
                offset < best.length && result.output.size() < count;
                 ++offset) {
                const uint64_t input_index = result.output.size();
                const int8_t bit =
                    (best.positive_bits & (1u << offset)) != 0u
                        ? int8_t{1}
                        : int8_t{-1};
                const auto committed =
                    transition(config, committed_state, input(input_index), bit);
                if (!committed.stable) {
                    result.stable = false;
                    break;
                }
                committed_state = committed.state;
                result.output.push_back(bit);
                update_metrics(
                    result, bit, committed, previous, run, ones);
            }
        }
    }
    result.seconds =
        std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
    result.one_density =
        static_cast<double>(ones) /
        static_cast<double>(std::max<size_t>(result.output.size(), 1u));
    return result;
}

std::vector<double> decode(const std::vector<int8_t>& dsd) {
    crossdsd_decoder_config config{};
    config.struct_size = sizeof(config);
    config.input_sample_rate_hz = kDsdRate;
    config.channel_count = 1;
    config.level_mode = CROSSDSD_LEVEL_PCM_CONTINUITY;
    config.filter_profile = CROSSDSD_FILTER_STANDARD;
    crossdsd_decoder* decoder = nullptr;
    if (crossdsd_decoder_create(&config, &decoder, nullptr) != CROSSDSD_OK) {
        return {};
    }
    std::vector<double> output(dsd.size() / kFactor + 1u);
    uint64_t produced = 0;
    const auto code = crossdsd_decoder_process(
        decoder, dsd.data(), dsd.size(), dsd.size(), output.data(),
        output.size(), output.size(), &produced, nullptr);
    crossdsd_decoder_destroy(decoder);
    if (code != CROSSDSD_OK) return {};
    output.resize(static_cast<size_t>(produced));
    return output;
}

struct AudioMetrics {
    double gain_db = std::numeric_limits<double>::quiet_NaN();
    double thdn_db = std::numeric_limits<double>::quiet_NaN();
    double noise_dbfs = std::numeric_limits<double>::quiet_NaN();
};

AudioMetrics measure(
    const std::vector<double>& decoded, double frequency,
    double expected_amplitude) {
    AudioMetrics metrics;
    if (expected_amplitude < 0.0) return metrics;
    const size_t trim = static_cast<size_t>(0.025 * kDxdRate);
    if (decoded.size() <= 2u * trim) return metrics;
    const size_t begin = trim;
    const size_t end = decoded.size() - trim;
    double mean = 0.0;
    for (size_t i = begin; i < end; ++i) mean += decoded[i];
    mean /= static_cast<double>(end - begin);
    if (frequency <= 0.0) {
        double power = 0.0;
        for (size_t i = begin; i < end; ++i) {
            const double centered = decoded[i] - mean;
            power += centered * centered;
        }
        metrics.noise_dbfs = 10.0 * std::log10(
            std::max(power / static_cast<double>(end - begin), 1e-300));
        return metrics;
    }
    const double omega =
        2.0 * std::numbers::pi * frequency / kDxdRate;
    double sine = 0.0;
    double cosine = 0.0;
    for (size_t i = begin; i < end; ++i) {
        const double value = decoded[i] - mean;
        sine += value * std::sin(omega * static_cast<double>(i));
        cosine += value * std::cos(omega * static_cast<double>(i));
    }
    const double scale = 2.0 / static_cast<double>(end - begin);
    sine *= scale;
    cosine *= scale;
    const double recovered = std::hypot(sine, cosine);
    metrics.gain_db =
        20.0 * std::log10(recovered / expected_amplitude);
    double residual_power = 0.0;
    for (size_t i = begin; i < end; ++i) {
        const double fitted =
            mean + sine * std::sin(omega * static_cast<double>(i)) +
            cosine * std::cos(omega * static_cast<double>(i));
        const double residual = decoded[i] - fitted;
        residual_power += residual * residual;
    }
    residual_power /= static_cast<double>(end - begin);
    metrics.thdn_db = 10.0 * std::log10(
        residual_power / std::max(0.5 * recovered * recovered, 1e-300));
    return metrics;
}

void print(
    const SearchConfig& config, std::string_view signal,
    const Simulation& simulation, const AudioMetrics& metrics) {
    const double realtime =
        simulation.seconds > 0.0
            ? (static_cast<double>(simulation.output.size()) / kDsdRate) /
                  simulation.seconds
            : 0.0;
    std::cout << config.name << ',' << config.depth << ',' << config.width
              << ',' << signal << ',' << (simulation.stable ? "true" : "false")
              << ',' << simulation.maximum_state << ','
              << simulation.maximum_quantizer_input << ','
              << simulation.maximum_run << ',' << simulation.one_density << ','
              << realtime << ',';
    if (std::isfinite(metrics.gain_db)) std::cout << metrics.gain_db;
    std::cout << ',';
    if (std::isfinite(metrics.thdn_db)) std::cout << metrics.thdn_db;
    std::cout << ',';
    if (std::isfinite(metrics.noise_dbfs)) std::cout << metrics.noise_dbfs;
    std::cout << '\n';
}

std::vector<float> make_extension(
    const float* samples, size_t count, bool left) {
    std::vector<float> result(kEdgeExtension);
    for (size_t index = 0; index < result.size(); ++index) {
        const size_t distance = index % count;
        result[index] =
            left ? samples[(kEdgeExtension - 1u - index) % count]
                 : samples[count - 1u - distance];
    }
    return result;
}

int encode_adaptive(
    const char* input_path, const char* output_path,
    const char* report_path) {
    crossdsd_error error{};
    error.struct_size = sizeof(error);
    crossdsd_wave_reader* reader = nullptr;
    auto code =
        crossdsd_wave_reader_open_file(input_path, &reader, &error);
    if (code != CROSSDSD_OK) {
        std::cerr << "cannot open DXD input: " << error.message << '\n';
        return 1;
    }
    crossdsd_wave_reader_info wave{};
    wave.struct_size = sizeof(wave);
    crossdsd_wave_reader_get_info(reader, &wave);
    if (wave.sample_rate_hz != kDxdRate || wave.bits_per_sample != 32u ||
        wave.frame_count == 0u) {
        std::cerr << "input must be non-empty float32 DXD at 352800 Hz\n";
        crossdsd_wave_reader_close(reader);
        return 1;
    }
    std::vector<float> interleaved(
        static_cast<size_t>(wave.frame_count) * wave.channel_count);
    uint64_t frames_read = 0;
    code = crossdsd_wave_reader_read(
        reader, interleaved.data(), wave.frame_count, &frames_read, &error);
    crossdsd_wave_reader_close(reader);
    if (code != CROSSDSD_OK || frames_read != wave.frame_count) {
        std::cerr << "cannot read complete DXD input: " << error.message << '\n';
        return 1;
    }

    const uint64_t extended_frames =
        kEdgeExtension + wave.frame_count + kEdgeExtension;
    std::vector<float> extended(
        static_cast<size_t>(extended_frames) * wave.channel_count);
    for (uint32_t channel = 0; channel < wave.channel_count; ++channel) {
        std::vector<float> source(static_cast<size_t>(wave.frame_count));
        for (uint64_t frame = 0; frame < wave.frame_count; ++frame) {
            const float value =
                interleaved[
                    static_cast<size_t>(frame) * wave.channel_count + channel];
            if (!std::isfinite(value) ||
                std::abs(value) > 1.428893958511103) {
                std::cerr << "input exceeds the +3.1 dB SACD domain\n";
                return 1;
            }
            source[static_cast<size_t>(frame)] = value;
        }
        const auto prefix =
            make_extension(source.data(), source.size(), true);
        const auto suffix =
            make_extension(source.data(), source.size(), false);
        float* destination =
            extended.data() + static_cast<uint64_t>(channel) * extended_frames;
        std::copy(prefix.begin(), prefix.end(), destination);
        std::copy(
            source.begin(), source.end(), destination + kEdgeExtension);
        std::copy(
            suffix.begin(), suffix.end(),
            destination + kEdgeExtension + wave.frame_count);
    }

    crossdsd_upsampler_config upsampler_config{};
    upsampler_config.struct_size = sizeof(upsampler_config);
    upsampler_config.input_sample_rate_hz = kDxdRate;
    upsampler_config.output_sample_rate_hz = kDsdRate;
    upsampler_config.channel_count = wave.channel_count;
    crossdsd_upsampler* upsampler = nullptr;
    code = crossdsd_upsampler_create(
        &upsampler_config, &upsampler, &error);
    if (code != CROSSDSD_OK) {
        std::cerr << "cannot create upsampler: " << error.message << '\n';
        return 1;
    }
    crossdsd_upsampler_info upsampler_info{};
    upsampler_info.struct_size = sizeof(upsampler_info);
    crossdsd_upsampler_get_info(upsampler, &upsampler_info);
    const uint64_t oversampled_stride = extended_frames * kFactor;
    std::vector<double> oversampled(
        static_cast<size_t>(oversampled_stride) * wave.channel_count);
    uint64_t produced = 0;
    code = crossdsd_upsampler_process(
        upsampler, extended.data(), extended_frames, extended_frames,
        oversampled.data(), oversampled_stride, oversampled_stride,
        &produced, &error);
    crossdsd_upsampler_destroy(upsampler);
    if (code != CROSSDSD_OK || produced != oversampled_stride) {
        std::cerr << "upsampling failed: " << error.message << '\n';
        return 1;
    }

    const uint64_t target_start =
        static_cast<uint64_t>(kEdgeExtension) * kFactor +
        static_cast<uint64_t>(
            std::llround(upsampler_info.latency_output_samples));
    const uint64_t target_bits = wave.frame_count * kFactor;
    if (target_start + target_bits > produced) {
        std::cerr << "invalid Phase 8 alignment window\n";
        return 1;
    }
#if defined(CROSSDSD_PHASE8_DSD128)
    const SearchConfig adaptive{
        kProfileName, 8, 12, 1.0, 0.0, 0.0, 0.0,
        1.0, 0.0, 3u, 0.001};
#elif defined(CROSSDSD_PHASE8_DSD256)
    const SearchConfig adaptive{
        kProfileName, 8, 6, 1.0, 0.0, 0.0, 0.0,
        1.0, 0.0, 2u, 0.0005};
#else
    const SearchConfig adaptive{
        kProfileName, 8, 16, 1.0, 0.0, 0.0, 0.0,
        16.0, 0.0, 3u};
#endif
    std::vector<Simulation> simulations;
    simulations.resize(wave.channel_count);
    std::vector<std::future<Simulation>> channel_tasks;
    channel_tasks.reserve(wave.channel_count);
    const auto encode_start = std::chrono::steady_clock::now();
    for (uint32_t channel = 0; channel < wave.channel_count; ++channel) {
        const double* channel_input =
            oversampled.data() +
            static_cast<uint64_t>(channel) * oversampled_stride +
            target_start;
        channel_tasks.push_back(std::async(
            std::launch::async,
            [adaptive, target_bits, channel_input] {
                if (kWholeProgramSlopeThreshold > 0.0) {
                    double maximum_slope = 0.0;
                    double maximum_peak = 0.0;
                    for (uint64_t index = 1; index < target_bits; ++index) {
                        maximum_peak = std::max(
                            maximum_peak, std::abs(channel_input[index]));
                        maximum_slope = std::max(
                            maximum_slope,
                            std::abs(
                                channel_input[index] -
                                channel_input[index - 1u]));
                    }
                    if (maximum_slope >= kWholeProgramSlopeThreshold ||
                        (kWholeProgramPeakThreshold > 0.0 &&
                         maximum_peak >= kWholeProgramPeakThreshold)) {
                        return simulate_conventional(
                            target_bits,
                            [channel_input](uint64_t index) {
                                return channel_input[index];
                            });
                    }
                }
                return simulate_beam(
                    adaptive, target_bits,
                    [channel_input](uint64_t index) {
                        return channel_input[index];
                    });
            }));
    }
    for (uint32_t channel = 0; channel < wave.channel_count; ++channel) {
        simulations[channel] = channel_tasks[channel].get();
        if (!simulations[channel].stable ||
            simulations[channel].output.size() != target_bits) {
            std::cerr << "adaptive modulator stability failure\n";
            return 1;
        }
    }
    const double encode_seconds =
        std::chrono::duration<double>(
            std::chrono::steady_clock::now() - encode_start).count();

    const uint64_t bytes_per_channel = target_bits / 8u;
    std::vector<uint8_t> packed(
        static_cast<size_t>(bytes_per_channel) * wave.channel_count, 0u);
    for (uint32_t channel = 0; channel < wave.channel_count; ++channel) {
        uint8_t* destination =
            packed.data() + static_cast<uint64_t>(channel) * bytes_per_channel;
        const auto& bits = simulations[channel].output;
        for (uint64_t bit = 0; bit < target_bits; ++bit) {
            if (bits[static_cast<size_t>(bit)] > 0) {
                destination[bit / 8u] |=
                    static_cast<uint8_t>(1u << (bit & 7u));
            }
        }
    }

    crossdsd_dsf_writer_config writer_config{};
    writer_config.struct_size = sizeof(writer_config);
    writer_config.sample_rate_hz = kDsdRate;
    writer_config.channel_count = wave.channel_count;
    crossdsd_dsf_writer* writer = nullptr;
    code = crossdsd_dsf_writer_open_file(
        output_path, &writer_config, &writer, &error);
    if (code == CROSSDSD_OK) {
        code = crossdsd_dsf_writer_write_packed(
            writer, packed.data(), bytes_per_channel, 8, &error);
    }
    if (writer) {
        const auto close_code = crossdsd_dsf_writer_close(writer, &error);
        if (code == CROSSDSD_OK) code = close_code;
    }
    if (code != CROSSDSD_OK) {
        std::cerr << "cannot write DSF output: " << error.message << '\n';
        return 1;
    }

    double maximum_state = 0.0;
    double maximum_quantizer = 0.0;
    uint64_t maximum_run = 0;
    for (const auto& simulation : simulations) {
        maximum_state =
            std::max(maximum_state, simulation.maximum_state);
        maximum_quantizer =
            std::max(
                maximum_quantizer,
                simulation.maximum_quantizer_input);
        maximum_run = std::max(maximum_run, simulation.maximum_run);
    }
    std::ofstream report(report_path);
    report << std::setprecision(12)
           << "{\n"
           << "  \"profile\":\"" << kProfileName << "\",\n"
           << "  \"input_frames\":" << wave.frame_count << ",\n"
           << "  \"channel_count\":" << wave.channel_count << ",\n"
           << "  \"output_dsd_samples_per_channel\":" << target_bits << ",\n"
           << "  \"encode_seconds\":" << encode_seconds << ",\n"
           << "  \"realtime_factor\":"
           << (static_cast<double>(wave.frame_count) / kDxdRate) /
                  encode_seconds
           << ",\n"
           << "  \"maximum_state\":" << maximum_state << ",\n"
           << "  \"maximum_quantizer_input\":" << maximum_quantizer << ",\n"
           << "  \"maximum_identical_bit_run\":" << maximum_run << "\n"
           << "}\n";
    std::cerr << "Encoded Phase 8 Studio " << kRateName << ": "
              << wave.frame_count << " frames, " << encode_seconds
              << " s, max state " << maximum_state << '\n';
    return report ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 5 && std::string_view(argv[1]) == "--encode") {
        return encode_adaptive(argv[2], argv[3], argv[4]);
    }
    const bool initial =
        argc == 2 && std::string_view(argv[1]) == "--initial";
    if (argc > 2 || (argc == 2 && !initial)) {
        std::cerr << "usage: " << argv[0] << " [--initial]\n";
        return 2;
    }
    const std::array<SearchConfig, 7> initial_configurations{{
        {"conventional", 1, 1, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0},
        {"beam-d4-w8-q", 4, 8, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0},
        {"beam-d6-w16-q", 6, 16, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0},
        {"beam-d8-w16-q", 8, 16, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0},
        {"beam-d8-w32-q", 8, 32, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0},
        {"beam-d12-w32-q", 12, 32, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0},
        {"beam-d8-w16-state", 8, 16, 1.0, 0.02, 0.0, 0.0, 0.0, 0.0},
    }};
    const std::array<SearchConfig, 12> adaptive_configurations{{
        {"conventional", 1, 1, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0},
        {"beam-d8-w16-q", 8, 16, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0},
        {"adaptive-0.25", 8, 16, 1.0, 0.0, 0.0, 0.0, 0.25, 0.0},
        {"adaptive-1", 8, 16, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0},
        {"adaptive-4", 8, 16, 1.0, 0.0, 0.0, 0.0, 4.0, 0.0},
        {"adaptive-16", 8, 16, 1.0, 0.0, 0.0, 0.0, 16.0, 0.0},
        {"adaptive-16-w12", 8, 12, 1.0, 0.0, 0.0, 0.0, 16.0, 0.0},
        {"adaptive-16-w8", 8, 8, 1.0, 0.0, 0.0, 0.0, 16.0, 0.0},
        {"adaptive-16-dynamic-gentle", 8, 16, 1.0, 0.0, 0.0, 0.0,
         16.0, 0.0, 1u},
        {"adaptive-16-dynamic-aggressive", 8, 16, 1.0, 0.0, 0.0, 0.0,
         16.0, 0.0, 2u},
        {"hybrid-1.0", 8, 16, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0},
        {"hybrid-1.2", 8, 16, 1.0, 0.0, 0.0, 0.0, 0.0, 1.2},
    }};
    const std::span<const SearchConfig> configurations =
        initial ? std::span<const SearchConfig>(initial_configurations)
                : std::span<const SearchConfig>(adaptive_configurations);
    constexpr double duration = 0.5;
    constexpr uint64_t count =
        static_cast<uint64_t>(duration * kDsdRate);
    std::cout << std::setprecision(12);
    std::cout
        << "candidate,depth,width,signal,stable,max_state,"
           "max_quantizer_input,max_run,one_density,realtime_factor,"
           "gain_db,thdn_db,decoded_noise_dbfs\n";

    for (const auto& config : configurations) {
        const auto run = [&](auto input, std::string_view name,
                             double frequency, double amplitude) {
            const auto simulation =
                config.depth == 1
                    ? simulate_conventional(count, input)
                    : simulate_beam(config, count, input);
            const auto decoded =
                simulation.stable ? decode(simulation.output)
                                  : std::vector<double>{};
            print(config, name, simulation, measure(decoded, frequency, amplitude));
        };
        run([](uint64_t) { return 0.0; }, "silence", 0.0, 0.0);
        run(
            [](uint64_t index) {
                return 0.1 * std::sin(
                    2.0 * std::numbers::pi * 1000.0 *
                    static_cast<double>(index) / kDsdRate);
            },
            "sine-1k--20dB", 1000.0, 0.1);
        run(
            [](uint64_t index) {
                return std::sin(
                    2.0 * std::numbers::pi * 1000.0 *
                    static_cast<double>(index) / kDsdRate);
            },
            "sine-1k-0dB", 1000.0, 1.0);
        run(
            [](uint64_t index) {
                constexpr double amplitude = 1.428893958511103;
                return amplitude * std::sin(
                    2.0 * std::numbers::pi * 1000.0 *
                    static_cast<double>(index) / kDsdRate);
            },
            "sine-1k-+3.1dB", 1000.0, 1.428893958511103);
        run(
            [](uint64_t index) {
                constexpr double amplitude = 0.5011872336272722;
                return amplitude * std::sin(
                    2.0 * std::numbers::pi * 20'000.0 *
                    static_cast<double>(index) / kDsdRate);
            },
            "sine-20k--6dB", 20'000.0, 0.5011872336272722);
        run(
            [](uint64_t index) {
                constexpr double amplitude = 1.428893958511103;
                return amplitude * std::sin(
                    2.0 * std::numbers::pi * 25'000.0 *
                    static_cast<double>(index) / kDsdRate);
            },
            "sine-25k-+3.1dB", 25'000.0, 1.428893958511103);
        if (!initial) {
            run(
                [](uint64_t index) {
                    const uint64_t segment =
                        index / static_cast<uint64_t>(0.05 * kDsdRate);
                    const double amplitude =
                        (segment & 1u) ? 1.428893958511103 : 0.1;
                    return amplitude * std::sin(
                        2.0 * std::numbers::pi * 1000.0 *
                        static_cast<double>(index) / kDsdRate);
                },
                "alternating--20-to-+3.1dB", -1.0, -1.0);
        }
    }
    return 0;
}
