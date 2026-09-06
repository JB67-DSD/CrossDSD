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
#include "crossdsd/formats.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <numbers>
#include <string>
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

crossdsd_encoder* create(
    uint32_t channels = 1, uint32_t output_rate = 2'822'400,
    crossdsd_modulator_profile profile =
        CROSSDSD_MODULATOR_CRFB_ORDER7_HINF_1_32,
    crossdsd_encoder_profile encoder_profile =
        CROSSDSD_ENCODER_STANDARD) {
    crossdsd_encoder_config config{};
    config.struct_size = sizeof(config);
    config.input_sample_rate_hz = 352'800;
    config.output_sample_rate_hz = output_rate;
    config.channel_count = channels;
    config.modulator_profile = profile;
    config.encoder_profile = encoder_profile;
    crossdsd_encoder* encoder = nullptr;
    crossdsd_error error{};
    error.struct_size = sizeof(error);
    CHECK(crossdsd_encoder_create(&config, &encoder, &error) == CROSSDSD_OK);
    return encoder;
}

std::vector<std::vector<uint8_t>> encode(
    crossdsd_encoder* encoder, const std::vector<float>& input,
    uint32_t channels, const std::vector<size_t>& chunks);

void test_dsd64_enhanced_segmentation() {
    constexpr size_t section = 80'000;
    constexpr size_t count = section * 3u;
    std::vector<float> input(count);
    for (size_t index = 0; index < count; ++index) {
        const double amplitude =
            index >= section && index < section * 2u ? 0.01 : 1.40;
        input[index] = static_cast<float>(
            amplitude * std::sin(
                            2.0 * std::numbers::pi * 997.0 *
                            static_cast<double>(index) / 352'800.0));
    }
    auto* whole_encoder = create(
        1, 2'822'400, CROSSDSD_MODULATOR_CRFB_ORDER7_HINF_1_32,
        CROSSDSD_ENCODER_DSD64_ENHANCED);
    auto* fragmented_encoder = create(
        1, 2'822'400, CROSSDSD_MODULATOR_CRFB_ORDER7_HINF_1_32,
        CROSSDSD_ENCODER_DSD64_ENHANCED);
    if (!whole_encoder || !fragmented_encoder) return;
    const auto whole = encode(whole_encoder, input, 1, {input.size()});
    const auto fragmented =
        encode(fragmented_encoder, input, 1, {1, 7, 31, 4093, 16});
    CHECK(whole == fragmented);

    crossdsd_encoder_info info{};
    info.struct_size = sizeof(info);
    CHECK(crossdsd_encoder_get_info(fragmented_encoder, &info) == CROSSDSD_OK);
    CHECK(info.encoder_profile == CROSSDSD_ENCODER_DSD64_ENHANCED);
    CHECK(info.dsd_samples_produced_per_channel == count * 8u);
    CHECK(info.enhanced_dsd_samples_per_channel > 0);
    CHECK(info.enhanced_dsd_samples_per_channel <
          info.dsd_samples_produced_per_channel);
    CHECK(info.enhanced_segment_count >= 2);
    CHECK(info.stability_failure == 0);

    crossdsd_modulator_channel_metrics metrics{};
    metrics.struct_size = sizeof(metrics);
    CHECK(crossdsd_encoder_get_channel_metrics(
              fragmented_encoder, 0, &metrics) == CROSSDSD_OK);
    CHECK(metrics.maximum_absolute_state < 2.0);
    CHECK(metrics.maximum_absolute_quantizer_input < 2.0);

    CHECK(crossdsd_encoder_reset(fragmented_encoder) == CROSSDSD_OK);
    const auto after_reset =
        encode(fragmented_encoder, input, 1, {257, 3, 8191});
    CHECK(whole == after_reset);
    crossdsd_encoder_destroy(whole_encoder);
    crossdsd_encoder_destroy(fragmented_encoder);

    crossdsd_encoder_config invalid{};
    invalid.struct_size = sizeof(invalid);
    invalid.input_sample_rate_hz = 352'800;
    invalid.output_sample_rate_hz = 5'644'800;
    invalid.channel_count = 1;
    invalid.modulator_profile =
        CROSSDSD_MODULATOR_DSD128_CRFB_ORDER5_HINF_1_30;
    invalid.encoder_profile = CROSSDSD_ENCODER_DSD64_ENHANCED;
    crossdsd_encoder* rejected = nullptr;
    CHECK(crossdsd_encoder_create(&invalid, &rejected, nullptr) ==
          CROSSDSD_ERROR_UNSUPPORTED_RATE);
    CHECK(rejected == nullptr);
}

std::vector<std::vector<uint8_t>> encode(
    crossdsd_encoder* encoder, const std::vector<float>& input,
    uint32_t channels, const std::vector<size_t>& chunks) {
    const size_t samples = input.size() / channels;
    crossdsd_encoder_info encoder_info{};
    encoder_info.struct_size = sizeof(encoder_info);
    CHECK(crossdsd_encoder_get_info(encoder, &encoder_info) == CROSSDSD_OK);
    const size_t bytes_per_input = encoder_info.interpolation_factor / 8u;
    std::vector<std::vector<uint8_t>> result(channels);
    size_t position = 0;
    size_t chunk_index = 0;
    while (position < samples) {
        const size_t count =
            std::min(chunks[chunk_index++ % chunks.size()], samples - position);
        const size_t capacity = count * bytes_per_input + 1u;
        std::vector<uint8_t> output(capacity * channels);
        std::vector<float> planar(count * channels);
        for (uint32_t channel = 0; channel < channels; ++channel) {
            std::copy_n(
                input.data() + static_cast<size_t>(channel) * samples + position,
                count, planar.data() + static_cast<size_t>(channel) * count);
        }
        uint64_t produced = 0;
        crossdsd_error error{};
        error.struct_size = sizeof(error);
        const auto process_result = crossdsd_encoder_process(
            encoder, planar.data(), count, count, output.data(), capacity,
            capacity, &produced, &error);
        if (process_result != CROSSDSD_OK) {
            std::cerr << "encoder process failed: " << error.message << '\n';
        }
        CHECK(process_result == CROSSDSD_OK);
        for (uint32_t channel = 0; channel < channels; ++channel) {
            const uint8_t* source =
                output.data() + static_cast<size_t>(channel) * capacity;
            result[channel].insert(
                result[channel].end(), source, source + produced);
        }
        position += count;
    }
    size_t produced_so_far = result[0].size();
    const size_t remaining =
        samples * bytes_per_input - produced_so_far;
    std::vector<uint8_t> final_output(
        std::max<size_t>(remaining, 1u) * channels);
    uint64_t final_bytes = 0;
    crossdsd_error error{};
    error.struct_size = sizeof(error);
    const auto finish_result = crossdsd_encoder_finish(
        encoder, remaining == 0 ? nullptr : final_output.data(), remaining,
        remaining, &final_bytes, &error);
    if (finish_result != CROSSDSD_OK) {
        std::cerr << "encoder finish failed: " << error.message << '\n';
    }
    CHECK(finish_result == CROSSDSD_OK);
    for (uint32_t channel = 0; channel < channels; ++channel) {
        const uint8_t* source =
            final_output.data() + static_cast<size_t>(channel) * remaining;
        result[channel].insert(
            result[channel].end(), source, source + final_bytes);
        CHECK(result[channel].size() == samples * bytes_per_input);
    }
    return result;
}

std::vector<int8_t> expand(const std::vector<uint8_t>& packed) {
    std::vector<int8_t> output(packed.size() * 8u);
    CHECK(crossdsd_expand_dsd_lsb(
              packed.data(), output.size(), output.data(), output.size()) ==
          CROSSDSD_OK);
    return output;
}

void test_info_empty_and_short_streams() {
    auto* encoder = create();
    if (!encoder) return;
    crossdsd_encoder_info info{};
    info.struct_size = sizeof(info);
    CHECK(crossdsd_encoder_get_info(encoder, &info) == CROSSDSD_OK);
    CHECK(info.api_version == CROSSDSD_DSP_API_VERSION);
    CHECK(info.interpolation_factor == 8);
    CHECK(info.edge_extension_input_samples == 94);
    CHECK(info.latency_output_samples == 738.0);
    CHECK(info.maximum_pcm_amplitude > 1.42);
    uint64_t produced = 99;
    CHECK(crossdsd_encoder_finish(
              encoder, nullptr, 0, 0, &produced, nullptr) == CROSSDSD_OK);
    CHECK(produced == 0);
    crossdsd_encoder_destroy(encoder);

    for (const size_t count : {1u, 2u, 17u, 92u, 93u, 94u}) {
        std::vector<float> input(count, 0.25f);
        encoder = create();
        if (!encoder) continue;
        const auto output = encode(encoder, input, 1, {1, 7, 31});
        CHECK(output[0].size() == count);
        CHECK(crossdsd_encoder_get_info(encoder, &info) == CROSSDSD_OK);
        CHECK(info.input_samples_consumed_per_channel == count);
        CHECK(info.dsd_samples_produced_per_channel == count * 8u);
        CHECK(info.packed_bytes_produced_per_channel == count);
        CHECK(info.finished == 1);
        crossdsd_encoder_destroy(encoder);
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
    auto* whole_encoder = create();
    auto* fragmented_encoder = create();
    if (!whole_encoder || !fragmented_encoder) return;
    const auto whole = encode(whole_encoder, input, 1, {input.size()});
    const auto fragmented =
        encode(fragmented_encoder, input, 1, {1, 7, 31, 4093, 16});
    CHECK(whole == fragmented);
    CHECK(crossdsd_encoder_reset(fragmented_encoder) == CROSSDSD_OK);
    const auto after_reset =
        encode(fragmented_encoder, input, 1, {257, 3, 8191});
    CHECK(whole == after_reset);
    crossdsd_encoder_destroy(whole_encoder);
    crossdsd_encoder_destroy(fragmented_encoder);
}

void test_dsd128_progressive_encoder() {
    constexpr size_t count = 20'003;
    std::vector<float> input(count);
    for (size_t index = 0; index < count; ++index) {
        input[index] = static_cast<float>(
            1.2 * std::sin(2.0 * std::numbers::pi * 1000.0 *
                           static_cast<double>(index) / 352'800.0));
    }
    const auto profile =
        CROSSDSD_MODULATOR_DSD128_CRFB_ORDER5_HINF_1_30;
    auto* whole_encoder = create(1, 5'644'800, profile);
    auto* fragmented_encoder = create(1, 5'644'800, profile);
    if (!whole_encoder || !fragmented_encoder) return;
    const auto whole = encode(whole_encoder, input, 1, {input.size()});
    const auto fragmented =
        encode(fragmented_encoder, input, 1, {1, 7, 31, 4093, 16});
    CHECK(whole == fragmented);
    CHECK(whole[0].size() == count * 2u);

    crossdsd_encoder_info info{};
    info.struct_size = sizeof(info);
    CHECK(crossdsd_encoder_get_info(fragmented_encoder, &info) == CROSSDSD_OK);
    CHECK(info.output_sample_rate_hz == 5'644'800);
    CHECK(info.interpolation_factor == 16);
    CHECK(info.dsd_samples_produced_per_channel == count * 16u);
    CHECK(info.packed_bytes_produced_per_channel == count * 2u);
    CHECK(info.stability_failure == 0);

    const auto expanded = expand(whole[0]);
    crossdsd_decoder_config decoder_config{};
    decoder_config.struct_size = sizeof(decoder_config);
    decoder_config.input_sample_rate_hz = 5'644'800;
    decoder_config.channel_count = 1;
    decoder_config.level_mode = CROSSDSD_LEVEL_PCM_CONTINUITY;
    decoder_config.filter_profile = CROSSDSD_FILTER_STANDARD;
    crossdsd_decoder* decoder = nullptr;
    CHECK(crossdsd_decoder_create(
              &decoder_config, &decoder, nullptr) == CROSSDSD_OK);
    std::vector<double> decoded(count + 1u);
    uint64_t decoded_count = 0;
    CHECK(crossdsd_decoder_process(
              decoder, expanded.data(), expanded.size(), expanded.size(),
              decoded.data(), decoded.size(), decoded.size(), &decoded_count,
              nullptr) == CROSSDSD_OK);
    CHECK(decoded_count == count);
    crossdsd_decoder_destroy(decoder);
    crossdsd_encoder_destroy(whole_encoder);
    crossdsd_encoder_destroy(fragmented_encoder);

    constexpr size_t maximum_count = 352'800;
    constexpr double maximum = 1.428893958511103;
    std::vector<float> difficult(maximum_count);
    for (size_t index = 0; index < maximum_count; ++index) {
        difficult[index] = static_cast<float>(
            maximum * std::sin(2.0 * std::numbers::pi * 25'000.0 *
                               static_cast<double>(index) / 352'800.0));
    }
    auto* difficult_encoder = create(1, 5'644'800, profile);
    if (!difficult_encoder) return;
    const auto difficult_output =
        encode(difficult_encoder, difficult, 1, {4096, 17, 65'537});
    CHECK(difficult_output[0].size() == maximum_count * 2u);
    crossdsd_modulator_channel_metrics metrics{};
    metrics.struct_size = sizeof(metrics);
    CHECK(crossdsd_encoder_get_channel_metrics(
              difficult_encoder, 0, &metrics) == CROSSDSD_OK);
    CHECK(metrics.maximum_absolute_state < 2.0);
    CHECK(metrics.maximum_absolute_quantizer_input < 2.0);
    crossdsd_encoder_destroy(difficult_encoder);
}

void test_dsd256_progressive_encoder() {
    constexpr size_t count = 10'003;
    std::vector<float> input(count);
    for (size_t index = 0; index < count; ++index) {
        input[index] = static_cast<float>(
            1.2 * std::sin(2.0 * std::numbers::pi * 1000.0 *
                           static_cast<double>(index) / 352'800.0));
    }
    const auto profile =
        CROSSDSD_MODULATOR_DSD256_CRFB_ORDER7_HINF_1_20;
    auto* whole_encoder = create(1, 11'289'600, profile);
    auto* fragmented_encoder = create(1, 11'289'600, profile);
    if (!whole_encoder || !fragmented_encoder) return;
    const auto whole = encode(whole_encoder, input, 1, {input.size()});
    const auto fragmented =
        encode(fragmented_encoder, input, 1, {1, 7, 31, 4093, 16});
    CHECK(whole == fragmented);
    CHECK(whole[0].size() == count * 4u);
    crossdsd_encoder_info info{};
    info.struct_size = sizeof(info);
    CHECK(crossdsd_encoder_get_info(fragmented_encoder, &info) == CROSSDSD_OK);
    CHECK(info.output_sample_rate_hz == 11'289'600);
    CHECK(info.interpolation_factor == 32);
    CHECK(info.dsd_samples_produced_per_channel == count * 32u);
    CHECK(info.packed_bytes_produced_per_channel == count * 4u);
    CHECK(info.stability_failure == 0);
    crossdsd_encoder_destroy(whole_encoder);
    crossdsd_encoder_destroy(fragmented_encoder);
}

void test_multichannel_independence() {
    constexpr size_t count = 10'000;
    std::vector<float> stereo(count * 2u);
    for (size_t index = 0; index < count; ++index) {
        stereo[index] = static_cast<float>(
            0.8 * std::sin(2.0 * std::numbers::pi * 1000.0 *
                           static_cast<double>(index) / 352'800.0));
        stereo[count + index] = static_cast<float>(
            0.4 * std::sin(2.0 * std::numbers::pi * 3000.0 *
                           static_cast<double>(index) / 352'800.0));
    }
    auto* stereo_encoder = create(2);
    auto* left_encoder = create();
    auto* right_encoder = create();
    if (!stereo_encoder || !left_encoder || !right_encoder) return;
    const auto stereo_output =
        encode(stereo_encoder, stereo, 2, {17, 4096, 3});
    const std::vector<float> left(stereo.begin(), stereo.begin() + count);
    const std::vector<float> right(stereo.begin() + count, stereo.end());
    const auto left_output = encode(left_encoder, left, 1, {997});
    const auto right_output = encode(right_encoder, right, 1, {61, 5});
    CHECK(stereo_output[0] == left_output[0]);
    CHECK(stereo_output[1] == right_output[0]);
    crossdsd_encoder_destroy(stereo_encoder);
    crossdsd_encoder_destroy(left_encoder);
    crossdsd_encoder_destroy(right_encoder);
}

void test_maximum_level_and_cycle_gain() {
    constexpr size_t count = 352'800;
    constexpr double maximum = 1.428893958511103;
    for (const double frequency : {20.0, 1000.0, 25'000.0}) {
        std::vector<float> input(count);
        for (size_t index = 0; index < count; ++index) {
            input[index] = static_cast<float>(
                maximum * std::sin(
                              2.0 * std::numbers::pi * frequency *
                              static_cast<double>(index) / 352'800.0));
        }
        auto* encoder = create();
        if (!encoder) continue;
        const auto packed = encode(encoder, input, 1, {4096, 17, 65'537});
        crossdsd_encoder_info info{};
        info.struct_size = sizeof(info);
        CHECK(crossdsd_encoder_get_info(encoder, &info) == CROSSDSD_OK);
        CHECK(info.stability_failure == 0);
        crossdsd_modulator_channel_metrics metrics{};
        metrics.struct_size = sizeof(metrics);
        CHECK(crossdsd_encoder_get_channel_metrics(
                  encoder, 0, &metrics) == CROSSDSD_OK);
        CHECK(metrics.maximum_absolute_state < 2.0);
        CHECK(metrics.maximum_absolute_quantizer_input < 2.0);

        if (frequency == 1000.0) {
            const auto dsd = expand(packed[0]);
            crossdsd_decoder_config decoder_config{};
            decoder_config.struct_size = sizeof(decoder_config);
            decoder_config.input_sample_rate_hz = 2'822'400;
            decoder_config.channel_count = 1;
            decoder_config.level_mode = CROSSDSD_LEVEL_PCM_CONTINUITY;
            decoder_config.filter_profile = CROSSDSD_FILTER_STANDARD;
            crossdsd_decoder* decoder = nullptr;
            CHECK(crossdsd_decoder_create(
                      &decoder_config, &decoder, nullptr) == CROSSDSD_OK);
            std::vector<double> decoded(count + 1u);
            uint64_t decoded_count = 0;
            CHECK(crossdsd_decoder_process(
                      decoder, dsd.data(), dsd.size(), dsd.size(),
                      decoded.data(), decoded.size(), decoded.size(),
                      &decoded_count, nullptr) == CROSSDSD_OK);
            const size_t begin = 20'000;
            const size_t end =
                std::min<size_t>(decoded_count, decoded.size()) - 20'000;
            const double omega =
                2.0 * std::numbers::pi * frequency / 352'800.0;
            double sine = 0.0;
            double cosine = 0.0;
            for (size_t index = begin; index < end; ++index) {
                sine += decoded[index] *
                        std::sin(omega * static_cast<double>(index));
                cosine += decoded[index] *
                          std::cos(omega * static_cast<double>(index));
            }
            const double amplitude =
                2.0 * std::hypot(sine, cosine) /
                static_cast<double>(end - begin);
            const double gain_db =
                20.0 * std::log10(amplitude / maximum);
            CHECK(std::abs(gain_db) < 0.01);
            crossdsd_decoder_destroy(decoder);
        }
        crossdsd_encoder_destroy(encoder);
    }
}

void test_dsf_writer_round_trip() {
    constexpr size_t count = 4097;
    std::vector<float> input(count);
    for (size_t index = 0; index < count; ++index) {
        input[index] = static_cast<float>(
            0.5 * std::sin(2.0 * std::numbers::pi * 1000.0 *
                           static_cast<double>(index) / 352'800.0));
    }
    auto* encoder = create();
    if (!encoder) return;
    const auto packed = encode(encoder, input, 1, {13, 997});
    crossdsd_encoder_destroy(encoder);

    const auto path =
        std::filesystem::temp_directory_path() /
        "crossdsd-reference-encoder-roundtrip.dsf";
    crossdsd_dsf_writer_config writer_config{};
    writer_config.struct_size = sizeof(writer_config);
    writer_config.sample_rate_hz = 2'822'400;
    writer_config.channel_count = 1;
    crossdsd_dsf_writer* writer = nullptr;
    CHECK(crossdsd_dsf_writer_open_file(
              path.string().c_str(), &writer_config, &writer, nullptr) ==
          CROSSDSD_OK);
    CHECK(crossdsd_dsf_writer_write_packed(
              writer, packed[0].data(), packed[0].size(), 8, nullptr) ==
          CROSSDSD_OK);
    CHECK(crossdsd_dsf_writer_close(writer, nullptr) == CROSSDSD_OK);

    crossdsd_reader* reader = nullptr;
    CHECK(crossdsd_reader_open_file(
              path.string().c_str(), &reader, nullptr) == CROSSDSD_OK);
    crossdsd_stream_info info{};
    info.struct_size = sizeof(info);
    CHECK(crossdsd_reader_get_info(reader, &info) == CROSSDSD_OK);
    CHECK(info.sample_rate_hz == 2'822'400);
    CHECK(info.sample_count_per_channel == count * 8u);
    std::vector<uint8_t> readback(count);
    uint64_t bytes_read = 0;
    uint8_t valid_bits = 0;
    CHECK(crossdsd_reader_read_packed(
              reader, readback.data(), readback.size(), &bytes_read,
              &valid_bits, nullptr) == CROSSDSD_OK);
    CHECK(bytes_read == count);
    CHECK(valid_bits == 8);
    CHECK(readback == packed[0]);
    crossdsd_reader_close(reader);
    std::filesystem::remove(path);
}

void test_errors_and_finish_retry() {
    auto* encoder = create();
    if (!encoder) return;
    const float invalid[] = {
        0.0f, std::numeric_limits<float>::quiet_NaN(), 0.0f};
    uint8_t output[4]{};
    uint64_t produced = 0;
    CHECK(crossdsd_encoder_process(
              encoder, invalid, 3, 3, output, 4, 4, &produced, nullptr) ==
          CROSSDSD_ERROR_INVALID_ARGUMENT);
    const float too_loud[] = {1.43f};
    CHECK(crossdsd_encoder_process(
              encoder, too_loud, 1, 1, output, 2, 2, &produced, nullptr) ==
          CROSSDSD_ERROR_INVALID_ARGUMENT);
    crossdsd_encoder_info info{};
    info.struct_size = sizeof(info);
    CHECK(crossdsd_encoder_get_info(encoder, &info) == CROSSDSD_OK);
    CHECK(info.input_samples_consumed_per_channel == 0);

    std::vector<float> valid(100, 0.0f);
    std::vector<uint8_t> progressive(101);
    CHECK(crossdsd_encoder_process(
              encoder, valid.data(), valid.size(), valid.size(),
              progressive.data(), progressive.size(), progressive.size(),
              &produced, nullptr) == CROSSDSD_OK);
    CHECK(produced <= valid.size());
    const uint64_t remaining = valid.size() - produced;
    std::vector<uint8_t> final_output(remaining);
    CHECK(crossdsd_encoder_finish(
              encoder, final_output.data(), remaining - 1u, remaining - 1u,
              &produced, nullptr) == CROSSDSD_ERROR_INVALID_ARGUMENT);
    CHECK(crossdsd_encoder_finish(
              encoder, final_output.data(), remaining, remaining, &produced,
              nullptr) == CROSSDSD_OK);
    CHECK(produced == remaining);
    CHECK(crossdsd_encoder_process(
              encoder, valid.data(), 1, 1, progressive.data(), 2, 2,
              &produced, nullptr) == CROSSDSD_ERROR_END_OF_STREAM);
    crossdsd_encoder_destroy(encoder);
}

}  // namespace

int main() {
    test_info_empty_and_short_streams();
    test_block_independence_and_reset();
    test_dsd64_enhanced_segmentation();
    test_dsd128_progressive_encoder();
    test_dsd256_progressive_encoder();
    test_multichannel_independence();
    test_maximum_level_and_cycle_gain();
    test_dsf_writer_round_trip();
    test_errors_and_finish_retry();
    if (failures) {
        std::cerr << failures << " encoder test(s) failed\n";
        return 1;
    }
    std::cout << "All CrossDSD progressive encoder tests passed\n";
    return 0;
}
