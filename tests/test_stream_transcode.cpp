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

#include "crossdsd/convert.h"
#include "crossdsd/stream.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
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

std::vector<std::vector<uint8_t>> transcode(
    uint32_t input_rate, uint32_t output_rate,
    crossdsd_encoder_profile profile, uint32_t channels,
    const std::vector<std::vector<uint8_t>>& input,
    const std::vector<size_t>& chunks,
    crossdsd_dsd_to_dsd_stream_info* final_info = nullptr) {
    crossdsd_dsd_to_dsd_stream_config config{};
    config.struct_size = sizeof(config);
    config.input_sample_rate_hz = input_rate;
    config.output_sample_rate_hz = output_rate;
    config.channel_count = channels;
    config.filter_profile = CROSSDSD_FILTER_STANDARD;
    config.encoder_profile = profile;
    crossdsd_dsd_to_dsd_stream* stream = nullptr;
    CHECK(crossdsd_dsd_to_dsd_stream_create(
              &config, &stream, nullptr) == CROSSDSD_OK);
    if (!stream) return {};

    std::vector<std::vector<uint8_t>> result(channels);
    const uint64_t decimation = input_rate / 352'800u;
    const uint64_t output_bytes_per_dxd = output_rate / 352'800u / 8u;
    size_t position = 0;
    size_t chunk_index = 0;
    while (position < input[0].size()) {
        const size_t count = std::min(
            chunks[chunk_index++ % chunks.size()],
            input[0].size() - position);
        std::vector<uint8_t> input_block(count * channels);
        for (uint32_t channel = 0; channel < channels; ++channel) {
            std::copy_n(
                input[channel].data() + position, count,
                input_block.data() + static_cast<size_t>(channel) * count);
        }
        const uint64_t dxd_capacity = count * 8u / decimation + 1u;
        const uint64_t capacity =
            dxd_capacity * output_bytes_per_dxd + 1u;
        std::vector<uint8_t> output(capacity * channels);
        uint64_t produced = 0;
        CHECK(crossdsd_dsd_to_dsd_stream_process(
                  stream, input_block.data(), count, count, 8,
                  output.data(), capacity, capacity, &produced,
                  nullptr) == CROSSDSD_OK);
        for (uint32_t channel = 0; channel < channels; ++channel) {
            result[channel].insert(
                result[channel].end(),
                output.data() + static_cast<uint64_t>(channel) * capacity,
                output.data() + static_cast<uint64_t>(channel) * capacity +
                    produced);
        }
        position += count;
    }

    crossdsd_dsd_to_dsd_stream_info info{};
    info.struct_size = sizeof(info);
    CHECK(crossdsd_dsd_to_dsd_stream_get_info(stream, &info) == CROSSDSD_OK);
    const uint64_t expected =
        info.reconstructed_dxd_frames * output_bytes_per_dxd;
    CHECK(info.output_packed_bytes_per_channel <= expected);
    const uint64_t remaining =
        expected - info.output_packed_bytes_per_channel;
    std::vector<uint8_t> final_output(
        static_cast<size_t>(remaining * channels));
    uint64_t final_bytes = 0;
    CHECK(crossdsd_dsd_to_dsd_stream_finish(
              stream, remaining == 0 ? nullptr : final_output.data(),
              remaining, remaining, &final_bytes, nullptr) == CROSSDSD_OK);
    CHECK(final_bytes == remaining);
    if (remaining != 0) {
        for (uint32_t channel = 0; channel < channels; ++channel) {
            result[channel].insert(
                result[channel].end(),
                final_output.data() +
                    static_cast<uint64_t>(channel) * remaining,
                final_output.data() +
                    static_cast<uint64_t>(channel + 1u) * remaining);
        }
    }
    info.struct_size = sizeof(info);
    CHECK(crossdsd_dsd_to_dsd_stream_get_info(stream, &info) == CROSSDSD_OK);
    CHECK(info.input_dsd_samples_per_channel == input[0].size() * 8u);
    CHECK(info.output_packed_bytes_per_channel == expected);
    CHECK(info.finished == 1);
    CHECK(info.stability_failure == 0);
    CHECK(info.channel_reconstructed_peaks[0] > 0.0);
    for (const auto& channel : result) CHECK(channel.size() == expected);
    crossdsd_modulator_channel_metrics metrics{};
    metrics.struct_size = sizeof(metrics);
    CHECK(crossdsd_dsd_to_dsd_stream_get_channel_metrics(
              stream, 0, &metrics) == CROSSDSD_OK);
    CHECK(metrics.one_density > 0.0 && metrics.one_density < 1.0);
    if (final_info) *final_info = info;
    crossdsd_dsd_to_dsd_stream_destroy(stream);
    return result;
}

std::vector<std::vector<uint8_t>> source(uint32_t channels, size_t bytes) {
    std::vector<std::vector<uint8_t>> result(
        channels, std::vector<uint8_t>(bytes));
    uint32_t state = 0x31415926u;
    for (uint32_t channel = 0; channel < channels; ++channel) {
        for (auto& value : result[channel]) {
            state = state * 1664525u + 1013904223u;
            value = static_cast<uint8_t>(state >> 24u);
        }
    }
    return result;
}

std::filesystem::path temporary(const char* suffix) {
    const auto value =
        std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("crossdsd-stream-transcode-" + std::to_string(value) + suffix);
}

std::string utf8(const std::filesystem::path& path) {
#if defined(_WIN32)
    const auto encoded = path.u8string();
    return std::string(
        reinterpret_cast<const char*>(encoded.data()), encoded.size());
#else
    return path.string();
#endif
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

void test_nine_rate_pairs_and_fragmentation() {
    constexpr uint32_t channels = 2;
    const auto input = source(channels, 4097);
    for (uint32_t input_rate :
         {2'822'400u, 5'644'800u, 11'289'600u, 22'579'200u}) {
        for (uint32_t output_rate :
             {2'822'400u, 5'644'800u, 11'289'600u, 22'579'200u}) {
            const auto whole = transcode(
                input_rate, output_rate, CROSSDSD_ENCODER_STANDARD,
                channels, input, {input[0].size()});
            const auto fragmented = transcode(
                input_rate, output_rate, CROSSDSD_ENCODER_STANDARD,
                channels, input, {1, 7, 31, 997});
            check_equal(whole, fragmented);
        }
    }
}

void test_enhanced_and_multichannel() {
    const auto mono = source(1, 8193);
    crossdsd_dsd_to_dsd_stream_info info{};
    const auto whole = transcode(
        2'822'400u, 2'822'400u, CROSSDSD_ENCODER_DSD64_ENHANCED,
        1, mono, {mono[0].size()}, &info);
    const auto fragmented = transcode(
        2'822'400u, 2'822'400u, CROSSDSD_ENCODER_DSD64_ENHANCED,
        1, mono, {1, 7, 31, 997});
    check_equal(whole, fragmented);
    CHECK(info.encoder_profile == CROSSDSD_ENCODER_DSD64_ENHANCED);

    const auto five_channels = source(5, 1025);
    const auto five_whole = transcode(
        2'822'400u, 5'644'800u, CROSSDSD_ENCODER_STANDARD,
        5, five_channels, {five_channels[0].size()});
    const auto five_fragmented = transcode(
        2'822'400u, 5'644'800u, CROSSDSD_ENCODER_STANDARD,
        5, five_channels, {3, 17, 257});
    check_equal(five_whole, five_fragmented);
}

void test_partial_input_seals_stream() {
    crossdsd_dsd_to_dsd_stream_config config{};
    config.struct_size = sizeof(config);
    config.input_sample_rate_hz = 2'822'400u;
    config.output_sample_rate_hz = 2'822'400u;
    config.channel_count = 1;
    config.filter_profile = CROSSDSD_FILTER_STANDARD;
    config.encoder_profile = CROSSDSD_ENCODER_STANDARD;
    crossdsd_dsd_to_dsd_stream* stream = nullptr;
    CHECK(crossdsd_dsd_to_dsd_stream_create(
              &config, &stream, nullptr) == CROSSDSD_OK);
    const uint8_t input = 0x15;
    uint8_t output[4]{};
    uint64_t produced = 0;
    CHECK(crossdsd_dsd_to_dsd_stream_process(
              stream, &input, 1, 1, 5, output, 4, 4, &produced,
              nullptr) == CROSSDSD_OK);
    CHECK(crossdsd_dsd_to_dsd_stream_process(
              stream, &input, 1, 1, 8, output, 4, 4, &produced,
              nullptr) == CROSSDSD_ERROR_INVALID_ARGUMENT);
    crossdsd_dsd_to_dsd_stream_destroy(stream);
}

void test_file_engine_equivalence() {
    constexpr uint32_t channels = 2;
    const auto input = source(channels, 4097);
    std::vector<uint8_t> input_planar(input[0].size() * channels);
    for (uint32_t channel = 0; channel < channels; ++channel) {
        std::copy(
            input[channel].begin(), input[channel].end(),
            input_planar.begin() +
                static_cast<ptrdiff_t>(channel * input[0].size()));
    }
    const auto source_path = temporary("-source.dsf");
    const auto output_path = temporary("-output.dsf");
    const auto source_utf8 = utf8(source_path);
    const auto output_utf8 = utf8(output_path);

    crossdsd_dsf_writer_config writer_config{};
    writer_config.struct_size = sizeof(writer_config);
    writer_config.sample_rate_hz = 2'822'400u;
    writer_config.channel_count = channels;
    writer_config.channel_type = 2;
    crossdsd_dsf_writer* writer = nullptr;
    CHECK(crossdsd_dsf_writer_open_file(
              source_utf8.c_str(), &writer_config, &writer,
              nullptr) == CROSSDSD_OK);
    CHECK(crossdsd_dsf_writer_write_packed(
              writer, input_planar.data(), input[0].size(), 8,
              nullptr) == CROSSDSD_OK);
    CHECK(crossdsd_dsf_writer_close(writer, nullptr) == CROSSDSD_OK);

    crossdsd_transcode_file_config file_config{};
    file_config.struct_size = sizeof(file_config);
    file_config.dsd_rate = CROSSDSD_DSD128;
    file_config.encoder_profile = CROSSDSD_ENCODER_STANDARD;
    file_config.packed_chunk_bytes_per_channel = input[0].size();
    crossdsd_transcode_file_report report{};
    report.struct_size = sizeof(report);
    CHECK(crossdsd_transcode_file(
              source_utf8.c_str(), output_utf8.c_str(), &file_config,
              &report, nullptr) == CROSSDSD_OK);

    crossdsd_reader* reader = nullptr;
    CHECK(crossdsd_reader_open_file(
              output_utf8.c_str(), &reader, nullptr) == CROSSDSD_OK);
    crossdsd_stream_info file_info{};
    file_info.struct_size = sizeof(file_info);
    CHECK(crossdsd_reader_get_info(reader, &file_info) == CROSSDSD_OK);
    const uint64_t file_bytes =
        (file_info.sample_count_per_channel + 7u) / 8u;
    std::vector<uint8_t> file_output(file_bytes * channels);
    uint64_t bytes_read = 0;
    uint8_t valid_bits = 0;
    CHECK(crossdsd_reader_read_packed(
              reader, file_output.data(), file_bytes, &bytes_read,
              &valid_bits, nullptr) == CROSSDSD_OK);
    CHECK(bytes_read == file_bytes);
    CHECK(valid_bits == 8);
    crossdsd_reader_close(reader);

    const auto streamed = transcode(
        2'822'400u, 5'644'800u, CROSSDSD_ENCODER_STANDARD,
        channels, input, {input[0].size()});
    CHECK(streamed[0].size() == file_bytes);
    for (uint32_t channel = 0; channel < channels; ++channel) {
        CHECK(std::equal(
            streamed[channel].begin(), streamed[channel].end(),
            file_output.begin() +
                static_cast<ptrdiff_t>(channel * file_bytes)));
    }
    std::filesystem::remove(source_path);
    std::filesystem::remove(output_path);
}

}  // namespace

int main() {
    test_nine_rate_pairs_and_fragmentation();
    test_enhanced_and_multichannel();
    test_partial_input_seals_stream();
    test_file_engine_equivalence();
    if (failures) {
        std::cerr << failures << " streaming transcoder test(s) failed\n";
        return 1;
    }
    std::cout << "All CrossDSD streaming transcoder tests passed\n";
    return 0;
}
