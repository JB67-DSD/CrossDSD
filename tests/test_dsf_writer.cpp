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

#include "crossdsd/formats.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

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

std::string path_utf8(const fs::path& path) {
#if defined(_WIN32)
    const auto encoded = path.u8string();
    return std::string(
        reinterpret_cast<const char*>(encoded.data()), encoded.size());
#else
    return path.string();
#endif
}

void test_progressive_roundtrip(const fs::path& directory) {
    const auto path = directory / "writer-roundtrip.dsf";
    const auto encoded = path_utf8(path);
    crossdsd_dsf_writer_config config{};
    config.struct_size = sizeof(config);
    config.sample_rate_hz = 2'822'400;
    config.channel_count = 2;
    config.block_size_per_channel = 4;
    crossdsd_dsf_writer* writer = nullptr;
    crossdsd_error error{};
    error.struct_size = sizeof(error);
    CHECK(crossdsd_dsf_writer_open_file(
              encoded.c_str(), &config, &writer, &error) == CROSSDSD_OK);
    CHECK(writer != nullptr);
    if (!writer) return;

    const std::array<uint8_t, 4> first{{0x11, 0x24, 0x51, 0x64}};
    CHECK(crossdsd_dsf_writer_write_packed(
              writer, first.data(), 2, 8, &error) == CROSSDSD_OK);
    const std::array<uint8_t, 2> last{{0x07, 0x03}};
    CHECK(crossdsd_dsf_writer_write_packed(
              writer, last.data(), 1, 3, &error) == CROSSDSD_OK);
    CHECK(crossdsd_dsf_writer_write_packed(
              writer, last.data(), 1, 3, &error) ==
          CROSSDSD_ERROR_INVALID_ARGUMENT);
    const std::array<uint8_t, 10> id3{{'I', 'D', '3', 4, 0, 0, 0, 0, 0, 0}};
    CHECK(crossdsd_dsf_writer_write_id3(
              writer, id3.data(), id3.size(), &error) == CROSSDSD_OK);
    crossdsd_dsf_writer_info writer_info{};
    writer_info.struct_size = sizeof(writer_info);
    CHECK(crossdsd_dsf_writer_get_info(writer, &writer_info) == CROSSDSD_OK);
    CHECK(writer_info.sample_count_per_channel == 19);
    CHECK(crossdsd_dsf_writer_close(writer, &error) == CROSSDSD_OK);

    crossdsd_reader* reader = nullptr;
    CHECK(crossdsd_reader_open_file(
              encoded.c_str(), &reader, &error) == CROSSDSD_OK);
    CHECK(reader != nullptr);
    if (!reader) return;
    crossdsd_stream_info info{};
    info.struct_size = sizeof(info);
    CHECK(crossdsd_reader_get_info(reader, &info) == CROSSDSD_OK);
    CHECK(info.sample_rate_hz == 2'822'400);
    CHECK(info.channel_count == 2);
    CHECK(info.channel_type == 2);
    CHECK(info.sample_count_per_channel == 19);
    CHECK(info.block_size_per_channel == 4);
    CHECK(info.padding_bits_per_channel == 13);
    CHECK(info.metadata_status == CROSSDSD_METADATA_ID3);

    std::array<uint8_t, 8> packed{};
    uint64_t bytes = 0;
    uint8_t valid = 0;
    CHECK(crossdsd_reader_read_packed(
              reader, packed.data(), 4, &bytes, &valid, &error) == CROSSDSD_OK);
    CHECK(bytes == 3);
    CHECK(valid == 3);
    CHECK(packed[0] == 0x11 && packed[1] == 0x24 && packed[2] == 0x07);
    CHECK(packed[4] == 0x51 && packed[5] == 0x64 && packed[6] == 0x03);
    std::array<uint8_t, 10> read_id3{};
    CHECK(crossdsd_reader_read_metadata(
              reader, read_id3.data(), read_id3.size(), &bytes, &error) ==
          CROSSDSD_OK);
    CHECK(bytes == id3.size());
    CHECK(read_id3 == id3);
    crossdsd_reader_close(reader);
}

void test_multiple_blocks_and_rates(const fs::path& directory) {
    const std::array<uint32_t, 3> rates{
        2'822'400, 5'644'800, 11'289'600};
    for (const uint32_t rate : rates) {
        const auto path = directory / ("writer-" + std::to_string(rate) + ".dsf");
        const auto encoded = path_utf8(path);
        crossdsd_dsf_writer_config config{};
        config.struct_size = sizeof(config);
        config.sample_rate_hz = rate;
        config.channel_count = 1;
        config.block_size_per_channel = 4;
        crossdsd_dsf_writer* writer = nullptr;
        CHECK(crossdsd_dsf_writer_open_file(
                  encoded.c_str(), &config, &writer, nullptr) == CROSSDSD_OK);
        std::array<uint8_t, 9> input{};
        for (size_t index = 0; index < input.size(); ++index) {
            input[index] = static_cast<uint8_t>(index * 17u + 3u);
        }
        CHECK(crossdsd_dsf_writer_write_packed(
                  writer, input.data(), input.size(), 8, nullptr) == CROSSDSD_OK);
        CHECK(crossdsd_dsf_writer_close(writer, nullptr) == CROSSDSD_OK);

        crossdsd_reader* reader = nullptr;
        CHECK(crossdsd_reader_open_file(
                  encoded.c_str(), &reader, nullptr) == CROSSDSD_OK);
        if (!reader) continue;
        crossdsd_stream_info info{};
        info.struct_size = sizeof(info);
        CHECK(crossdsd_reader_get_info(reader, &info) == CROSSDSD_OK);
        CHECK(info.sample_rate_hz == rate);
        CHECK(info.sample_count_per_channel == 72);
        CHECK(info.stored_bits_per_channel == 96);
        std::array<uint8_t, 12> output{};
        uint64_t bytes = 0;
        uint8_t valid = 0;
        CHECK(crossdsd_reader_read_packed(
                  reader, output.data(), output.size(), &bytes, &valid, nullptr) ==
              CROSSDSD_OK);
        CHECK(bytes == input.size() && valid == 8);
        CHECK(std::memcmp(output.data(), input.data(), input.size()) == 0);
        crossdsd_reader_close(reader);
    }
}

void test_multichannel_planar_layout(const fs::path& directory) {
    const auto path = directory / "writer-three-channel.dsf";
    const auto encoded = path_utf8(path);
    crossdsd_dsf_writer_config config{};
    config.struct_size = sizeof(config);
    config.sample_rate_hz = 2'822'400;
    config.channel_count = 3;
    config.channel_type = 3;
    config.block_size_per_channel = 4;
    crossdsd_dsf_writer* writer = nullptr;
    CHECK(crossdsd_dsf_writer_open_file(
              encoded.c_str(), &config, &writer, nullptr) == CROSSDSD_OK);
    const std::array<uint8_t, 15> input{{
        0x10, 0x11, 0x12, 0x13, 0x14,
        0x20, 0x21, 0x22, 0x23, 0x24,
        0x30, 0x31, 0x32, 0x33, 0x34,
    }};
    CHECK(crossdsd_dsf_writer_write_packed(
              writer, input.data(), 5, 8, nullptr) == CROSSDSD_OK);
    CHECK(crossdsd_dsf_writer_close(writer, nullptr) == CROSSDSD_OK);

    crossdsd_reader* reader = nullptr;
    CHECK(crossdsd_reader_open_file(
              encoded.c_str(), &reader, nullptr) == CROSSDSD_OK);
    if (!reader) return;
    crossdsd_stream_info info{};
    info.struct_size = sizeof(info);
    CHECK(crossdsd_reader_get_info(reader, &info) == CROSSDSD_OK);
    CHECK(info.channel_count == 3 && info.channel_type == 3);
    std::array<uint8_t, 24> output{};
    uint64_t bytes = 0;
    uint8_t valid = 0;
    CHECK(crossdsd_reader_read_packed(
              reader, output.data(), 8, &bytes, &valid, nullptr) == CROSSDSD_OK);
    CHECK(bytes == 5 && valid == 8);
    for (uint32_t channel = 0; channel < 3; ++channel) {
        CHECK(std::memcmp(
                  output.data() + channel * 8,
                  input.data() + channel * 5, 5) == 0);
    }
    crossdsd_reader_close(reader);
}

void test_rejections(const fs::path& directory) {
    const auto path = path_utf8(directory / "invalid.dsf");
    crossdsd_dsf_writer_config config{};
    config.struct_size = sizeof(config);
    config.sample_rate_hz = 352'800;
    config.channel_count = 2;
    crossdsd_dsf_writer* writer = nullptr;
    CHECK(crossdsd_dsf_writer_open_file(
              path.c_str(), &config, &writer, nullptr) ==
          CROSSDSD_ERROR_UNSUPPORTED_RATE);

    config.sample_rate_hz = 2'822'400;
    config.channel_count = 3;
    CHECK(crossdsd_dsf_writer_open_file(
              path.c_str(), &config, &writer, nullptr) ==
          CROSSDSD_ERROR_INVALID_ARGUMENT);

    config.channel_count = 1;
    CHECK(crossdsd_dsf_writer_open_file(
              path.c_str(), &config, &writer, nullptr) == CROSSDSD_OK);
    const uint8_t invalid_tail = 0xf8;
    CHECK(crossdsd_dsf_writer_write_packed(
              writer, &invalid_tail, 1, 3, nullptr) ==
          CROSSDSD_ERROR_INVALID_ARGUMENT);
    CHECK(crossdsd_dsf_writer_close(writer, nullptr) ==
          CROSSDSD_ERROR_INVALID_SAMPLE_COUNT);
}

}  // namespace

int main() {
    const fs::path directory =
        fs::temp_directory_path() /
        ("crossdsd-writer-tests-" + std::to_string(std::rand()));
    fs::create_directories(directory);
    test_progressive_roundtrip(directory);
    test_multiple_blocks_and_rates(directory);
    test_multichannel_planar_layout(directory);
    test_rejections(directory);
    fs::remove_all(directory);
    if (failures != 0) {
        std::cerr << failures << " DSF writer test(s) failed\n";
        return 1;
    }
    std::cout << "All CrossDSD DSF writer tests passed\n";
    return 0;
}
