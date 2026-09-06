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

crossdsd_upsampler* create(uint32_t channels = 1) {
    crossdsd_upsampler_config config{};
    config.struct_size = sizeof(config);
    config.input_sample_rate_hz = 352'800;
    config.output_sample_rate_hz = 2'822'400;
    config.channel_count = channels;
    crossdsd_upsampler* upsampler = nullptr;
    crossdsd_error error{};
    error.struct_size = sizeof(error);
    CHECK(crossdsd_upsampler_create(&config, &upsampler, &error) == CROSSDSD_OK);
    return upsampler;
}

std::vector<double> process(
    crossdsd_upsampler* upsampler, const std::vector<float>& input,
    const std::vector<size_t>& chunks) {
    std::vector<double> result;
    size_t position = 0;
    size_t chunk_index = 0;
    while (position < input.size()) {
        const size_t count =
            std::min(chunks[chunk_index++ % chunks.size()], input.size() - position);
        std::vector<double> output(count * 8u);
        uint64_t produced = 0;
        CHECK(crossdsd_upsampler_process(
                  upsampler, input.data() + position, count, count,
                  output.data(), output.size(), output.size(), &produced,
                  nullptr) == CROSSDSD_OK);
        CHECK(produced == count * 8u);
        result.insert(
            result.end(), output.begin(),
            output.begin() + static_cast<ptrdiff_t>(produced));
        position += count;
    }
    return result;
}

void test_configuration_response_and_latency() {
    auto* upsampler = create();
    if (!upsampler) return;
    crossdsd_upsampler_info info{};
    info.struct_size = sizeof(info);
    CHECK(crossdsd_upsampler_get_info(upsampler, &info) == CROSSDSD_OK);
    CHECK(info.api_version == CROSSDSD_DSP_API_VERSION);
    CHECK(info.interpolation_factor == 8);
    CHECK(info.interpolation_stage_count == 3);
    CHECK(info.conditioning_filter_taps > 100);
    CHECK(info.interpolation_filter_taps[0] > 0);
    CHECK(info.interpolation_filter_taps[1] > 0);
    CHECK(info.interpolation_filter_taps[2] > 0);
    CHECK(info.latency_output_samples > 0.0);

    double minimum_db = 0.0;
    double maximum_db = -1000.0;
    for (unsigned step = 0; step <= 250; ++step) {
        const double frequency = 25'000.0 * step / 250.0;
        double magnitude = 0.0;
        double phase = 0.0;
        CHECK(crossdsd_upsampler_frequency_response(
                  upsampler, frequency, &magnitude, &phase) == CROSSDSD_OK);
        const double db = 20.0 * std::log10(std::max(magnitude, 1e-300));
        minimum_db = std::min(minimum_db, db);
        maximum_db = std::max(maximum_db, db);
    }
    CHECK(maximum_db - minimum_db <= 0.01);
    for (double frequency : {40'000.0, 60'000.0, 100'000.0, 176'400.0}) {
        double magnitude = 0.0;
        double phase = 0.0;
        CHECK(crossdsd_upsampler_frequency_response(
                  upsampler, frequency, &magnitude, &phase) == CROSSDSD_OK);
        CHECK(20.0 * std::log10(std::max(magnitude, 1e-300)) <= -100.0);
    }
    crossdsd_upsampler_destroy(upsampler);
}

void test_all_rate_architectures_and_tolerances() {
    struct Expected {
        uint32_t rate;
        uint32_t factor;
        uint32_t stages;
        double latency;
    };
    for (const auto expected : {
             Expected{2'822'400, 8, 3, 738.0},
             Expected{5'644'800, 16, 4, 1484.0},
             Expected{11'289'600, 32, 5, 2976.0},
             Expected{22'579'200, 64, 6, 5960.0},
         }) {
        crossdsd_upsampler_config config{};
        config.struct_size = sizeof(config);
        config.input_sample_rate_hz = 352'800;
        config.output_sample_rate_hz = expected.rate;
        config.channel_count = 1;
        crossdsd_upsampler* upsampler = nullptr;
        CHECK(crossdsd_upsampler_create(
                  &config, &upsampler, nullptr) == CROSSDSD_OK);
        if (!upsampler) continue;
        crossdsd_upsampler_info info{};
        info.struct_size = sizeof(info);
        CHECK(crossdsd_upsampler_get_info(upsampler, &info) == CROSSDSD_OK);
        CHECK(info.interpolation_factor == expected.factor);
        CHECK(info.interpolation_stage_count == expected.stages);
        CHECK(info.latency_output_samples == expected.latency);

        double minimum_db = 0.0;
        double maximum_db = -1000.0;
        for (unsigned step = 0; step <= 250; ++step) {
            double magnitude = 0.0;
            double phase = 0.0;
            CHECK(crossdsd_upsampler_frequency_response(
                      upsampler, 25'000.0 * step / 250.0, &magnitude,
                      &phase) == CROSSDSD_OK);
            const double db =
                20.0 * std::log10(std::max(magnitude, 1e-300));
            minimum_db = std::min(minimum_db, db);
            maximum_db = std::max(maximum_db, db);
        }
        CHECK(maximum_db - minimum_db <= 0.01);
        for (const double frequency :
             {40'000.0, 60'000.0, 100'000.0, 176'400.0}) {
            double magnitude = 0.0;
            double phase = 0.0;
            CHECK(crossdsd_upsampler_frequency_response(
                      upsampler, frequency, &magnitude, &phase) ==
                  CROSSDSD_OK);
            CHECK(20.0 * std::log10(std::max(magnitude, 1e-300)) <=
                  -100.0);
        }
        crossdsd_upsampler_destroy(upsampler);
    }
}

void test_block_independence_and_reset() {
    constexpr size_t count = 20'003;
    std::vector<float> input(count);
    for (size_t index = 0; index < count; ++index) {
        input[index] = static_cast<float>(
            1.2 * std::sin(2.0 * std::numbers::pi * 1000.0 *
                           static_cast<double>(index) / 352'800.0));
    }
    auto* whole_upsampler = create();
    auto* fragmented_upsampler = create();
    if (!whole_upsampler || !fragmented_upsampler) return;
    const auto whole = process(whole_upsampler, input, {input.size()});
    const auto fragmented =
        process(fragmented_upsampler, input, {1, 7, 31, 4093, 16});
    CHECK(whole == fragmented);
    CHECK(crossdsd_upsampler_reset(fragmented_upsampler) == CROSSDSD_OK);
    const auto after_reset =
        process(fragmented_upsampler, input, {257, 3, 8191});
    CHECK(whole == after_reset);
    crossdsd_upsampler_destroy(whole_upsampler);
    crossdsd_upsampler_destroy(fragmented_upsampler);
}

void test_impulse_latency() {
    auto* upsampler = create();
    if (!upsampler) return;
    crossdsd_upsampler_info info{};
    info.struct_size = sizeof(info);
    CHECK(crossdsd_upsampler_get_info(upsampler, &info) == CROSSDSD_OK);
    std::vector<float> impulse(400, 0.0f);
    impulse[0] = 1.0f;
    const auto output = process(upsampler, impulse, {13, 1, 257});
    const auto peak = std::max_element(
        output.begin(), output.end(), [](double left, double right) {
            return std::abs(left) < std::abs(right);
        });
    const auto peak_index =
        static_cast<uint64_t>(std::distance(output.begin(), peak));
    CHECK(peak_index == static_cast<uint64_t>(info.latency_output_samples));
    crossdsd_upsampler_destroy(upsampler);
}

void test_gain_and_multichannel() {
    constexpr size_t count = 10'000;
    std::vector<float> input(count * 2u, 0.0f);
    std::fill(input.begin(), input.begin() + count, 1.0f);
    std::vector<double> output(count * 8u * 2u);
    auto* upsampler = create(2);
    if (!upsampler) return;
    uint64_t produced = 0;
    CHECK(crossdsd_upsampler_process(
              upsampler, input.data(), count, count, output.data(), count * 8u,
              count * 8u, &produced, nullptr) == CROSSDSD_OK);
    CHECK(produced == count * 8u);
    const size_t settled = 2000;
    double left_error = 0.0;
    double right_peak = 0.0;
    for (size_t index = settled; index < produced; ++index) {
        left_error = std::max(left_error, std::abs(output[index] - 1.0));
        right_peak = std::max(
            right_peak, std::abs(output[count * 8u + index]));
    }
    if (left_error >= 1e-9) {
        std::cerr << "settled DC error: " << left_error << '\n';
    }
    CHECK(left_error < 1e-9);
    CHECK(right_peak < 1e-15);
    crossdsd_upsampler_destroy(upsampler);
}

void test_invalid_input_is_atomic() {
    auto* upsampler = create();
    if (!upsampler) return;
    const float invalid[] = {
        0.0f, std::numeric_limits<float>::quiet_NaN(), 1.0f};
    double output[24]{};
    uint64_t produced = 99;
    CHECK(crossdsd_upsampler_process(
              upsampler, invalid, 3, 3, output, 24, 24, &produced, nullptr) ==
          CROSSDSD_ERROR_INVALID_ARGUMENT);
    crossdsd_upsampler_info info{};
    info.struct_size = sizeof(info);
    CHECK(crossdsd_upsampler_get_info(upsampler, &info) == CROSSDSD_OK);
    CHECK(info.input_samples_consumed_per_channel == 0);
    CHECK(info.output_samples_produced_per_channel == 0);
    crossdsd_upsampler_destroy(upsampler);
}

void test_modulator_compatibility_at_maximum_level() {
    constexpr double maximum = 1.428893958511103;
    constexpr size_t count = 352'800;
    for (const double frequency : {20.0, 1000.0, 25'000.0}) {
        std::vector<float> input(count);
        for (size_t index = 0; index < count; ++index) {
            input[index] = static_cast<float>(
                maximum * std::sin(
                              2.0 * std::numbers::pi * frequency *
                              static_cast<double>(index) / 352'800.0));
        }
        auto* upsampler = create();
        if (!upsampler) continue;
        crossdsd_modulator_config modulator_config{};
        modulator_config.struct_size = sizeof(modulator_config);
        modulator_config.sample_rate_hz = 2'822'400;
        modulator_config.channel_count = 1;
        modulator_config.profile =
            CROSSDSD_MODULATOR_CRFB_ORDER7_HINF_1_32;
        crossdsd_modulator* modulator = nullptr;
        CHECK(crossdsd_modulator_create(
                  &modulator_config, &modulator, nullptr) == CROSSDSD_OK);
        if (!modulator) {
            crossdsd_upsampler_destroy(upsampler);
            continue;
        }
        constexpr size_t chunk = 4096;
        std::vector<double> oversampled(chunk * 8u);
        std::vector<int8_t> dsd(chunk * 8u);
        size_t position = 0;
        uint64_t oversampled_position = 0;
        while (position < input.size()) {
            const size_t current = std::min(chunk, input.size() - position);
            uint64_t produced = 0;
            CHECK(crossdsd_upsampler_process(
                      upsampler, input.data() + position, current, current,
                      oversampled.data(), oversampled.size(),
                      oversampled.size(), &produced, nullptr) == CROSSDSD_OK);
            const uint64_t skip =
                oversampled_position < 2048u
                    ? std::min<uint64_t>(
                          produced, 2048u - oversampled_position)
                    : 0u;
            crossdsd_error error{};
            error.struct_size = sizeof(error);
            const auto modulation_result = crossdsd_modulator_process(
                modulator, oversampled.data() + skip, produced - skip,
                produced - skip, dsd.data(), dsd.size(), dsd.size(), &error);
            if (modulation_result != CROSSDSD_OK) {
                const auto peak = *std::max_element(
                    oversampled.begin(),
                    oversampled.begin() + static_cast<ptrdiff_t>(produced),
                    [](double left, double right) {
                        return std::abs(left) < std::abs(right);
                    });
                std::cerr << "compatibility failure at " << frequency
                          << " Hz, peak " << std::abs(peak) << ": "
                          << error.message << '\n';
            }
            CHECK(modulation_result == CROSSDSD_OK);
            oversampled_position += produced;
            position += current;
        }
        crossdsd_modulator_channel_metrics metrics{};
        metrics.struct_size = sizeof(metrics);
        CHECK(crossdsd_modulator_get_channel_metrics(
                  modulator, 0, &metrics) == CROSSDSD_OK);
        CHECK(metrics.maximum_absolute_state < 2.0);
        CHECK(metrics.maximum_absolute_quantizer_input < 2.0);
        crossdsd_modulator_destroy(modulator);
        crossdsd_upsampler_destroy(upsampler);
    }
}

}  // namespace

int main() {
    test_configuration_response_and_latency();
    test_all_rate_architectures_and_tolerances();
    test_block_independence_and_reset();
    test_impulse_latency();
    test_gain_and_multichannel();
    test_invalid_input_is_atomic();
    test_modulator_compatibility_at_maximum_level();
    if (failures) {
        std::cerr << failures << " upsampler test(s) failed\n";
        return 1;
    }
    std::cout << "All CrossDSD reference upsampler tests passed\n";
    return 0;
}
