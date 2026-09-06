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
#include <cmath>
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

std::vector<std::vector<uint8_t>> source(uint32_t channels, size_t bytes) {
    std::vector<std::vector<uint8_t>> result(
        channels, std::vector<uint8_t>(bytes));
    uint32_t state = 0x27182818u;
    for (uint32_t channel = 0; channel < channels; ++channel) {
        for (auto& value : result[channel]) {
            state = state * 1664525u + 1013904223u;
            value = static_cast<uint8_t>(state >> 24u);
        }
    }
    return result;
}

crossdsd_dsd_peak_stream_info analyze(
    uint32_t rate, uint32_t channels,
    const std::vector<std::vector<uint8_t>>& input,
    const std::vector<size_t>& chunks) {
    crossdsd_dsd_peak_stream_config config{};
    config.struct_size = sizeof(config);
    config.input_sample_rate_hz = rate;
    config.channel_count = channels;
    config.filter_profile = CROSSDSD_FILTER_STANDARD;
    crossdsd_dsd_peak_stream* stream = nullptr;
    CHECK(crossdsd_dsd_peak_stream_create(
              &config, &stream, nullptr) == CROSSDSD_OK);
    size_t position = 0;
    size_t chunk_index = 0;
    while (stream && position < input[0].size()) {
        const size_t count = std::min(
            chunks[chunk_index++ % chunks.size()],
            input[0].size() - position);
        std::vector<uint8_t> block(count * channels);
        for (uint32_t channel = 0; channel < channels; ++channel) {
            std::copy_n(
                input[channel].data() + position, count,
                block.data() + static_cast<size_t>(channel) * count);
        }
        CHECK(crossdsd_dsd_peak_stream_process(
                  stream, block.data(), count, count, 8,
                  nullptr) == CROSSDSD_OK);
        position += count;
    }
    CHECK(crossdsd_dsd_peak_stream_finish(stream, nullptr) == CROSSDSD_OK);
    crossdsd_dsd_peak_stream_info info{};
    info.struct_size = sizeof(info);
    CHECK(crossdsd_dsd_peak_stream_get_info(stream, &info) == CROSSDSD_OK);
    CHECK(info.finished == 1);
    CHECK(info.input_sealed == 1);
    CHECK(info.input_dsd_samples_per_channel == input[0].size() * 8u);
    CHECK(info.reconstructed_peak > 0.0);
    CHECK(std::isfinite(info.reconstructed_peak_dbfs));
    crossdsd_dsd_peak_stream_destroy(stream);
    return info;
}

std::filesystem::path temporary(const char* suffix) {
    const auto value =
        std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("crossdsd-stream-peak-" + std::to_string(value) + suffix);
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

void test_fragmentation_and_rates() {
    constexpr uint32_t channels = 2;
    const auto input = source(channels, 10'003);
    for (uint32_t rate :
         {2'822'400u, 5'644'800u, 11'289'600u, 22'579'200u}) {
        const auto whole = analyze(
            rate, channels, input, {input[0].size()});
        const auto fragmented = analyze(
            rate, channels, input, {1, 7, 31, 997});
        CHECK(whole.reconstructed_frames == fragmented.reconstructed_frames);
        CHECK(whole.reconstructed_peak == fragmented.reconstructed_peak);
        CHECK(whole.reconstructed_peak_dbfs ==
              fragmented.reconstructed_peak_dbfs);
        for (uint32_t channel = 0; channel < channels; ++channel) {
            CHECK(whole.channel_reconstructed_peaks[channel] ==
                  fragmented.channel_reconstructed_peaks[channel]);
        }
    }
}

void test_file_engine_equivalence() {
    constexpr uint32_t channels = 2;
    const auto input = source(channels, 4097);
    std::vector<uint8_t> planar(input[0].size() * channels);
    for (uint32_t channel = 0; channel < channels; ++channel) {
        std::copy(
            input[channel].begin(), input[channel].end(),
            planar.begin() +
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
              writer, planar.data(), input[0].size(), 8,
              nullptr) == CROSSDSD_OK);
    CHECK(crossdsd_dsf_writer_close(writer, nullptr) == CROSSDSD_OK);

    crossdsd_transcode_file_config config{};
    config.struct_size = sizeof(config);
    config.dsd_rate = CROSSDSD_DSD64;
    config.encoder_profile = CROSSDSD_ENCODER_STANDARD;
    config.packed_chunk_bytes_per_channel = input[0].size();
    crossdsd_transcode_file_report report{};
    report.struct_size = sizeof(report);
    CHECK(crossdsd_transcode_file(
              source_utf8.c_str(), output_utf8.c_str(), &config,
              &report, nullptr) == CROSSDSD_OK);

    const auto streamed = analyze(
        2'822'400u, channels, input, {1, 7, 31, 997});
    CHECK(streamed.reconstructed_frames == report.reconstructed_frames);
    CHECK(streamed.reconstructed_peak == report.reconstructed_peak);
    CHECK(streamed.reconstructed_peak_dbfs ==
          report.reconstructed_peak_dbfs);
    for (uint32_t channel = 0; channel < channels; ++channel) {
        CHECK(streamed.channel_reconstructed_peaks[channel] ==
              report.channel_reconstructed_peaks[channel]);
    }
    std::filesystem::remove(source_path);
    std::filesystem::remove(output_path);
}

void test_silence_initial_state() {
    crossdsd_dsd_peak_stream_config config{};
    config.struct_size = sizeof(config);
    config.input_sample_rate_hz = 2'822'400u;
    config.channel_count = 1;
    config.filter_profile = CROSSDSD_FILTER_STANDARD;
    crossdsd_dsd_peak_stream* stream = nullptr;
    CHECK(crossdsd_dsd_peak_stream_create(
              &config, &stream, nullptr) == CROSSDSD_OK);
    crossdsd_dsd_peak_stream_info info{};
    info.struct_size = sizeof(info);
    CHECK(crossdsd_dsd_peak_stream_get_info(stream, &info) == CROSSDSD_OK);
    CHECK(info.reconstructed_peak == 0.0);
    CHECK(std::isinf(info.reconstructed_peak_dbfs));
    CHECK(info.reconstructed_peak_dbfs < 0.0);
    crossdsd_dsd_peak_stream_destroy(stream);
}

}  // namespace

int main() {
    test_fragmentation_and_rates();
    test_file_engine_equivalence();
    test_silence_initial_state();
    if (failures) {
        std::cerr << failures << " streaming peak test(s) failed\n";
        return 1;
    }
    std::cout << "All CrossDSD streaming peak tests passed\n";
    return 0;
}
