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

std::vector<float> decode(
    uint32_t rate, uint32_t channels,
    const std::vector<std::vector<uint8_t>>& packed,
    const std::vector<size_t>& chunks) {
    crossdsd_dsd_to_dxd_stream_config config{};
    config.struct_size = sizeof(config);
    config.input_sample_rate_hz = rate;
    config.channel_count = channels;
    config.filter_profile = CROSSDSD_FILTER_STANDARD;
    crossdsd_dsd_to_dxd_stream* stream = nullptr;
    CHECK(crossdsd_dsd_to_dxd_stream_create(
              &config, &stream, nullptr) == CROSSDSD_OK);
    if (!stream) return {};

    std::vector<float> result;
    size_t position = 0;
    size_t chunk_index = 0;
    while (position < packed[0].size()) {
        const size_t count = std::min(
            chunks[chunk_index++ % chunks.size()],
            packed[0].size() - position);
        std::vector<uint8_t> block(count * channels);
        for (uint32_t channel = 0; channel < channels; ++channel) {
            std::copy_n(
                packed[channel].data() + position, count,
                block.data() + static_cast<size_t>(channel) * count);
        }
        const uint64_t capacity = count * 8u / (rate / 352'800u) + 1u;
        std::vector<float> output(capacity * channels);
        uint64_t produced = 0;
        CHECK(crossdsd_dsd_to_dxd_stream_process(
                  stream, block.data(), count, count, 8, output.data(),
                  capacity, &produced, nullptr) == CROSSDSD_OK);
        result.insert(
            result.end(), output.begin(),
            output.begin() + static_cast<ptrdiff_t>(produced * channels));
        position += count;
    }
    crossdsd_dsd_to_dxd_stream_info info{};
    info.struct_size = sizeof(info);
    CHECK(crossdsd_dsd_to_dxd_stream_get_info(stream, &info) == CROSSDSD_OK);
    CHECK(info.input_dsd_samples_per_channel == packed[0].size() * 8u);
    CHECK(info.output_dxd_frames * channels == result.size());
    CHECK(info.channel_peaks[0] > 0.0);
    crossdsd_dsd_to_dxd_stream_destroy(stream);
    return result;
}

std::vector<float> decode_reference(
    uint32_t rate, uint32_t channels,
    const std::vector<std::vector<uint8_t>>& packed) {
    const uint64_t samples = packed[0].size() * 8u;
    std::vector<int8_t> expanded(samples * channels);
    for (uint32_t channel = 0; channel < channels; ++channel) {
        CHECK(crossdsd_expand_dsd_lsb(
                  packed[channel].data(), samples,
                  expanded.data() + static_cast<uint64_t>(channel) * samples,
                  samples) == CROSSDSD_OK);
    }
    crossdsd_decoder_config config{};
    config.struct_size = sizeof(config);
    config.input_sample_rate_hz = rate;
    config.channel_count = channels;
    config.level_mode = CROSSDSD_LEVEL_PCM_CONTINUITY;
    config.filter_profile = CROSSDSD_FILTER_STANDARD;
    crossdsd_decoder* decoder = nullptr;
    CHECK(crossdsd_decoder_create(&config, &decoder, nullptr) == CROSSDSD_OK);
    const uint64_t capacity = samples / (rate / 352'800u) + 1u;
    std::vector<double> planar(capacity * channels);
    uint64_t produced = 0;
    CHECK(crossdsd_decoder_process(
              decoder, expanded.data(), samples, samples, planar.data(),
              capacity, capacity, &produced, nullptr) == CROSSDSD_OK);
    std::vector<float> result(produced * channels);
    for (uint64_t frame = 0; frame < produced; ++frame) {
        for (uint32_t channel = 0; channel < channels; ++channel) {
            result[frame * channels + channel] = static_cast<float>(
                planar[static_cast<uint64_t>(channel) * capacity + frame]);
        }
    }
    crossdsd_decoder_destroy(decoder);
    return result;
}

void test_fragmentation_and_channels() {
    constexpr uint32_t channels = 2;
    constexpr size_t bytes = 10'003;
    std::vector<std::vector<uint8_t>> packed(
        channels, std::vector<uint8_t>(bytes));
    uint32_t state = 0x12345678u;
    for (uint32_t channel = 0; channel < channels; ++channel) {
        for (auto& value : packed[channel]) {
            state = state * 1664525u + 1013904223u;
            value = static_cast<uint8_t>(state >> 24u);
        }
    }
    for (uint32_t rate :
         {2'822'400u, 5'644'800u, 11'289'600u, 22'579'200u}) {
        const auto reference = decode_reference(rate, channels, packed);
        const auto whole = decode(rate, channels, packed, {bytes});
        const auto fragmented = decode(rate, channels, packed, {1, 7, 31, 997});
        CHECK(reference.size() == whole.size());
        CHECK(whole.size() == fragmented.size());
        for (size_t index = 0; index < whole.size(); ++index) {
            CHECK(std::bit_cast<uint32_t>(reference[index]) ==
                  std::bit_cast<uint32_t>(whole[index]));
            CHECK(std::bit_cast<uint32_t>(whole[index]) ==
                  std::bit_cast<uint32_t>(fragmented[index]));
        }
    }
}

void test_partial_byte_seals_stream() {
    crossdsd_dsd_to_dxd_stream_config config{};
    config.struct_size = sizeof(config);
    config.input_sample_rate_hz = 2'822'400;
    config.channel_count = 1;
    config.filter_profile = CROSSDSD_FILTER_STANDARD;
    crossdsd_dsd_to_dxd_stream* stream = nullptr;
    CHECK(crossdsd_dsd_to_dxd_stream_create(
              &config, &stream, nullptr) == CROSSDSD_OK);
    const uint8_t packed = 0x15;
    float output[2]{};
    uint64_t produced = 0;
    CHECK(crossdsd_dsd_to_dxd_stream_process(
              stream, &packed, 1, 1, 5, output, 2, &produced,
              nullptr) == CROSSDSD_OK);
    crossdsd_dsd_to_dxd_stream_info info{};
    info.struct_size = sizeof(info);
    CHECK(crossdsd_dsd_to_dxd_stream_get_info(stream, &info) == CROSSDSD_OK);
    CHECK(info.finished == 1);
    CHECK(info.input_dsd_samples_per_channel == 5);
    CHECK(crossdsd_dsd_to_dxd_stream_process(
              stream, &packed, 1, 1, 8, output, 2, &produced,
              nullptr) == CROSSDSD_ERROR_INVALID_ARGUMENT);
    crossdsd_dsd_to_dxd_stream_destroy(stream);
}

}  // namespace

int main() {
    test_fragmentation_and_channels();
    test_partial_byte_seals_stream();
    if (failures) {
        std::cerr << failures << " streaming decoder test(s) failed\n";
        return 1;
    }
    std::cout << "All CrossDSD streaming decoder tests passed\n";
    return 0;
}
