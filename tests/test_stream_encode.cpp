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
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <numbers>
#include <type_traits>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                      \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: "       \
                      << #condition << '\n';                                     \
            ++failures;                                                         \
        }                                                                       \
    } while (false)

template <typename Sample>
std::vector<std::vector<uint8_t>> encode(
    uint32_t rate, crossdsd_encoder_profile profile, uint32_t channels,
    const std::vector<Sample>& interleaved, const std::vector<size_t>& chunks,
    crossdsd_dxd_to_dsd_stream_info* final_info = nullptr) {
    crossdsd_dxd_to_dsd_stream_config config{};
    config.struct_size = sizeof(config);
    config.output_sample_rate_hz = rate;
    config.channel_count = channels;
    config.encoder_profile = profile;
    crossdsd_dxd_to_dsd_stream* stream = nullptr;
    CHECK(crossdsd_dxd_to_dsd_stream_create(
              &config, &stream, nullptr) == CROSSDSD_OK);
    if (!stream) return {};

    const uint64_t frames = interleaved.size() / channels;
    const uint64_t bytes_per_input = rate / 352'800u / 8u;
    std::vector<std::vector<uint8_t>> result(channels);
    uint64_t position = 0;
    size_t chunk_index = 0;
    while (position < frames) {
        const uint64_t count = std::min<uint64_t>(
            chunks[chunk_index++ % chunks.size()], frames - position);
        const uint64_t capacity = count * bytes_per_input + 1u;
        std::vector<uint8_t> output(capacity * channels);
        uint64_t produced = 0;
        crossdsd_result status;
        if constexpr (std::is_same_v<Sample, float>) {
            status = crossdsd_dxd_to_dsd_stream_process_f32(
                stream, interleaved.data() + position * channels, count,
                output.data(), capacity, capacity, &produced, nullptr);
        } else {
            status = crossdsd_dxd_to_dsd_stream_process_f64(
                stream, interleaved.data() + position * channels, count,
                output.data(), capacity, capacity, &produced, nullptr);
        }
        CHECK(status == CROSSDSD_OK);
        for (uint32_t channel = 0; channel < channels; ++channel) {
            result[channel].insert(
                result[channel].end(),
                output.data() + static_cast<uint64_t>(channel) * capacity,
                output.data() + static_cast<uint64_t>(channel) * capacity +
                    produced);
        }
        position += count;
    }

    crossdsd_dxd_to_dsd_stream_info info{};
    info.struct_size = sizeof(info);
    CHECK(crossdsd_dxd_to_dsd_stream_get_info(stream, &info) == CROSSDSD_OK);
    const uint64_t expected = frames * bytes_per_input;
    CHECK(info.input_dxd_frames == frames);
    CHECK(info.output_packed_bytes_per_channel <= expected);
    const uint64_t remaining =
        expected - info.output_packed_bytes_per_channel;
    std::vector<uint8_t> final_output(
        static_cast<size_t>(remaining * channels));
    uint64_t final_bytes = 0;
    CHECK(crossdsd_dxd_to_dsd_stream_finish(
              stream, remaining == 0 ? nullptr : final_output.data(),
              remaining, remaining, &final_bytes, nullptr) == CROSSDSD_OK);
    CHECK(final_bytes == remaining);
    for (uint32_t channel = 0; channel < channels; ++channel) {
        result[channel].insert(
            result[channel].end(),
            final_output.data() + static_cast<uint64_t>(channel) * remaining,
            final_output.data() +
                static_cast<uint64_t>(channel + 1u) * remaining);
        CHECK(result[channel].size() == expected);
    }
    info.struct_size = sizeof(info);
    CHECK(crossdsd_dxd_to_dsd_stream_get_info(stream, &info) == CROSSDSD_OK);
    CHECK(info.finished == 1);
    CHECK(info.stability_failure == 0);
    CHECK(info.output_packed_bytes_per_channel == expected);
    CHECK(info.channel_input_peaks[0] > 0.0);
    crossdsd_modulator_channel_metrics metrics{};
    metrics.struct_size = sizeof(metrics);
    CHECK(crossdsd_dxd_to_dsd_stream_get_channel_metrics(
              stream, 0, &metrics) == CROSSDSD_OK);
    CHECK(metrics.one_density > 0.0 && metrics.one_density < 1.0);
    if (final_info) *final_info = info;
    crossdsd_dxd_to_dsd_stream_destroy(stream);
    return result;
}

std::vector<float> signal(uint32_t channels, uint64_t frames) {
    std::vector<float> values(frames * channels);
    for (uint64_t frame = 0; frame < frames; ++frame) {
        for (uint32_t channel = 0; channel < channels; ++channel) {
            values[frame * channels + channel] = static_cast<float>(
                (0.65 - channel * 0.1) *
                std::sin(
                    2.0 * std::numbers::pi *
                    (997.0 + channel * 1300.0) *
                    static_cast<double>(frame) / 352'800.0));
        }
    }
    return values;
}

void check_equal(
    const std::vector<std::vector<uint8_t>>& left,
    const std::vector<std::vector<uint8_t>>& right) {
    CHECK(left.size() == right.size());
    for (size_t channel = 0;
         channel < std::min(left.size(), right.size()); ++channel) {
        CHECK(left[channel] == right[channel]);
    }
}

void test_standard_rates_and_fragmentation() {
    constexpr uint32_t channels = 2;
    const auto input = signal(channels, 12'289);
    std::vector<double> input_f64(input.begin(), input.end());
    for (uint32_t rate :
         {2'822'400u, 5'644'800u, 11'289'600u, 22'579'200u}) {
        const auto whole = encode(
            rate, CROSSDSD_ENCODER_STANDARD, channels, input,
            {input.size() / channels});
        const auto fragmented = encode(
            rate, CROSSDSD_ENCODER_STANDARD, channels, input,
            {1, 7, 31, 997});
        const auto f64 = encode(
            rate, CROSSDSD_ENCODER_STANDARD, channels, input_f64,
            {257, 3, 4093});
        check_equal(whole, fragmented);
        check_equal(whole, f64);
    }
}

void test_enhanced_fragmentation() {
    const auto input = signal(1, 24'577);
    crossdsd_dxd_to_dsd_stream_info info{};
    const auto whole = encode(
        2'822'400u, CROSSDSD_ENCODER_DSD64_ENHANCED, 1, input,
        {input.size()}, &info);
    const auto fragmented = encode(
        2'822'400u, CROSSDSD_ENCODER_DSD64_ENHANCED, 1, input,
        {1, 7, 31, 997});
    check_equal(whole, fragmented);
    CHECK(info.encoder_profile == CROSSDSD_ENCODER_DSD64_ENHANCED);
}

void test_invalid_enhanced_rate() {
    crossdsd_dxd_to_dsd_stream_config config{};
    config.struct_size = sizeof(config);
    config.output_sample_rate_hz = 5'644'800u;
    config.channel_count = 1;
    config.encoder_profile = CROSSDSD_ENCODER_DSD64_ENHANCED;
    crossdsd_dxd_to_dsd_stream* stream = nullptr;
    CHECK(crossdsd_dxd_to_dsd_stream_create(
              &config, &stream, nullptr) ==
          CROSSDSD_ERROR_INVALID_ARGUMENT);
    CHECK(stream == nullptr);
}

}  // namespace

int main() {
    test_standard_rates_and_fragmentation();
    test_enhanced_fragmentation();
    test_invalid_enhanced_rate();
    if (failures) {
        std::cerr << failures << " streaming encoder test(s) failed\n";
        return 1;
    }
    std::cout << "All CrossDSD streaming encoder tests passed\n";
    return 0;
}
