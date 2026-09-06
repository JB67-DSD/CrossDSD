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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
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

void put32(std::vector<uint8_t>& data, size_t offset, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) data[offset + i] = static_cast<uint8_t>(value >> (8u * i));
}

void put64(std::vector<uint8_t>& data, size_t offset, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) data[offset + i] = static_cast<uint8_t>(value >> (8u * i));
}

struct FixtureOptions {
    uint32_t channels = 2;
    uint32_t rate = 2'822'400;
    uint64_t samples = 19;
    uint32_t block_size = 4;
    bool metadata = true;
    uint32_t extra_block_groups = 0;
};

std::vector<uint8_t> make_dsf(const FixtureOptions& options) {
    const uint64_t valid_bytes = (options.samples + 7u) / 8u;
    const uint64_t stored_bytes =
        ((valid_bytes + options.block_size - 1u) / options.block_size) * options.block_size;
    const uint64_t payload =
        (stored_bytes + uint64_t(options.extra_block_groups) * options.block_size) *
        options.channels;
    const std::array<uint8_t, 10> id3{{'I', 'D', '3', 4, 0, 0, 0, 0, 0, 0}};
    const uint64_t metadata_offset = 92u + payload;
    const uint64_t file_size = metadata_offset + (options.metadata ? id3.size() : 0u);
    std::vector<uint8_t> data(static_cast<size_t>(file_size), 0);
    std::memcpy(data.data(), "DSD ", 4);
    put64(data, 4, 28);
    put64(data, 12, file_size);
    put64(data, 20, options.metadata ? metadata_offset : 0);
    std::memcpy(data.data() + 28, "fmt ", 4);
    put64(data, 32, 52);
    put32(data, 40, 1);
    put32(data, 44, 0);
    put32(data, 48, options.channels == 1 ? 1 : 2);
    put32(data, 52, options.channels);
    put32(data, 56, options.rate);
    put32(data, 60, 1);
    put64(data, 64, options.samples);
    put32(data, 72, options.block_size);
    std::memcpy(data.data() + 80, "data", 4);
    put64(data, 84, payload + 12u);

    for (uint64_t logical = 0; logical < valid_bytes; ++logical) {
        const uint64_t block = logical / options.block_size;
        const uint64_t within = logical % options.block_size;
        for (uint64_t channel = 0; channel < options.channels; ++channel) {
            const uint64_t physical =
                92u + block * options.block_size * options.channels +
                channel * options.block_size + within;
            data[physical] = static_cast<uint8_t>(0x11u + channel * 0x40u + logical * 0x13u);
        }
    }
    if (options.metadata) {
        std::memcpy(data.data() + metadata_offset, id3.data(), id3.size());
    }
    return data;
}

fs::path write_fixture(const fs::path& directory, const std::string& name,
                       const std::vector<uint8_t>& data) {
    const fs::path path = directory / name;
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(data.data()),
                 static_cast<std::streamsize>(data.size()));
    return path;
}

crossdsd_result open(const fs::path& path, crossdsd_reader** reader,
                     crossdsd_error* error = nullptr) {
    crossdsd_error local{};
    local.struct_size = sizeof(local);
#if defined(_WIN32)
    const auto encoded = path.u8string();
    const std::string utf8(
        reinterpret_cast<const char*>(encoded.data()), encoded.size());
#else
    const std::string utf8 = path.string();
#endif
    return crossdsd_reader_open_file(
        utf8.c_str(), reader, error ? error : &local);
}

void test_valid_and_progressive(const fs::path& directory) {
    const auto path = write_fixture(directory, "stereo.dsf", make_dsf({}));
    crossdsd_reader* reader = nullptr;
    CHECK(open(path, &reader) == CROSSDSD_OK);
    CHECK(reader != nullptr);
    if (!reader) return;
    crossdsd_stream_info info{};
    info.struct_size = sizeof(info);
    CHECK(crossdsd_reader_get_info(reader, &info) == CROSSDSD_OK);
    CHECK(info.channel_count == 2);
    CHECK(info.dsd_rate == CROSSDSD_DSD64);
    CHECK(info.sample_count_per_channel == 19);
    CHECK(info.padding_bits_per_channel == 13);
    CHECK(info.metadata_status == CROSSDSD_METADATA_ID3);

    std::array<uint8_t, 4> first{};
    uint64_t bytes = 0;
    uint8_t valid_bits = 0;
    CHECK(crossdsd_reader_read_packed(reader, first.data(), 2, &bytes, &valid_bits, nullptr)
          == CROSSDSD_OK);
    CHECK(bytes == 2 && valid_bits == 8);
    CHECK(first[0] == 0x11 && first[1] == 0x24);
    CHECK(first[2] == 0x51 && first[3] == 0x64);

    std::array<uint8_t, 4> last{};
    CHECK(crossdsd_reader_read_packed(reader, last.data(), 2, &bytes, &valid_bits, nullptr)
          == CROSSDSD_OK);
    CHECK(bytes == 1 && valid_bits == 3);
    CHECK(last[0] == (0x37 & 0x07));
    CHECK(last[2] == (0x77 & 0x07));
    CHECK(crossdsd_reader_read_packed(reader, last.data(), 2, &bytes, &valid_bits, nullptr)
          == CROSSDSD_ERROR_END_OF_STREAM);

    std::array<uint8_t, 10> metadata{};
    CHECK(crossdsd_reader_read_metadata(reader, metadata.data(), metadata.size(), &bytes, nullptr)
          == CROSSDSD_OK);
    CHECK(bytes == 10 && std::memcmp(metadata.data(), "ID3", 3) == 0);
    crossdsd_reader_close(reader);
}

void test_bit_expansion() {
    const std::array<uint8_t, 2> packed{{0b10010010, 0b00000001}};
    std::array<int8_t, 9> expanded{};
    CHECK(crossdsd_expand_dsd_lsb(
              packed.data(), 9, expanded.data(), expanded.size()) == CROSSDSD_OK);
    const std::array<int8_t, 9> expected{{-1, 1, -1, -1, 1, -1, -1, 1, 1}};
    CHECK(expanded == expected);
}

void test_padding_0_to_7(const fs::path& directory) {
    for (uint64_t remainder = 0; remainder < 8; ++remainder) {
        FixtureOptions options{};
        options.channels = 1;
        options.samples = remainder == 0 ? 16 : 8 + remainder;
        options.metadata = false;
        const auto path = write_fixture(
            directory, "padding-" + std::to_string(remainder) + ".dsf", make_dsf(options));
        crossdsd_reader* reader = nullptr;
        CHECK(open(path, &reader) == CROSSDSD_OK);
        if (!reader) continue;
        std::array<uint8_t, 8> output{};
        uint64_t bytes = 0;
        uint8_t valid = 0;
        CHECK(crossdsd_reader_read_packed(
                  reader, output.data(), output.size(), &bytes, &valid, nullptr) == CROSSDSD_OK);
        CHECK(valid == (remainder == 0 ? 8 : remainder));
        if (remainder != 0) {
            CHECK((output[bytes - 1] & static_cast<uint8_t>(~((1u << remainder) - 1u))) == 0);
        }
        crossdsd_reader_close(reader);
    }
}

void test_compatible_extra_padding_group(const fs::path& directory) {
    FixtureOptions options{};
    options.samples = 32;
    options.block_size = 4;
    options.extra_block_groups = 1;
    const auto path = write_fixture(
        directory, "one-extra-padding-group.dsf", make_dsf(options));
    crossdsd_reader* reader = nullptr;
    CHECK(open(path, &reader) == CROSSDSD_OK);
    if (reader) {
        crossdsd_stream_info info{};
        info.struct_size = sizeof(info);
        CHECK(crossdsd_reader_get_info(reader, &info) == CROSSDSD_OK);
        CHECK(info.sample_count_per_channel == 32);
        CHECK(info.audio_size == 16);
        CHECK(info.padding_bits_per_channel == 32);
        crossdsd_reader_close(reader);
    }

    options.extra_block_groups = 2;
    const auto oversized = write_fixture(
        directory, "two-extra-padding-groups.dsf", make_dsf(options));
    reader = nullptr;
    CHECK(open(oversized, &reader) == CROSSDSD_ERROR_INVALID_SAMPLE_COUNT);
    CHECK(reader == nullptr);
}

void test_rejections(const fs::path& directory) {
    struct Mutation {
        const char* name;
        size_t offset;
        uint8_t value;
        crossdsd_result expected;
    };
    const std::array<Mutation, 6> mutations{{
        {"signature", 0, 'X', CROSSDSD_ERROR_INVALID_SIGNATURE},
        {"fmt", 28, 'X', CROSSDSD_ERROR_INVALID_CHUNK},
        {"rate", 58, 0, CROSSDSD_ERROR_UNSUPPORTED_RATE},
        {"channels", 52, 0, CROSSDSD_ERROR_UNSUPPORTED_CHANNELS},
        {"bits", 60, 8, CROSSDSD_ERROR_UNSUPPORTED_FORMAT},
        {"data", 80, 'X', CROSSDSD_ERROR_INVALID_CHUNK},
    }};
    for (const auto& mutation : mutations) {
        auto data = make_dsf({});
        data[mutation.offset] = mutation.value;
        const auto path = write_fixture(directory, std::string(mutation.name) + ".dsf", data);
        crossdsd_reader* reader = nullptr;
        CHECK(open(path, &reader) == mutation.expected);
        CHECK(reader == nullptr);
        if (reader) {
            crossdsd_reader_close(reader);
            reader = nullptr;
        }
    }
    auto truncated = make_dsf({});
    truncated.resize(93);
    put64(truncated, 12, truncated.size());
    put64(truncated, 20, 0);
    const auto truncated_path = write_fixture(directory, "truncated.dsf", truncated);
    crossdsd_reader* reader = nullptr;
    CHECK(open(truncated_path, &reader) != CROSSDSD_OK);

    auto bad_metadata = make_dsf({});
    bad_metadata[bad_metadata.size() - 10] = 'N';
    const auto bad_metadata_path = write_fixture(directory, "bad-metadata.dsf", bad_metadata);
    CHECK(open(bad_metadata_path, &reader) == CROSSDSD_OK);
    if (reader) {
        crossdsd_stream_info info{};
        info.struct_size = sizeof(info);
        CHECK(crossdsd_reader_get_info(reader, &info) == CROSSDSD_OK);
        CHECK(info.metadata_status == CROSSDSD_METADATA_INVALID);
        crossdsd_reader_close(reader);
    }
}

}  // namespace

int main() {
    const fs::path directory =
        fs::temp_directory_path() / ("crossdsd-tests-" + std::to_string(std::rand()));
    fs::create_directories(directory);
    test_valid_and_progressive(directory);
    test_bit_expansion();
    test_padding_0_to_7(directory);
    test_compatible_extra_padding_group(directory);
    test_rejections(directory);
    fs::remove_all(directory);
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All CrossDSD format tests passed\n";
    return 0;
}
