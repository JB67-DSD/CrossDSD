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
#include <iomanip>
#include <iostream>
#include <limits>
#include <numbers>
#include <span>
#include <string_view>
#include <vector>

namespace {

constexpr uint32_t kDxdRate = 352'800;
uint32_t gDsdRate = 2'822'400;
uint32_t gFactor = 8;
constexpr double kMaximumSacdDb = 3.1;
constexpr double kMaximumModulatorInput =
    0.5 * 1.428893958511103;  // 0.5 * 10^(3.1/20)
constexpr double kAbortState = 32.0;
constexpr double kAbortQuantizerInput = 8.0;

struct Candidate {
    const char* name;
    uint32_t order;
    double h_inf;
    std::span<const double> matrix;
};

/*
 * CRFB state-space realizations generated for CrossDSD with synthesizeNTF()
 * using optimized zeros, OSR=56 and the stated Lee H_inf. Rows are
 * (order + 1), columns are (order + 2): [A B1 B2; C D1 D2].
 */
constexpr std::array<double, 20> kOrder3H12{{
    1, 0, 0, 0.005076764282623747, -0.005076764282623747,
    1, 1, -0.0018880200421345084, 0.056467646432450176, -0.056467646432450176,
    1, 1, 0.9981119799578655, 0.36267901191653806, -0.36267901191653806,
    0, 0, 1, 1, 0,
}};
constexpr std::array<double, 20> kOrder3H13{{
    1, 0, 0, 0.013900248547442273, -0.013900248547442273,
    1, 1, -0.0018880200421345084, 0.11190035706701884, -0.11190035706701884,
    1, 1, 0.9981119799578655, 0.5207458537038033, -0.5207458537038033,
    0, 0, 1, 1, 0,
}};
constexpr std::array<double, 30> kOrder4H12{{
    1, -0.00036376417955596985, 0, 0, 0.0002986922280410329, -0.0002986922280410329,
    1, 0.999636235820444, 0, 0, 0.0061746496652699346, -0.0061746496652699346,
    0, 1, 1, -0.002333366890020949, 0.05630735678468955, -0.05630735678468955,
    0, 1, 1, 0.997666633109979, 0.3627990264590949, -0.3627990264590949,
    0, 0, 0, 1, 1, 0,
}};
constexpr std::array<double, 30> kOrder4H13{{
    1, -0.00036376417955596985, 0, 0, 0.0012173319653940777, -0.0012173319653940777,
    1, 0.999636235820444, 0, 0, 0.017507397000886087, -0.017507397000886087,
    0, 1, 1, -0.002333366890020949, 0.11282525604011939, -0.11282525604011939,
    0, 1, 1, 0.997666633109979, 0.5219069219232255, -0.5219069219232255,
    0, 0, 0, 1, 1, 0,
}};
constexpr std::array<double, 30> kOrder4H14{{
    1, -0.00036376417955596985, 0, 0, 0.003084522317267444, -0.003084522317267444,
    1, 0.999636235820444, 0, 0, 0.03510926349905984, -0.03510926349905984,
    0, 1, 1, -0.002333366890020949, 0.17800806424721052, -0.17800806424721052,
    0, 1, 1, 0.997666633109979, 0.6684917782546562, -0.6684917782546562,
    0, 0, 0, 1, 1, 0,
}};
constexpr std::array<double, 42> kOrder5H12{{
    1, 0, 0, 0, 0, 1.5475258164353244e-05, -1.5475258164353244e-05,
    1, 1, -0.0009124573574517125, 0, 0, 0.00039754907986068856, -0.00039754907986068856,
    1, 1, 0.9990875426425483, 0, 0, 0.0064595693688479965, -0.0064595693688479965,
    0, 0, 1, 1, -0.002583800370977052, 0.05582771686796117, -0.05582771686796117,
    0, 0, 1, 1, 0.997416199629023, 0.362596775565246, -0.362596775565246,
    0, 0, 0, 0, 1, 1, 0,
}};
constexpr std::array<double, 42> kOrder5H13{{
    1, 0, 0, 0, 0, 8.741185436858398e-05, -8.741185436858398e-05,
    1, 1, -0.0009124573574517125, 0, 0, 0.0016836384478749033, -0.0016836384478749033,
    1, 1, 0.9990875426425483, 0, 0, 0.018720771278748743, -0.018720771278748743,
    0, 0, 1, 1, -0.002583800370977052, 0.11277566200318506, -0.11277566200318506,
    0, 0, 1, 1, 0.997416199629023, 0.5220934553897641, -0.5220934553897641,
    0, 0, 0, 0, 1, 1, 0,
}};
constexpr std::array<double, 42> kOrder5H132{{
    1, 0, 0, 0, 0, 0.0001140921006448975, -0.0001140921006448975,
    1, 1, -0.0009124573574517125, 0, 0, 0.002094235552669032, -0.002094235552669032,
    1, 1, 0.9990875426425483, 0, 0, 0.02202261405592412, -0.02202261405592412,
    0, 0, 1, 1, -0.002583800370977052, 0.12538314873840334, -0.12538314873840334,
    0, 0, 1, 1, 0.997416199629023, 0.5524647736407551, -0.5524647736407551,
    0, 0, 0, 0, 1, 1, 0,
}};
constexpr std::array<double, 42> kOrder5H134{{
    1, 0, 0, 0, 0, 0.00014615156009228965, -0.00014615156009228965,
    1, 1, -0.0009124573574517125, 0, 0, 0.002563584939257462, -0.002563584939257462,
    1, 1, 0.9990875426425483, 0, 0, 0.025607199662280585, -0.025607199662280585,
    0, 0, 1, 1, -0.002583800370977052, 0.13830487459443375, -0.13830487459443375,
    0, 0, 1, 1, 0.997416199629023, 0.5823609648207648, -0.5823609648207648,
    0, 0, 0, 0, 1, 1, 0,
}};
constexpr std::array<double, 42> kOrder5H136{{
    1, 0, 0, 0, 0, 0.00018414875782358676, -0.00018414875782358676,
    1, 1, -0.0009124573574517125, 0, 0, 0.0030944084820559202, -0.0030944084820559202,
    1, 1, 0.9990875426425483, 0, 0, 0.02947223719454941, -0.02947223719454941,
    0, 0, 1, 1, -0.002583800370977052, 0.15150800632430825, -0.15150800632430825,
    0, 0, 1, 1, 0.997416199629023, 0.6117951336604941, -0.6117951336604941,
    0, 0, 0, 0, 1, 1, 0,
}};
constexpr std::array<double, 56> kOrder6H12{{
    1, -0.00017919794965370883, 0, 0, 0, 0, 5.179604310392225e-07, -5.179604310392225e-07,
    1, 0.9998208020503463, 0, 0, 0, 0, 2.4169545684980934e-05, -2.4169545684980934e-05,
    0, 1, 1, -0.0013757913106493547, 0, 0, 0.00042752382467010767, -0.00042752382467010767,
    0, 1, 1, 0.9986242086893506, 0, 0, 0.006478311134898876, -0.006478311134898876,
    0, 0, 0, 1, 1, -0.002735860923840372, 0.05525472036721361, -0.05525472036721361,
    0, 0, 0, 1, 1, 0.9972641390761596, 0.3623043121953706, -0.3623043121953706,
    0, 0, 0, 0, 0, 1, 1, 0,
}};
constexpr std::array<double, 56> kOrder6H13{{
    1, -0.00017919794965370883, 0, 0, 0, 0, 4.567143324943735e-06, -4.567143324943735e-06,
    1, 0.9998208020503463, 0, 0, 0, 0, 0.0001414930535411918, -0.0001414930535411918,
    0, 1, 1, -0.0013757913106493547, 0, 0, 0.001880979904963398, -0.001880979904963398,
    0, 1, 1, 0.9986242086893506, 0, 0, 0.019154785197571383, -0.019154785197571383,
    0, 0, 0, 1, 1, -0.002735860923840372, 0.1124338067709108, -0.1124338067709108,
    0, 0, 0, 1, 1, 0.9972641390761596, 0.521986875965539, -0.521986875965539,
    0, 0, 0, 0, 0, 1, 1, 0,
}};
constexpr std::array<double, 56> kOrder6H132{{
    1, -0.00017919794965370883, 0, 0, 0, 0, 6.3576870140347925e-06, -6.3576870140347925e-06,
    1, 0.9998208020503463, 0, 0, 0, 0, 0.00018554345155357031, -0.00018554345155357031,
    0, 1, 1, -0.0013757913106493547, 0, 0, 0.002348970308417405, -0.002348970308417405,
    0, 1, 1, 0.9986242086893506, 0, 0, 0.022585368619682227, -0.022585368619682227,
    0, 0, 0, 1, 1, -0.002735860923840372, 0.12510146555544258, -0.12510146555544258,
    0, 0, 0, 1, 1, 0.9972641390761596, 0.5524107595335995, -0.5524107595335995,
    0, 0, 0, 0, 0, 1, 1, 0,
}};
constexpr std::array<double, 56> kOrder6H134{{
    1, -0.00017919794965370883, 0, 0, 0, 0, 8.641498649327907e-06, -8.641498649327907e-06,
    1, 0.9998208020503463, 0, 0, 0, 0, 0.0002386944524481221, -0.0002386944524481221,
    0, 1, 1, -0.0013757913106493547, 0, 0, 0.002885209998781521, -0.002885209998781521,
    0, 1, 1, 0.9986242086893506, 0, 0, 0.026315424996456387, -0.026315424996456387,
    0, 0, 0, 1, 1, -0.002735860923840372, 0.13808866098107017, -0.13808866098107017,
    0, 0, 0, 1, 1, 0.9972641390761596, 0.5823656285915467, -0.5823656285915467,
    0, 0, 0, 0, 0, 1, 1, 0,
}};

#include "../src/reference_modulator_coefficients.inc"

const std::array<Candidate, 15> kCandidates{{
    {"crfb-o3-h1.2", 3, 1.2, kOrder3H12},
    {"crfb-o3-h1.3", 3, 1.3, kOrder3H13},
    {"crfb-o4-h1.2", 4, 1.2, kOrder4H12},
    {"crfb-o4-h1.3", 4, 1.3, kOrder4H13},
    {"crfb-o4-h1.4", 4, 1.4, kOrder4H14},
    {"crfb-o5-h1.2", 5, 1.2, kOrder5H12},
    {"crfb-o5-h1.3", 5, 1.3, kOrder5H13},
    {"crfb-o5-h1.32", 5, 1.32, kOrder5H132},
    {"crfb-o5-h1.34", 5, 1.34, kOrder5H134},
    {"crfb-o5-h1.36", 5, 1.36, kOrder5H136},
    {"crfb-o6-h1.2", 6, 1.2, kOrder6H12},
    {"crfb-o6-h1.3", 6, 1.3, kOrder6H13},
    {"crfb-o6-h1.32", 6, 1.32, kOrder6H132},
    {"crfb-o6-h1.34", 6, 1.34, kOrder6H134},
    {"crfb-o7-h1.32", 7, 1.32, kOrder7H132},
}};

#include "phase7_dsd128_candidates.inc"

const std::array<Candidate, 20> kDsd128Candidates{{
    {"dsd128-crfb-o4-h1.2", 4, 1.20, kDsd128Order4H12},
    {"dsd128-crfb-o4-h1.25", 4, 1.25, kDsd128Order4H125},
    {"dsd128-crfb-o4-h1.3", 4, 1.30, kDsd128Order4H13},
    {"dsd128-crfb-o4-h1.32", 4, 1.32, kDsd128Order4H132},
    {"dsd128-crfb-o5-h1.2", 5, 1.20, kDsd128Order5H12},
    {"dsd128-crfb-o5-h1.25", 5, 1.25, kDsd128Order5H125},
    {"dsd128-crfb-o5-h1.3", 5, 1.30, kDsd128Order5H13},
    {"dsd128-crfb-o5-h1.32", 5, 1.32, kDsd128Order5H132},
    {"dsd128-crfb-o6-h1.2", 6, 1.20, kDsd128Order6H12},
    {"dsd128-crfb-o6-h1.25", 6, 1.25, kDsd128Order6H125},
    {"dsd128-crfb-o6-h1.3", 6, 1.30, kDsd128Order6H13},
    {"dsd128-crfb-o6-h1.32", 6, 1.32, kDsd128Order6H132},
    {"dsd128-crfb-o7-h1.2", 7, 1.20, kDsd128Order7H12},
    {"dsd128-crfb-o7-h1.25", 7, 1.25, kDsd128Order7H125},
    {"dsd128-crfb-o7-h1.3", 7, 1.30, kDsd128Order7H13},
    {"dsd128-crfb-o7-h1.32", 7, 1.32, kDsd128Order7H132},
    {"dsd128-crfb-o8-h1.2", 8, 1.20, kDsd128Order8H12},
    {"dsd128-crfb-o8-h1.25", 8, 1.25, kDsd128Order8H125},
    {"dsd128-crfb-o8-h1.3", 8, 1.30, kDsd128Order8H13},
    {"dsd128-crfb-o8-h1.32", 8, 1.32, kDsd128Order8H132},
}};

#include "phase7_dsd256_candidates.inc"

const std::array<Candidate, 20> kDsd256Candidates{{
    {"dsd256-crfb-o4-h1.2", 4, 1.20, kDsd256Order4H12},
    {"dsd256-crfb-o4-h1.25", 4, 1.25, kDsd256Order4H125},
    {"dsd256-crfb-o4-h1.3", 4, 1.30, kDsd256Order4H13},
    {"dsd256-crfb-o4-h1.32", 4, 1.32, kDsd256Order4H132},
    {"dsd256-crfb-o5-h1.2", 5, 1.20, kDsd256Order5H12},
    {"dsd256-crfb-o5-h1.25", 5, 1.25, kDsd256Order5H125},
    {"dsd256-crfb-o5-h1.3", 5, 1.30, kDsd256Order5H13},
    {"dsd256-crfb-o5-h1.32", 5, 1.32, kDsd256Order5H132},
    {"dsd256-crfb-o6-h1.2", 6, 1.20, kDsd256Order6H12},
    {"dsd256-crfb-o6-h1.25", 6, 1.25, kDsd256Order6H125},
    {"dsd256-crfb-o6-h1.3", 6, 1.30, kDsd256Order6H13},
    {"dsd256-crfb-o6-h1.32", 6, 1.32, kDsd256Order6H132},
    {"dsd256-crfb-o7-h1.2", 7, 1.20, kDsd256Order7H12},
    {"dsd256-crfb-o7-h1.25", 7, 1.25, kDsd256Order7H125},
    {"dsd256-crfb-o7-h1.3", 7, 1.30, kDsd256Order7H13},
    {"dsd256-crfb-o7-h1.32", 7, 1.32, kDsd256Order7H132},
    {"dsd256-crfb-o8-h1.2", 8, 1.20, kDsd256Order8H12},
    {"dsd256-crfb-o8-h1.25", 8, 1.25, kDsd256Order8H125},
    {"dsd256-crfb-o8-h1.3", 8, 1.30, kDsd256Order8H13},
    {"dsd256-crfb-o8-h1.32", 8, 1.32, kDsd256Order8H132},
}};

struct Simulation {
    std::vector<int8_t> output;
    uint64_t sample_count = 0;
    double max_state = 0;
    double max_quantizer_input = 0;
    uint64_t max_run = 0;
    double one_density = 0;
    bool stable = true;
};

double matrix_at(const Candidate& candidate, uint32_t row, uint32_t column) {
    return candidate.matrix[
        static_cast<size_t>(row) * (candidate.order + 2u) + column];
}

template <typename Input>
Simulation simulate(
    const Candidate& candidate, uint64_t count, const Input& input,
    bool keep_output = true, double dither_amplitude = 0.0) {
    Simulation result;
    if (keep_output) result.output.resize(static_cast<size_t>(count));
    std::array<double, 8> state{};
    std::array<double, 8> next{};
    int8_t previous = 0;
    uint64_t run = 0;
    uint64_t ones = 0;
    uint64_t random_state = 0x9e3779b97f4a7c15ULL;
    for (uint64_t index = 0; index < count; ++index) {
        const double u = input(index);
        double quantizer_input =
            matrix_at(candidate, candidate.order, candidate.order) * u;
        for (uint32_t column = 0; column < candidate.order; ++column) {
            quantizer_input +=
                matrix_at(candidate, candidate.order, column) * state[column];
        }
        if (dither_amplitude != 0.0) {
            random_state =
                random_state * 6364136223846793005ULL + 1442695040888963407ULL;
            const double first =
                static_cast<double>(random_state >> 11u) *
                (1.0 / 9007199254740992.0);
            random_state =
                random_state * 6364136223846793005ULL + 1442695040888963407ULL;
            const double second =
                static_cast<double>(random_state >> 11u) *
                (1.0 / 9007199254740992.0);
            quantizer_input += dither_amplitude * (first - second);
        }
        const int8_t bit = quantizer_input >= 0 ? 1 : -1;
        if (keep_output) result.output[static_cast<size_t>(index)] = bit;
        result.sample_count = index + 1u;
        ones += bit > 0;
        run = bit == previous ? run + 1u : 1u;
        previous = bit;
        result.max_run = std::max(result.max_run, run);
        result.max_quantizer_input =
            std::max(result.max_quantizer_input, std::abs(quantizer_input));

        for (uint32_t row = 0; row < candidate.order; ++row) {
            double value =
                matrix_at(candidate, row, candidate.order) * u +
                matrix_at(candidate, row, candidate.order + 1u) * bit;
            for (uint32_t column = 0; column < candidate.order; ++column) {
                value += matrix_at(candidate, row, column) * state[column];
            }
            next[row] = value;
            result.max_state = std::max(result.max_state, std::abs(value));
        }
        for (uint32_t row = 0; row < candidate.order; ++row) {
            state[row] = next[row];
        }
        if (!std::isfinite(result.max_state) ||
            !std::isfinite(result.max_quantizer_input) ||
            result.max_state > kAbortState ||
            result.max_quantizer_input > kAbortQuantizerInput) {
            result.stable = false;
            if (keep_output) {
                result.output.resize(static_cast<size_t>(index + 1u));
            }
            break;
        }
    }
    result.one_density =
        static_cast<double>(ones) /
        static_cast<double>(std::max<uint64_t>(result.sample_count, 1u));
    return result;
}

std::vector<double> decode(const std::vector<int8_t>& dsd) {
    crossdsd_decoder_config config{};
    config.struct_size = sizeof(config);
    config.input_sample_rate_hz = gDsdRate;
    config.channel_count = 1;
    config.level_mode = CROSSDSD_LEVEL_PCM_CONTINUITY;
    config.filter_profile = CROSSDSD_FILTER_STANDARD;
    crossdsd_decoder* decoder = nullptr;
    if (crossdsd_decoder_create(&config, &decoder, nullptr) != CROSSDSD_OK) {
        return {};
    }
    std::vector<double> output(dsd.size() / gFactor + 1u);
    uint64_t produced = 0;
    const auto result = crossdsd_decoder_process(
        decoder, dsd.data(), dsd.size(), dsd.size(), output.data(),
        output.size(), output.size(), &produced, nullptr);
    crossdsd_decoder_destroy(decoder);
    if (result != CROSSDSD_OK) return {};
    output.resize(static_cast<size_t>(produced));
    return output;
}

struct AudioMetrics {
    double recovered_amplitude = 0;
    double gain_db = -std::numeric_limits<double>::infinity();
    double thdn_db = std::numeric_limits<double>::infinity();
};

AudioMetrics measure_tone(
    const std::vector<double>& decoded, double frequency,
    double expected_amplitude) {
    AudioMetrics metrics;
    const size_t trim = static_cast<size_t>(0.05 * kDxdRate);
    if (decoded.size() <= 2u * trim) return metrics;
    const size_t begin = trim;
    const size_t end = decoded.size() - trim;
    const double omega =
        2.0 * std::numbers::pi * frequency / static_cast<double>(kDxdRate);
    double mean = 0;
    for (size_t index = begin; index < end; ++index) mean += decoded[index];
    mean /= static_cast<double>(end - begin);
    double sine = 0;
    double cosine = 0;
    for (size_t index = begin; index < end; ++index) {
        const double value = decoded[index] - mean;
        sine += value * std::sin(omega * static_cast<double>(index));
        cosine += value * std::cos(omega * static_cast<double>(index));
    }
    const double scale = 2.0 / static_cast<double>(end - begin);
    sine *= scale;
    cosine *= scale;
    metrics.recovered_amplitude = std::hypot(sine, cosine);
    metrics.gain_db =
        20.0 * std::log10(metrics.recovered_amplitude / expected_amplitude);

    double residual_power = 0;
    for (size_t index = begin; index < end; ++index) {
        const double fitted =
            mean + sine * std::sin(omega * static_cast<double>(index)) +
            cosine * std::cos(omega * static_cast<double>(index));
        const double residual = decoded[index] - fitted;
        residual_power += residual * residual;
    }
    residual_power /= static_cast<double>(end - begin);
    const double signal_power =
        0.5 * metrics.recovered_amplitude * metrics.recovered_amplitude;
    metrics.thdn_db = 10.0 * std::log10(residual_power / signal_power);
    return metrics;
}

double rms_dbfs(const std::vector<double>& values) {
    const size_t trim = static_cast<size_t>(0.05 * kDxdRate);
    if (values.size() <= 2u * trim) return 0;
    double sum = 0;
    for (size_t index = trim; index < values.size() - trim; ++index) {
        sum += values[index] * values[index];
    }
    const double rms =
        std::sqrt(sum / static_cast<double>(values.size() - 2u * trim));
    return 20.0 * std::log10(std::max(rms, 1e-300));
}

void print_case(
    const Candidate& candidate, std::string_view test, const Simulation& sim,
    double gain_db = std::numeric_limits<double>::quiet_NaN(),
    double thdn_db = std::numeric_limits<double>::quiet_NaN(),
    double noise_dbfs = std::numeric_limits<double>::quiet_NaN()) {
    std::cout << candidate.name << ',' << candidate.order << ','
              << candidate.h_inf << ',' << test << ','
              << (sim.stable ? "true" : "false") << ','
              << sim.max_state << ',' << sim.max_quantizer_input << ','
              << sim.max_run << ',' << sim.one_density << ',';
    if (std::isfinite(gain_db)) std::cout << gain_db;
    std::cout << ',';
    if (std::isfinite(thdn_db)) std::cout << thdn_db;
    std::cout << ',';
    if (std::isfinite(noise_dbfs)) std::cout << noise_dbfs;
    std::cout << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    const bool extended_only =
        argc == 2 && std::string_view(argv[1]) == "--extended-only";
    const bool high_order_only =
        argc == 2 && std::string_view(argv[1]) == "--high-order";
    const bool dither_only =
        argc == 2 && std::string_view(argv[1]) == "--dither";
    const bool dsd128_only =
        argc == 2 && std::string_view(argv[1]) == "--dsd128";
    const bool dsd128_extended =
        argc == 2 && std::string_view(argv[1]) == "--dsd128-extended";
    const bool dsd256_only =
        argc == 2 && std::string_view(argv[1]) == "--dsd256";
    const bool dsd256_extended =
        argc == 2 && std::string_view(argv[1]) == "--dsd256-extended";
    if (argc > 2 ||
        (argc == 2 && !extended_only && !high_order_only && !dither_only &&
         !dsd128_only && !dsd128_extended && !dsd256_only &&
         !dsd256_extended)) {
        std::cerr << "usage: " << argv[0]
                  << " [--extended-only|--high-order|--dither|--dsd128"
                     "|--dsd128-extended|--dsd256|--dsd256-extended]\n";
        return 2;
    }
    std::cout << std::setprecision(12);
    std::cout
        << "candidate,order,h_inf,test,stable,max_state,"
           "max_quantizer_input,max_run,one_density,gain_db,thdn_db,"
           "decoded_noise_dbfs\n";

    if (dsd128_only || dsd128_extended) {
        gDsdRate = 5'644'800;
        gFactor = 16;
    } else if (dsd256_only || dsd256_extended) {
        gDsdRate = 11'289'600;
        gFactor = 32;
    }
    const uint64_t one_second = gDsdRate;
    if (dither_only) {
        const Candidate& candidate = kCandidates[6];
        constexpr double frequency = 1000.0;
        constexpr double pcm_amplitude = 0.1;
        for (const double dither :
             {1e-7, 3e-7, 1e-6, 3e-6, 1e-5, 3e-5, 1e-4, 3e-4, 1e-3,
              3e-3, 1e-2}) {
            const auto tone = simulate(
                candidate, one_second,
                [](uint64_t index) {
                    return 0.5 * pcm_amplitude *
                           std::sin(
                               2.0 * std::numbers::pi * frequency *
                               static_cast<double>(index) / gDsdRate);
                },
                true, dither);
            const auto metrics =
                measure_tone(decode(tone.output), frequency, pcm_amplitude);
            char name[64]{};
            std::snprintf(name, sizeof(name), "dither-%.0e", dither);
            print_case(candidate, name, tone, metrics.gain_db, metrics.thdn_db);
        }
        return 0;
    }

    const std::span<const Candidate> active_candidates =
        (dsd128_only || dsd128_extended)
            ? std::span<const Candidate>(kDsd128Candidates)
        : (dsd256_only || dsd256_extended)
            ? std::span<const Candidate>(kDsd256Candidates)
            : std::span<const Candidate>(kCandidates);
    if (!extended_only && !dsd128_extended && !dsd256_extended)
        for (const auto& candidate : active_candidates) {
        if (high_order_only && candidate.order < 7u) continue;
        const auto silence =
            simulate(candidate, 2u * one_second, [](uint64_t) { return 0.0; });
        const auto decoded_silence =
            silence.stable ? decode(silence.output) : std::vector<double>{};
        print_case(
            candidate, "silence-2s", silence,
            std::numeric_limits<double>::quiet_NaN(),
            std::numeric_limits<double>::quiet_NaN(),
            decoded_silence.empty() ? std::numeric_limits<double>::quiet_NaN()
                                    : rms_dbfs(decoded_silence));

        for (const double level_db : {-60.0, -20.0, 0.0, 1.5, kMaximumSacdDb}) {
            const double pcm_amplitude = std::pow(10.0, level_db / 20.0);
            const double modulator_amplitude = 0.5 * pcm_amplitude;
            constexpr double frequency = 1000.0;
            const auto tone = simulate(
                candidate, one_second, [=](uint64_t index) {
                    return modulator_amplitude *
                           std::sin(
                               2.0 * std::numbers::pi * frequency *
                               static_cast<double>(index) / gDsdRate);
                });
            AudioMetrics metrics;
            if (tone.stable) {
                metrics = measure_tone(
                    decode(tone.output), frequency, pcm_amplitude);
            }
            char name[64]{};
            std::snprintf(name, sizeof(name), "sine-1k-%+.1fdB", level_db);
            print_case(candidate, name, tone, metrics.gain_db, metrics.thdn_db);
        }

        for (const double frequency : {20.0, 25'000.0}) {
            const auto tone = simulate(
                candidate, 2u * one_second, [=](uint64_t index) {
                    return kMaximumModulatorInput *
                           std::sin(
                               2.0 * std::numbers::pi * frequency *
                               static_cast<double>(index) / gDsdRate);
                });
            print_case(
                candidate, frequency == 20.0 ? "sine-20Hz-+3.1dB"
                                             : "sine-25k-+3.1dB",
                tone);
        }

        const auto positive_dc = simulate(
            candidate, 5u * one_second,
            [](uint64_t) { return kMaximumModulatorInput; });
        print_case(candidate, "dc-+3.1dB-5s", positive_dc);

        const auto transitions = simulate(
            candidate, 2u * one_second, [](uint64_t index) {
                const uint64_t period = gDsdRate / 100u;
                return ((index / period) & 1u) ? kMaximumModulatorInput
                                               : -kMaximumModulatorInput;
            });
        print_case(candidate, "transitions-+3.1dB", transitions);
    }

    if (high_order_only || dsd128_only || dsd256_only) return 0;

    const uint64_t long_test = 60u * one_second;
    if (dsd128_extended) {
        for (const size_t candidate_index : {3u, 6u, 9u}) {
            const Candidate& selected = kDsd128Candidates[candidate_index];
            const auto dc = simulate(
                selected, long_test,
                [](uint64_t) { return kMaximumModulatorInput; }, false);
            print_case(selected, "extended-dc-+3.1dB-60s", dc);
            for (const double frequency : {20.0, 25'000.0}) {
                const auto tone = simulate(
                    selected, long_test, [=](uint64_t index) {
                        return kMaximumModulatorInput *
                               std::sin(2.0 * std::numbers::pi * frequency *
                                        static_cast<double>(index) /
                                        gDsdRate);
                    }, false);
                print_case(
                    selected,
                    frequency == 20.0
                        ? "extended-sine-20Hz-+3.1dB-60s"
                        : "extended-sine-25k-+3.1dB-60s",
                    tone);
            }
            const auto transitions = simulate(
                selected, long_test, [](uint64_t index) {
                    const uint64_t period = gDsdRate / 100u;
                    return ((index / period) & 1u)
                               ? kMaximumModulatorInput
                               : -kMaximumModulatorInput;
                }, false);
            print_case(
                selected, "extended-transitions-+3.1dB-60s", transitions);
        }
        return 0;
    }
    if (dsd256_extended) {
        for (const size_t candidate_index : {6u, 9u, 12u, 16u}) {
            const Candidate& selected = kDsd256Candidates[candidate_index];
            const auto dc = simulate(
                selected, long_test,
                [](uint64_t) { return kMaximumModulatorInput; }, false);
            print_case(selected, "extended-dc-+3.1dB-60s", dc);
            for (const double frequency : {20.0, 25'000.0}) {
                const auto tone = simulate(
                    selected, long_test, [=](uint64_t index) {
                        return kMaximumModulatorInput *
                               std::sin(2.0 * std::numbers::pi * frequency *
                                        static_cast<double>(index) /
                                        gDsdRate);
                    }, false);
                print_case(
                    selected,
                    frequency == 20.0
                        ? "extended-sine-20Hz-+3.1dB-60s"
                        : "extended-sine-25k-+3.1dB-60s",
                    tone);
            }
            const auto transitions = simulate(
                selected, long_test, [](uint64_t index) {
                    const uint64_t period = gDsdRate / 100u;
                    return ((index / period) & 1u)
                               ? kMaximumModulatorInput
                               : -kMaximumModulatorInput;
                }, false);
            print_case(
                selected, "extended-transitions-+3.1dB-60s", transitions);
        }
        return 0;
    }
    for (const size_t candidate_index : {3u, 6u, 7u, 11u, 14u}) {
        const Candidate& selected = kCandidates[candidate_index];
        const auto long_dc = simulate(
            selected, long_test,
            [](uint64_t) { return kMaximumModulatorInput; }, false);
        print_case(selected, "extended-dc-+3.1dB-60s", long_dc);
        for (const double frequency : {20.0, 25'000.0}) {
            const auto long_tone = simulate(
                selected, long_test, [=](uint64_t index) {
                    return kMaximumModulatorInput *
                           std::sin(
                               2.0 * std::numbers::pi * frequency *
                               static_cast<double>(index) / gDsdRate);
                }, false);
            print_case(
                selected, frequency == 20.0
                              ? "extended-sine-20Hz-+3.1dB-60s"
                              : "extended-sine-25k-+3.1dB-60s",
                long_tone);
        }
        const auto long_transitions = simulate(
            selected, long_test, [](uint64_t index) {
                const uint64_t period = gDsdRate / 100u;
                return ((index / period) & 1u) ? kMaximumModulatorInput
                                               : -kMaximumModulatorInput;
            }, false);
        print_case(
            selected, "extended-transitions-+3.1dB-60s", long_transitions);
    }
    return 0;
}
