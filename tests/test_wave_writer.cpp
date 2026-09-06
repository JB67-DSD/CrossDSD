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

#include "crossdsd/audio.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
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

uint32_t u32(const std::vector<uint8_t>& bytes, size_t offset) {
    return static_cast<uint32_t>(bytes[offset]) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 8u) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16u) |
           (static_cast<uint32_t>(bytes[offset + 3]) << 24u);
}

uint64_t u64(const std::vector<uint8_t>& bytes, size_t offset) {
    return static_cast<uint64_t>(u32(bytes, offset)) |
           (static_cast<uint64_t>(u32(bytes, offset + 4)) << 32u);
}

std::filesystem::path temporary(const char* suffix) {
    const auto value = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("crossdsd-wave-" + std::to_string(value) + suffix);
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

std::vector<uint8_t> read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::vector<uint8_t>(
        std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

void test_riff_float_and_no_clipping() {
    const auto path = temporary(".wav");
    crossdsd_wave_writer_config config{};
    config.struct_size = sizeof(config);
    config.sample_rate_hz = CROSSDSD_DXD_SAMPLE_RATE;
    config.channel_count = 2;
    config.expected_frame_count = 3;
    config.container = CROSSDSD_WAVE_AUTO;
    crossdsd_wave_writer* writer = nullptr;
    crossdsd_error error{};
    error.struct_size = sizeof(error);
    const auto path_utf8 = utf8(path);
    CHECK(crossdsd_wave_writer_open_file(
              path_utf8.c_str(), &config, &writer, &error) == CROSSDSD_OK);
    const std::vector<float> samples = {
        1.25f, -1.5f, 0.0f, -0.0f, 3.0f, -4.0f,
    };
    CHECK(crossdsd_wave_writer_write(
              writer, samples.data(), 3, &error) == CROSSDSD_OK);
    const uint8_t id3[] = {'I', 'D', '3', 4, 0};
    CHECK(crossdsd_wave_writer_write_id3(
              writer, id3, sizeof(id3), &error) == CROSSDSD_OK);
    crossdsd_wave_writer_info info{};
    info.struct_size = sizeof(info);
    CHECK(crossdsd_wave_writer_get_info(writer, &info) == CROSSDSD_OK);
    CHECK(info.container == CROSSDSD_WAVE_RIFF);
    CHECK(info.frame_count == 3);
    CHECK(info.data_size_bytes == samples.size() * sizeof(float));
    CHECK(info.metadata_size_bytes == 8u + sizeof(id3) + 1u);
    CHECK(crossdsd_wave_writer_close(writer, &error) == CROSSDSD_OK);

    const auto bytes = read(path);
    CHECK(std::string(bytes.begin(), bytes.begin() + 4) == "RIFF");
    CHECK(std::string(bytes.begin() + 8, bytes.begin() + 12) == "WAVE");
    CHECK(u32(bytes, 4) == bytes.size() - 8u);
    CHECK(u32(bytes, 16) == 16u);
    CHECK(u32(bytes, 24) == CROSSDSD_DXD_SAMPLE_RATE);
    CHECK(u32(bytes, 44) == 3u);
    CHECK(u32(bytes, 52) == samples.size() * sizeof(float));
    for (size_t index = 0; index < samples.size(); ++index) {
        CHECK(u32(bytes, 56u + index * 4u) ==
              std::bit_cast<uint32_t>(samples[index]));
    }
    const size_t id3_offset = 56u + samples.size() * sizeof(float);
    CHECK(std::string(
              bytes.begin() + static_cast<ptrdiff_t>(id3_offset),
              bytes.begin() + static_cast<ptrdiff_t>(id3_offset + 4u)) ==
          "id3 ");
    CHECK(u32(bytes, id3_offset + 4u) == sizeof(id3));
    CHECK(std::equal(
        std::begin(id3), std::end(id3),
        bytes.begin() + static_cast<ptrdiff_t>(id3_offset + 8u)));

    crossdsd_wave_reader* reader = nullptr;
    CHECK(crossdsd_wave_reader_open_file(
              path_utf8.c_str(), &reader, &error) == CROSSDSD_OK);
    crossdsd_wave_reader_info reader_info{};
    reader_info.struct_size = sizeof(reader_info);
    CHECK(crossdsd_wave_reader_get_info(reader, &reader_info) == CROSSDSD_OK);
    CHECK(reader_info.container == CROSSDSD_WAVE_RIFF);
    CHECK(reader_info.sample_rate_hz == CROSSDSD_DXD_SAMPLE_RATE);
    CHECK(reader_info.channel_count == 2);
    CHECK(reader_info.frame_count == 3);
    CHECK(reader_info.metadata_status == CROSSDSD_METADATA_ID3);
    std::vector<float> read_samples(samples.size());
    uint64_t frames_read = 0;
    CHECK(crossdsd_wave_reader_read(
              reader, read_samples.data(), 3, &frames_read, &error) ==
          CROSSDSD_OK);
    CHECK(frames_read == 3);
    for (size_t index = 0; index < samples.size(); ++index) {
        CHECK(std::bit_cast<uint32_t>(read_samples[index]) ==
              std::bit_cast<uint32_t>(samples[index]));
    }
    CHECK(crossdsd_wave_reader_read(
              reader, read_samples.data(), 3, &frames_read, &error) ==
          CROSSDSD_ERROR_END_OF_STREAM);
    std::vector<uint8_t> read_id3(sizeof(id3));
    uint64_t metadata_read = 0;
    CHECK(crossdsd_wave_reader_read_metadata(
              reader, read_id3.data(), read_id3.size(), &metadata_read,
              &error) == CROSSDSD_OK);
    CHECK(metadata_read == sizeof(id3));
    CHECK(std::equal(
        std::begin(id3), std::end(id3), read_id3.begin()));
    crossdsd_wave_reader_close(reader);
    std::filesystem::remove(path);
}

void test_forced_rf64() {
    const auto path = temporary(".wav");
    crossdsd_wave_writer_config config{};
    config.struct_size = sizeof(config);
    config.sample_rate_hz = CROSSDSD_DXD_SAMPLE_RATE;
    config.channel_count = 1;
    config.expected_frame_count = 2;
    config.container = CROSSDSD_WAVE_RF64;
    crossdsd_wave_writer* writer = nullptr;
    crossdsd_error error{};
    error.struct_size = sizeof(error);
    const auto path_utf8 = utf8(path);
    CHECK(crossdsd_wave_writer_open_file(
              path_utf8.c_str(), &config, &writer, &error) == CROSSDSD_OK);
    const float samples[] = {0.5f, -2.0f};
    CHECK(crossdsd_wave_writer_write(writer, samples, 2, &error) == CROSSDSD_OK);
    CHECK(crossdsd_wave_writer_close(writer, &error) == CROSSDSD_OK);

    const auto bytes = read(path);
    CHECK(std::string(bytes.begin(), bytes.begin() + 4) == "RF64");
    CHECK(u32(bytes, 4) == UINT32_MAX);
    CHECK(std::string(bytes.begin() + 12, bytes.begin() + 16) == "ds64");
    CHECK(u64(bytes, 20) == bytes.size() - 8u);
    CHECK(u64(bytes, 28) == sizeof(samples));
    CHECK(u64(bytes, 36) == 2u);
    CHECK(std::string(bytes.begin() + 84, bytes.begin() + 88) == "data");
    CHECK(u32(bytes, 88) == UINT32_MAX);
    crossdsd_wave_reader* reader = nullptr;
    CHECK(crossdsd_wave_reader_open_file(
              path_utf8.c_str(), &reader, &error) == CROSSDSD_OK);
    crossdsd_wave_reader_info reader_info{};
    reader_info.struct_size = sizeof(reader_info);
    CHECK(crossdsd_wave_reader_get_info(reader, &reader_info) == CROSSDSD_OK);
    CHECK(reader_info.container == CROSSDSD_WAVE_RF64);
    CHECK(reader_info.frame_count == 2);
    float read_samples[2]{};
    uint64_t frames_read = 0;
    CHECK(crossdsd_wave_reader_read(
              reader, read_samples, 2, &frames_read, &error) == CROSSDSD_OK);
    CHECK(frames_read == 2);
    CHECK(std::bit_cast<uint32_t>(read_samples[0]) ==
          std::bit_cast<uint32_t>(samples[0]));
    CHECK(std::bit_cast<uint32_t>(read_samples[1]) ==
          std::bit_cast<uint32_t>(samples[1]));
    crossdsd_wave_reader_close(reader);
    std::filesystem::remove(path);
}

void test_auto_selects_rf64() {
    const auto path = temporary(".wav");
    crossdsd_wave_writer_config config{};
    config.struct_size = sizeof(config);
    config.sample_rate_hz = CROSSDSD_DXD_SAMPLE_RATE;
    config.channel_count = 2;
    config.expected_frame_count = UINT32_MAX;
    config.container = CROSSDSD_WAVE_AUTO;
    crossdsd_wave_writer* writer = nullptr;
    const auto path_utf8 = utf8(path);
    CHECK(crossdsd_wave_writer_open_file(
              path_utf8.c_str(), &config, &writer, nullptr) == CROSSDSD_OK);
    crossdsd_wave_writer_info info{};
    info.struct_size = sizeof(info);
    CHECK(crossdsd_wave_writer_get_info(writer, &info) == CROSSDSD_OK);
    CHECK(info.container == CROSSDSD_WAVE_RF64);
    CHECK(crossdsd_wave_writer_close(writer, nullptr) == CROSSDSD_OK);
    std::filesystem::remove(path);
}

void test_sony_dsf_channel_layouts_round_trip() {
    constexpr uint32_t counts[] = {0, 1, 2, 3, 4, 4, 5, 6};
    constexpr uint32_t masks[] = {
        0, 0x0004, 0x0003, 0x0007, 0x0033, 0x000f, 0x0037, 0x003f};
    for (uint32_t type = 1; type <= 7; ++type) {
        const auto path = temporary("-layout.wav");
        crossdsd_wave_writer_config config{};
        config.struct_size = sizeof(config);
        config.sample_rate_hz = CROSSDSD_DXD_SAMPLE_RATE;
        config.channel_count = counts[type];
        config.expected_frame_count = 1;
        config.container = CROSSDSD_WAVE_RIFF;
        config.channel_type = type;
        crossdsd_wave_writer* writer = nullptr;
        crossdsd_error error{};
        error.struct_size = sizeof(error);
        const auto path_utf8 = utf8(path);
        CHECK(crossdsd_wave_writer_open_file(
                  path_utf8.c_str(), &config, &writer, &error) == CROSSDSD_OK);
        std::vector<float> samples(counts[type], 0.0f);
        CHECK(crossdsd_wave_writer_write(
                  writer, samples.data(), 1, &error) == CROSSDSD_OK);
        CHECK(crossdsd_wave_writer_close(writer, &error) == CROSSDSD_OK);

        const auto bytes = read(path);
        CHECK(u32(bytes, 16) == 40u);
        CHECK((bytes[20] | (static_cast<uint16_t>(bytes[21]) << 8u)) ==
              0xfffeu);
        CHECK(u32(bytes, 40) == masks[type]);

        crossdsd_wave_reader* reader = nullptr;
        CHECK(crossdsd_wave_reader_open_file(
                  path_utf8.c_str(), &reader, &error) == CROSSDSD_OK);
        crossdsd_wave_reader_info info{};
        info.struct_size = sizeof(info);
        CHECK(crossdsd_wave_reader_get_info(reader, &info) == CROSSDSD_OK);
        CHECK(info.channel_count == counts[type]);
        CHECK(info.channel_type == type);
        CHECK(info.channel_mask == masks[type]);
        crossdsd_wave_reader_close(reader);
        std::filesystem::remove(path);
    }
}

void test_reader_rejects_integer_pcm() {
    const auto path = temporary("-integer.wav");
    const std::vector<uint8_t> bytes = {
        'R', 'I', 'F', 'F', 40, 0, 0, 0, 'W', 'A', 'V', 'E',
        'f', 'm', 't', ' ', 16, 0, 0, 0,
        1, 0, 1, 0,
        0x44, 0xac, 0, 0,
        0x88, 0x58, 1, 0,
        2, 0, 16, 0,
        'd', 'a', 't', 'a', 4, 0, 0, 0,
        0, 0, 0, 0,
    };
    {
        std::ofstream stream(path, std::ios::binary);
        stream.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }
    const auto path_utf8 = utf8(path);
    crossdsd_wave_reader* reader = nullptr;
    CHECK(crossdsd_wave_reader_open_file(
              path_utf8.c_str(), &reader, nullptr) ==
          CROSSDSD_ERROR_UNSUPPORTED_FORMAT);
    CHECK(reader == nullptr);
    std::filesystem::remove(path);
}

}  // namespace

int main() {
    test_riff_float_and_no_clipping();
    test_forced_rf64();
    test_auto_selects_rf64();
    test_sony_dsf_channel_layouts_round_trip();
    test_reader_rejects_integer_pcm();
    if (failures) {
        std::cerr << failures << " WAVE writer test(s) failed\n";
        return 1;
    }
    std::cout << "All CrossDSD WAVE writer tests passed\n";
    return 0;
}
