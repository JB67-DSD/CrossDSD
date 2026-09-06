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
#include <limits>
#include <numbers>
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

crossdsd_modulator* create(
    uint32_t channels = 1, uint32_t rate = 2'822'400,
    crossdsd_modulator_profile profile =
        CROSSDSD_MODULATOR_CRFB_ORDER7_HINF_1_32) {
    crossdsd_modulator_config config{};
    config.struct_size = sizeof(config);
    config.sample_rate_hz = rate;
    config.channel_count = channels;
    config.profile = profile;
    crossdsd_modulator* modulator = nullptr;
    crossdsd_error error{};
    error.struct_size = sizeof(error);
    CHECK(crossdsd_modulator_create(&config, &modulator, &error) == CROSSDSD_OK);
    return modulator;
}

std::vector<int8_t> process(
    crossdsd_modulator* modulator, const std::vector<double>& input,
    const std::vector<size_t>& chunks) {
    std::vector<int8_t> output(input.size());
    size_t position = 0;
    size_t chunk_index = 0;
    while (position < input.size()) {
        const size_t count =
            std::min(chunks[chunk_index++ % chunks.size()], input.size() - position);
        CHECK(crossdsd_modulator_process(
                  modulator, input.data() + position, count, count,
                  output.data() + position, count, count, nullptr) == CROSSDSD_OK);
        position += count;
    }
    return output;
}

void test_configuration_and_info() {
    auto* modulator = create();
    if (!modulator) return;
    crossdsd_modulator_info info{};
    info.struct_size = sizeof(info);
    CHECK(crossdsd_modulator_get_info(modulator, &info) == CROSSDSD_OK);
    CHECK(info.api_version == CROSSDSD_DSP_API_VERSION);
    CHECK(info.sample_rate_hz == 2'822'400);
    CHECK(info.modulator_order == 7);
    CHECK(info.h_inf == 1.32);
    CHECK(info.maximum_pcm_amplitude == 1.6);
    crossdsd_modulator_destroy(modulator);

    auto* dsd128 = create(
        1, 5'644'800,
        CROSSDSD_MODULATOR_DSD128_CRFB_ORDER5_HINF_1_30);
    if (dsd128) {
        info = {};
        info.struct_size = sizeof(info);
        CHECK(crossdsd_modulator_get_info(dsd128, &info) == CROSSDSD_OK);
        CHECK(info.sample_rate_hz == 5'644'800);
        CHECK(info.modulator_order == 5);
        CHECK(info.h_inf == 1.30);
        crossdsd_modulator_destroy(dsd128);
    }
    auto* dsd256 = create(
        1, 11'289'600,
        CROSSDSD_MODULATOR_DSD256_CRFB_ORDER7_HINF_1_20);
    if (dsd256) {
        info = {};
        info.struct_size = sizeof(info);
        CHECK(crossdsd_modulator_get_info(dsd256, &info) == CROSSDSD_OK);
        CHECK(info.sample_rate_hz == 11'289'600);
        CHECK(info.modulator_order == 7);
        CHECK(info.h_inf == 1.20);
        crossdsd_modulator_destroy(dsd256);
    }
    auto* dsd512 = create(
        1, 22'579'200,
        CROSSDSD_MODULATOR_DSD512_CRFB_ORDER7_HINF_1_20);
    if (dsd512) {
        info = {};
        info.struct_size = sizeof(info);
        CHECK(crossdsd_modulator_get_info(dsd512, &info) == CROSSDSD_OK);
        CHECK(info.sample_rate_hz == 22'579'200);
        CHECK(info.modulator_order == 7);
        CHECK(info.h_inf == 1.20);
        crossdsd_modulator_destroy(dsd512);
    }

    crossdsd_modulator_config config{};
    config.struct_size = sizeof(config);
    config.sample_rate_hz = 5'644'800;
    config.channel_count = 1;
    config.profile = CROSSDSD_MODULATOR_CRFB_ORDER7_HINF_1_32;
    crossdsd_modulator* invalid = nullptr;
    CHECK(crossdsd_modulator_create(&config, &invalid, nullptr) ==
          CROSSDSD_ERROR_UNSUPPORTED_RATE);
    CHECK(invalid == nullptr);
}

void test_block_independence_and_reset() {
    constexpr size_t count = 200'003;
    std::vector<double> input(count);
    for (size_t index = 0; index < count; ++index) {
        input[index] =
            1.2 * std::sin(2.0 * std::numbers::pi * 1000.0 *
                           static_cast<double>(index) / 2'822'400.0);
    }
    auto* whole_modulator = create();
    auto* fragmented_modulator = create();
    if (!whole_modulator || !fragmented_modulator) return;
    const auto whole = process(whole_modulator, input, {input.size()});
    const auto fragmented =
        process(fragmented_modulator, input, {1, 7, 31, 4093, 16});
    CHECK(whole == fragmented);

    CHECK(crossdsd_modulator_reset(fragmented_modulator) == CROSSDSD_OK);
    const auto after_reset =
        process(fragmented_modulator, input, {257, 3, 8191});
    CHECK(whole == after_reset);

    crossdsd_modulator_info info{};
    info.struct_size = sizeof(info);
    CHECK(crossdsd_modulator_get_info(fragmented_modulator, &info) ==
          CROSSDSD_OK);
    CHECK(info.input_samples_consumed_per_channel == count);
    CHECK(info.output_samples_produced_per_channel == count);
    CHECK(info.stability_failure == 0);
    crossdsd_modulator_destroy(whole_modulator);
    crossdsd_modulator_destroy(fragmented_modulator);
}

void test_channels_and_metrics() {
    constexpr size_t count = 100'000;
    std::vector<double> input(count * 2u);
    std::fill(input.begin(), input.begin() + count, 0.0);
    std::fill(input.begin() + count, input.end(), 1.0);
    std::vector<int8_t> output(input.size());
    auto* modulator = create(2);
    if (!modulator) return;
    CHECK(crossdsd_modulator_process(
              modulator, input.data(), count, count, output.data(), count,
              count, nullptr) == CROSSDSD_OK);
    CHECK(std::all_of(output.begin(), output.end(), [](int8_t value) {
        return value == -1 || value == 1;
    }));
    crossdsd_modulator_channel_metrics left{};
    crossdsd_modulator_channel_metrics right{};
    left.struct_size = sizeof(left);
    right.struct_size = sizeof(right);
    CHECK(crossdsd_modulator_get_channel_metrics(modulator, 0, &left) ==
          CROSSDSD_OK);
    CHECK(crossdsd_modulator_get_channel_metrics(modulator, 1, &right) ==
          CROSSDSD_OK);
    CHECK(left.maximum_absolute_input == 0.0);
    CHECK(right.maximum_absolute_input == 1.0);
    CHECK(std::abs(left.one_density - 0.5) < 0.001);
    CHECK(std::abs(right.one_density - 0.75) < 0.001);
    CHECK(right.maximum_absolute_state > 0.0);
    CHECK(right.maximum_absolute_quantizer_input > 0.0);
    crossdsd_modulator_destroy(modulator);
}

void test_maximum_level_and_invalid_input() {
    constexpr double maximum = 1.428893958511103;
    constexpr size_t count = 2'822'400;
    std::vector<double> transitions(count);
    constexpr size_t period = 2'822'400 / 100;
    for (size_t index = 0; index < count; ++index) {
        transitions[index] = ((index / period) & 1u) ? maximum : -maximum;
    }
    auto* modulator = create();
    if (!modulator) return;
    const auto output = process(modulator, transitions, {4096, 17, 65'537});
    CHECK(output.size() == transitions.size());
    crossdsd_modulator_channel_metrics metrics{};
    metrics.struct_size = sizeof(metrics);
    CHECK(crossdsd_modulator_get_channel_metrics(modulator, 0, &metrics) ==
          CROSSDSD_OK);
    CHECK(metrics.maximum_absolute_state < 2.0);
    CHECK(metrics.maximum_absolute_quantizer_input < 2.0);
    CHECK(metrics.maximum_identical_bit_run <= 20);

    crossdsd_modulator_info before{};
    before.struct_size = sizeof(before);
    CHECK(crossdsd_modulator_get_info(modulator, &before) == CROSSDSD_OK);
    const double invalid[] = {1.600001};
    int8_t bit = 0;
    CHECK(crossdsd_modulator_process(
              modulator, invalid, 1, 1, &bit, 1, 1, nullptr) ==
          CROSSDSD_ERROR_INVALID_ARGUMENT);
    const double not_finite[] = {std::numeric_limits<double>::quiet_NaN()};
    CHECK(crossdsd_modulator_process(
              modulator, not_finite, 1, 1, &bit, 1, 1, nullptr) ==
          CROSSDSD_ERROR_INVALID_ARGUMENT);
    crossdsd_modulator_info after{};
    after.struct_size = sizeof(after);
    CHECK(crossdsd_modulator_get_info(modulator, &after) == CROSSDSD_OK);
    CHECK(after.input_samples_consumed_per_channel ==
          before.input_samples_consumed_per_channel);
    crossdsd_modulator_destroy(modulator);
}

}  // namespace

int main() {
    test_configuration_and_info();
    test_block_independence_and_reset();
    test_channels_and_metrics();
    test_maximum_level_and_invalid_input();
    if (failures) {
        std::cerr << failures << " modulator test(s) failed\n";
        return 1;
    }
    std::cout << "All CrossDSD reference modulator tests passed\n";
    return 0;
}
