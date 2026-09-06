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
#include <cstdint>
#include <iostream>
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

crossdsd_decoder* create(
    uint32_t rate, uint32_t channels, crossdsd_level_mode level,
    crossdsd_filter_profile profile = CROSSDSD_FILTER_STANDARD) {
    crossdsd_decoder_config config{};
    config.struct_size = sizeof(config);
    config.input_sample_rate_hz = rate;
    config.channel_count = channels;
    config.level_mode = level;
    config.filter_profile = profile;
    crossdsd_decoder* decoder = nullptr;
    crossdsd_error error{};
    error.struct_size = sizeof(error);
    const auto result = crossdsd_decoder_create(&config, &decoder, &error);
    if (result != CROSSDSD_OK) {
        std::cerr << "create failed: " << error.message << '\n';
        ++failures;
    }
    return decoder;
}

std::vector<double> decode(
    uint32_t rate, const std::vector<int8_t>& input, const std::vector<size_t>& chunks,
    crossdsd_level_mode level = CROSSDSD_LEVEL_DSD_RAW) {
    crossdsd_decoder* decoder = create(rate, 1, level);
    if (!decoder) return {};
    std::vector<double> result;
    size_t position = 0;
    size_t chunk_index = 0;
    while (position < input.size()) {
        const size_t count =
            std::min(chunks[chunk_index++ % chunks.size()], input.size() - position);
        const uint64_t capacity = count / (rate / 352'800u) + 1u;
        std::vector<double> output(static_cast<size_t>(capacity));
        uint64_t produced = 0;
        CHECK(crossdsd_decoder_process(
                  decoder, input.data() + position, count, count, output.data(),
                  capacity, capacity, &produced, nullptr) == CROSSDSD_OK);
        result.insert(result.end(), output.begin(), output.begin() + static_cast<ptrdiff_t>(produced));
        position += count;
    }
    crossdsd_decoder_destroy(decoder);
    return result;
}

void test_rates_and_filter() {
    for (const auto [rate, stages] : {
             std::pair<uint32_t, uint32_t>{2'822'400, 3},
             {5'644'800, 4},
             {11'289'600, 5},
         }) {
        crossdsd_decoder* decoder = create(rate, 2, CROSSDSD_LEVEL_PCM_CONTINUITY);
        if (!decoder) continue;
        crossdsd_decoder_info info{};
        info.struct_size = sizeof(info);
        CHECK(crossdsd_decoder_get_info(decoder, &info) == CROSSDSD_OK);
        CHECK(info.output_sample_rate_hz == 352'800);
        CHECK(info.decimation_factor == rate / 352'800);
        CHECK(info.stage_count == stages);
        CHECK(info.applied_gain == 2.0);
        CHECK(info.latency_output_samples > 0.0);

        double minimum_db = 0.0;
        double maximum_db = -1000.0;
        for (unsigned step = 0; step <= 200; ++step) {
            const double frequency = 20'000.0 * step / 200.0;
            double magnitude = 0.0, phase = 0.0;
            CHECK(crossdsd_decoder_frequency_response(
                      decoder, frequency, &magnitude, &phase) == CROSSDSD_OK);
            const double db = 20.0 * std::log10(magnitude);
            minimum_db = std::min(minimum_db, db);
            maximum_db = std::max(maximum_db, db);
        }
        CHECK(maximum_db - minimum_db <= 0.01);
        for (double frequency : {40'000.0, 60'000.0, 100'000.0, 176'400.0}) {
            double magnitude = 0.0, phase = 0.0;
            CHECK(crossdsd_decoder_frequency_response(
                      decoder, frequency, &magnitude, &phase) == CROSSDSD_OK);
            CHECK(20.0 * std::log10(std::max(magnitude, 1e-300)) <= -100.0);
        }
        crossdsd_decoder_destroy(decoder);
    }
}

void test_block_independence_and_level() {
    std::vector<int8_t> input(50'003);
    uint32_t state = 0x12345678u;
    for (auto& value : input) {
        state = state * 1664525u + 1013904223u;
        value = (state >> 31u) ? 1 : -1;
    }
    for (uint32_t rate :
         {2'822'400u, 5'644'800u, 11'289'600u, 22'579'200u}) {
        const auto whole = decode(rate, input, {input.size()});
        const auto fragmented = decode(rate, input, {1, 7, 31, 4093, 16});
        CHECK(whole == fragmented);
        const auto doubled =
            decode(rate, input, {257, 3, 8191}, CROSSDSD_LEVEL_PCM_CONTINUITY);
        CHECK(whole.size() == doubled.size());
        for (size_t index = 0; index < whole.size(); ++index) {
            CHECK(doubled[index] == whole[index] * 2.0);
        }
    }
}

void test_raw_filter_profile() {
    for (uint32_t rate :
         {2'822'400u, 5'644'800u, 11'289'600u, 22'579'200u}) {
        crossdsd_decoder* decoder = create(
            rate, 1, CROSSDSD_LEVEL_PCM_CONTINUITY, CROSSDSD_FILTER_RAW);
        if (!decoder) continue;
        crossdsd_decoder_info info{};
        info.struct_size = sizeof(info);
        CHECK(crossdsd_decoder_get_info(decoder, &info) == CROSSDSD_OK);
        CHECK(info.filter_profile == CROSSDSD_FILTER_RAW);
        for (double frequency : {20'000.0, 40'000.0, 100'000.0, 150'000.0}) {
            double magnitude = 0.0, phase = 0.0;
            CHECK(crossdsd_decoder_frequency_response(
                      decoder, frequency, &magnitude, &phase) == CROSSDSD_OK);
            CHECK(20.0 * std::log10(std::max(magnitude, 1e-300)) >= -0.01);
        }
        double magnitude = 0.0, phase = 0.0;
        CHECK(crossdsd_decoder_frequency_response(
                  decoder, 176'400.0, &magnitude, &phase) == CROSSDSD_OK);
        CHECK(20.0 * std::log10(std::max(magnitude, 1e-300)) <= -100.0);
        crossdsd_decoder_destroy(decoder);
    }
}

void test_channels_and_no_clipping() {
    constexpr uint64_t count = 20'000;
    std::vector<int8_t> input(count * 2u);
    std::fill(input.begin(), input.begin() + count, 1);
    for (uint64_t i = 0; i < count; ++i) input[count + i] = (i % 2u) ? 1 : -1;
    crossdsd_decoder* decoder =
        create(2'822'400, 2, CROSSDSD_LEVEL_PCM_CONTINUITY);
    if (!decoder) return;
    const uint64_t capacity = count / 8u + 1u;
    std::vector<double> output(capacity * 2u);
    uint64_t produced = 0;
    CHECK(crossdsd_decoder_process(
              decoder, input.data(), count, count, output.data(), capacity,
              capacity, &produced, nullptr) == CROSSDSD_OK);
    CHECK(produced == count / 8u);
    const auto left_peak = *std::max_element(
        output.begin(), output.begin() + static_cast<ptrdiff_t>(produced));
    const auto right_peak = *std::max_element(
        output.begin() + static_cast<ptrdiff_t>(capacity),
        output.begin() + static_cast<ptrdiff_t>(capacity + produced));
    CHECK(left_peak > 1.9);
    CHECK(right_peak < 0.1);
    crossdsd_decoder_destroy(decoder);
}

}  // namespace

int main() {
    test_rates_and_filter();
    test_block_independence_and_level();
    test_raw_filter_profile();
    test_channels_and_no_clipping();
    if (failures) {
        std::cerr << failures << " decoder test(s) failed\n";
        return 1;
    }
    std::cout << "All CrossDSD reference decoder tests passed\n";
    return 0;
}
