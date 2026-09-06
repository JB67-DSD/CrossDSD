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

#include "enhanced_modulator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <future>
#include <limits>
#include <new>
#include <vector>

namespace crossdsd::enhanced {
namespace {

constexpr uint32_t kRate = 2'822'400;
constexpr uint32_t kOrder = 7;
constexpr uint32_t kDepth = 8;
constexpr uint32_t kMaximumWidth = 16;
constexpr double kPcmToModulator = 0.5;
constexpr double kMaximumPcmAmplitude = 1.428893958511103;
constexpr double kAbortState = 8.0;
constexpr double kAbortQuantizer = 8.0;
constexpr double kAdaptiveWeight = 16.0;
const double kEnvelopeRelease =
    std::exp(-1.0 / (0.002 * static_cast<double>(kRate)));
const double kSelectorRelease =
    std::exp(-1.0 / (0.050 * static_cast<double>(kRate)));
constexpr double kEnhancedEnterLevel = 0.05;
constexpr double kEnhancedExitLevel = 0.10;
constexpr uint64_t kMinimumSegmentSamples = kRate / 50u;

#include "reference_modulator_coefficients.inc"

constexpr auto& kMatrix = kOrder7H132;

double matrix_at(uint32_t row, uint32_t column) {
    return kMatrix[
        static_cast<size_t>(row) * (kOrder + 2u) + column];
}

struct State {
    std::array<double, kOrder> modulator{};
    double level_envelope = 0.0;
    int8_t previous_bit = 0;
};

struct Transition {
    State state;
    double quantizer_input = 0.0;
    double maximum_state = 0.0;
    double cost = 0.0;
    bool stable = true;
};

Transition transition(const State& current, double pcm_input, int8_t bit) {
    Transition result;
    const double input = kPcmToModulator * pcm_input;
    double quantizer_input = matrix_at(kOrder, kOrder) * input;
    for (uint32_t column = 0; column < kOrder; ++column) {
        quantizer_input +=
            matrix_at(kOrder, column) * current.modulator[column];
    }
    result.quantizer_input = quantizer_input;
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
        if (!std::isfinite(value) || std::abs(value) > kAbortState) {
            result.stable = false;
        }
    }
    if (!std::isfinite(quantizer_input) ||
        std::abs(quantizer_input) > kAbortQuantizer) {
        result.stable = false;
    }
    result.state.level_envelope =
        std::max(
            std::abs(pcm_input),
            kEnvelopeRelease * current.level_envelope);
    result.state.previous_bit = bit;
    const double quantizer_error =
        static_cast<double>(bit) - quantizer_input;
    const bool disagrees_with_greedy =
        bit != (quantizer_input >= 0.0 ? int8_t{1} : int8_t{-1});
    const double normalized_level =
        std::min(result.state.level_envelope / kMaximumPcmAmplitude, 1.0);
    const double level2 = normalized_level * normalized_level;
    result.cost =
        quantizer_error * quantizer_error +
        kAdaptiveWeight * level2 * level2 * disagrees_with_greedy;
    return result;
}

struct Path {
    State endpoint;
    uint16_t positive_bits = 0;
    uint32_t length = 0;
    double cost = 0.0;
};

struct Expansion {
    State endpoint;
    double cost = 0.0;
    uint8_t parent = 0;
    int8_t bit = 0;
};

struct Channel {
    std::array<Path, kMaximumWidth> beam{};
    size_t beam_count = 1;
    State committed;
    crossdsd_modulator_channel_metrics metrics{};
    uint64_t current_run = 0;
    uint64_t one_count = 0;
    uint64_t sample_count = 0;
    int8_t previous_bit = 0;
    std::array<double, kDepth> pending_inputs{};
    uint32_t pending_input_count = 0;
};

uint32_t width_for_level(double envelope) {
    const double level =
        std::min(envelope / kMaximumPcmAmplitude, 1.0);
    return level >= 0.85 ? 1u
         : level >= 0.60 ? 4u
         : level >= 0.02 ? 8u
                         : 16u;
}

struct Selector {
    double envelope = 0.0;
    uint64_t samples_since_transition = kMinimumSegmentSamples;
    bool enhanced = false;
};

uint32_t select_width(Selector& selector, double input) {
    selector.envelope =
        std::max(std::abs(input), kSelectorRelease * selector.envelope);
    if (selector.samples_since_transition <
        std::numeric_limits<uint64_t>::max()) {
        ++selector.samples_since_transition;
    }
    if (!selector.enhanced &&
        selector.envelope <= kEnhancedEnterLevel &&
        selector.samples_since_transition >= kMinimumSegmentSamples) {
        selector.enhanced = true;
        selector.samples_since_transition = 0;
    } else if (
        selector.enhanced &&
        selector.envelope >= kEnhancedExitLevel &&
        selector.samples_since_transition >= kMinimumSegmentSamples) {
        selector.enhanced = false;
        selector.samples_since_transition = 0;
    }
    return selector.enhanced ? width_for_level(selector.envelope) : 1u;
}

bool commit(Channel& channel, double input, int8_t bit) {
    const auto step = transition(channel.committed, input, bit);
    if (!step.stable) return false;
    channel.committed = step.state;
    auto& metrics = channel.metrics;
    metrics.maximum_absolute_input =
        std::max(metrics.maximum_absolute_input, std::abs(input));
    metrics.maximum_absolute_state =
        std::max(metrics.maximum_absolute_state, step.maximum_state);
    metrics.maximum_absolute_quantizer_input =
        std::max(
            metrics.maximum_absolute_quantizer_input,
            std::abs(step.quantizer_input));
    channel.current_run =
        bit == channel.previous_bit ? channel.current_run + 1u : 1u;
    channel.previous_bit = bit;
    metrics.maximum_identical_bit_run =
        std::max(metrics.maximum_identical_bit_run, channel.current_run);
    channel.one_count += bit > 0;
    ++channel.sample_count;
    return true;
}

bool advance(
    Channel& channel, double input, uint32_t retained_width,
    int8_t& output, bool& produced) {
    if (channel.pending_input_count >= kDepth) return false;
    channel.pending_inputs[channel.pending_input_count++] = input;
    std::array<Path, kMaximumWidth> next_beam{};
    std::array<Expansion, 2u * kMaximumWidth> expanded{};
    size_t expanded_count = 0;
    for (size_t path_index = 0; path_index < channel.beam_count; ++path_index) {
        const auto& path = channel.beam[path_index];
        for (const int8_t bit : {int8_t{-1}, int8_t{1}}) {
            const auto step = transition(path.endpoint, input, bit);
            if (!step.stable) continue;
            auto& next = expanded[expanded_count++];
            next.endpoint = step.state;
            next.cost = path.cost + step.cost;
            next.parent = static_cast<uint8_t>(path_index);
            next.bit = bit;
        }
    }
    if (expanded_count == 0u) return false;
    channel.beam_count =
        std::min<size_t>(retained_width, expanded_count);
    std::partial_sort(
        expanded.begin(), expanded.begin() + channel.beam_count,
        expanded.begin() + expanded_count,
        [](const Expansion& left, const Expansion& right) {
            return left.cost < right.cost;
        });
    for (size_t retained = 0; retained < channel.beam_count; ++retained) {
        const auto& candidate = expanded[retained];
        auto& next = next_beam[retained];
        next = channel.beam[candidate.parent];
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
    channel.beam = std::move(next_beam);
    produced = false;
    if (channel.beam[0].length < kDepth) return true;

    output =
        (channel.beam[0].positive_bits & 1u) != 0u
            ? int8_t{1}
            : int8_t{-1};
    const double committed_input = channel.pending_inputs[0];
    const double common_cost =
        transition(channel.committed, committed_input, output).cost;
    if (!commit(channel, committed_input, output)) return false;
    std::move(
        channel.pending_inputs.begin() + 1,
        channel.pending_inputs.begin() + channel.pending_input_count,
        channel.pending_inputs.begin());
    --channel.pending_input_count;

    size_t rebased_count = 0;
    for (size_t path_index = 0; path_index < channel.beam_count; ++path_index) {
        auto path = channel.beam[path_index];
        const int8_t first =
            (path.positive_bits & 1u) != 0u ? int8_t{1} : int8_t{-1};
        if (first != output) continue;
        path.cost -= common_cost;
        --path.length;
        path.positive_bits >>= 1u;
        channel.beam[rebased_count++] = std::move(path);
    }
    channel.beam_count = rebased_count;
    produced = true;
    return rebased_count != 0u;
}

}  // namespace

struct Modulator {
    uint32_t channel_count = 0;
    std::vector<Channel> channels;
    std::vector<Selector> selectors;
    uint16_t pending_enhanced = 0;
    uint32_t pending_length = 0;
    uint64_t enhanced_sample_count = 0;
    uint64_t enhanced_segment_count = 0;
    bool previous_enhanced = false;
    uint32_t failed = 0;
    bool finished = false;
};

Modulator* create(uint32_t channel_count) {
    if (channel_count == 0 || channel_count > 256) return nullptr;
    try {
        auto* result = new Modulator;
        result->channel_count = channel_count;
        result->channels.resize(channel_count);
        result->selectors.resize(channel_count);
        for (uint32_t channel = 0; channel < channel_count; ++channel) {
            result->channels[channel].metrics.struct_size =
                sizeof(crossdsd_modulator_channel_metrics);
            result->channels[channel].metrics.channel_index = channel;
        }
        return result;
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

void destroy(Modulator* modulator) {
    delete modulator;
}

crossdsd_result process(
    Modulator* modulator, const double* input_planar,
    uint64_t input_count_per_channel, uint64_t input_stride,
    int8_t* output_planar, uint64_t output_capacity_per_channel,
    uint64_t output_stride, uint64_t* output_count_per_channel) {
    if (!modulator || !output_count_per_channel ||
        (input_count_per_channel != 0 && (!input_planar || !output_planar)) ||
        input_stride < input_count_per_channel ||
        output_stride < output_capacity_per_channel ||
        modulator->finished) {
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    *output_count_per_channel = 0;
    if (modulator->failed) return CROSSDSD_ERROR_OVERFLOW;
    std::vector<uint32_t> widths(
        static_cast<size_t>(
            input_count_per_channel * modulator->channel_count));
    for (uint32_t channel = 0; channel < modulator->channel_count; ++channel) {
        auto& selector = modulator->selectors[channel];
        for (uint64_t index = 0; index < input_count_per_channel; ++index) {
            const double input =
                input_planar[
                    static_cast<uint64_t>(channel) * input_stride + index];
            widths[
                static_cast<uint64_t>(channel) * input_count_per_channel +
                index] = select_width(selector, input);
        }
    }

    const auto process_channel = [&](uint32_t channel) {
        uint64_t produced = 0;
        for (uint64_t index = 0; index < input_count_per_channel; ++index) {
            int8_t bit = 0;
            bool emitted = false;
            const double input =
                input_planar[
                    static_cast<uint64_t>(channel) * input_stride + index];
            if (!advance(
                    modulator->channels[channel], input,
                    widths[
                        static_cast<uint64_t>(channel) *
                            input_count_per_channel +
                        index],
                    bit, emitted)) {
                return std::numeric_limits<uint64_t>::max();
            }
            if (emitted) {
                if (produced >= output_capacity_per_channel) {
                    return std::numeric_limits<uint64_t>::max();
                }
                output_planar[
                    static_cast<uint64_t>(channel) * output_stride +
                    produced++] = bit;
            }
        }
        return produced;
    };

    std::vector<uint64_t> channel_counts(modulator->channel_count);
    if (modulator->channel_count > 1u &&
        input_count_per_channel >= 4'096u) {
        std::vector<std::future<uint64_t>> workers;
        workers.reserve(modulator->channel_count);
        for (uint32_t channel = 0; channel < modulator->channel_count;
             ++channel) {
            workers.emplace_back(
                std::async(std::launch::async, process_channel, channel));
        }
        for (uint32_t channel = 0; channel < modulator->channel_count;
             ++channel) {
            channel_counts[channel] = workers[channel].get();
        }
    } else {
        for (uint32_t channel = 0; channel < modulator->channel_count;
             ++channel) {
            channel_counts[channel] = process_channel(channel);
        }
    }
    const uint64_t produced_count = channel_counts[0];
    for (const uint64_t count : channel_counts) {
        if (count == std::numeric_limits<uint64_t>::max()) {
            modulator->failed = 1;
            return CROSSDSD_ERROR_OVERFLOW;
        }
        if (count != produced_count) {
            modulator->failed = 1;
            return CROSSDSD_ERROR_INVALID_CHUNK;
        }
    }

    uint64_t tracked_produced = 0;
    for (uint64_t index = 0; index < input_count_per_channel; ++index) {
        bool enhanced = false;
        for (uint32_t channel = 0; channel < modulator->channel_count;
             ++channel) {
            enhanced =
                enhanced ||
                widths[
                    static_cast<uint64_t>(channel) *
                        input_count_per_channel +
                    index] > 1u;
        }
        if (enhanced) {
            modulator->pending_enhanced |=
                static_cast<uint16_t>(1u << modulator->pending_length);
        }
        ++modulator->pending_length;
        if (modulator->pending_length >= kDepth) {
            const bool enhanced =
                (modulator->pending_enhanced & 1u) != 0u;
            if (enhanced) {
                ++modulator->enhanced_sample_count;
                if (!modulator->previous_enhanced) {
                    ++modulator->enhanced_segment_count;
                }
            }
            modulator->previous_enhanced = enhanced;
            modulator->pending_enhanced >>= 1u;
            --modulator->pending_length;
            ++tracked_produced;
        }
    }
    if (tracked_produced != produced_count) {
        modulator->failed = 1;
        return CROSSDSD_ERROR_INVALID_CHUNK;
    }
    *output_count_per_channel = produced_count;
    return CROSSDSD_OK;
}

crossdsd_result finish(
    Modulator* modulator, int8_t* output_planar,
    uint64_t output_capacity_per_channel, uint64_t output_stride,
    uint64_t* output_count_per_channel) {
    if (!modulator || !output_count_per_channel || modulator->finished ||
        output_stride < output_capacity_per_channel ||
        (modulator->pending_length != 0 && !output_planar)) {
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    if (modulator->failed) return CROSSDSD_ERROR_OVERFLOW;
    uint64_t produced = 0;
    while (modulator->pending_length != 0) {
        if (produced >= output_capacity_per_channel) {
            return CROSSDSD_ERROR_INVALID_ARGUMENT;
        }
        for (uint32_t channel = 0; channel < modulator->channel_count; ++channel) {
            auto& state = modulator->channels[channel];
            if (state.beam_count == 0 || state.beam[0].length == 0) {
                modulator->failed = 1;
                return CROSSDSD_ERROR_INVALID_CHUNK;
            }
            const int8_t bit =
                (state.beam[0].positive_bits & 1u) != 0u
                    ? int8_t{1}
                    : int8_t{-1};
            if (state.pending_input_count == 0 ||
                !commit(state, state.pending_inputs[0], bit)) {
                modulator->failed = 1;
                return CROSSDSD_ERROR_OVERFLOW;
            }
            std::move(
                state.pending_inputs.begin() + 1,
                state.pending_inputs.begin() + state.pending_input_count,
                state.pending_inputs.begin());
            --state.pending_input_count;
            output_planar[
                static_cast<uint64_t>(channel) * output_stride + produced] =
                bit;
            --state.beam[0].length;
            state.beam[0].positive_bits >>= 1u;
        }
        const bool enhanced =
            (modulator->pending_enhanced & 1u) != 0u;
        if (enhanced) {
            ++modulator->enhanced_sample_count;
            if (!modulator->previous_enhanced) {
                ++modulator->enhanced_segment_count;
            }
        }
        modulator->previous_enhanced = enhanced;
        modulator->pending_enhanced >>= 1u;
        --modulator->pending_length;
        ++produced;
    }
    modulator->finished = true;
    *output_count_per_channel = produced;
    return CROSSDSD_OK;
}

crossdsd_result reset(Modulator* modulator) {
    if (!modulator) return CROSSDSD_ERROR_INVALID_ARGUMENT;
    const uint32_t channel_count = modulator->channel_count;
    std::vector<Channel> channels(channel_count);
    for (uint32_t channel = 0; channel < channel_count; ++channel) {
        channels[channel].metrics.struct_size =
            sizeof(crossdsd_modulator_channel_metrics);
        channels[channel].metrics.channel_index = channel;
    }
    modulator->channels = std::move(channels);
    modulator->selectors.assign(channel_count, Selector{});
    modulator->pending_enhanced = 0;
    modulator->pending_length = 0;
    modulator->enhanced_sample_count = 0;
    modulator->enhanced_segment_count = 0;
    modulator->previous_enhanced = false;
    modulator->failed = 0;
    modulator->finished = false;
    return CROSSDSD_OK;
}

crossdsd_result get_channel_metrics(
    const Modulator* modulator, uint32_t channel_index,
    crossdsd_modulator_channel_metrics* metrics) {
    if (!modulator || !metrics ||
        metrics->struct_size < sizeof(*metrics) ||
        channel_index >= modulator->channel_count) {
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    *metrics = modulator->channels[channel_index].metrics;
    const auto& channel = modulator->channels[channel_index];
    metrics->one_density =
        channel.sample_count == 0
            ? 0.0
            : static_cast<double>(channel.one_count) /
                static_cast<double>(channel.sample_count);
    return CROSSDSD_OK;
}

uint64_t enhanced_samples(const Modulator* modulator) {
    return modulator ? modulator->enhanced_sample_count : 0;
}

uint64_t enhanced_segments(const Modulator* modulator) {
    return modulator ? modulator->enhanced_segment_count : 0;
}

uint32_t stability_failure(const Modulator* modulator) {
    return modulator ? modulator->failed : 1u;
}

}  // namespace crossdsd::enhanced
