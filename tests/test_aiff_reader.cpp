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

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;
#define CHECK(value) do { if (!(value)) { \
    std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #value "\n"; \
    ++failures; } } while (false)

void be16(std::vector<uint8_t>& bytes, uint16_t value) {
    bytes.push_back(static_cast<uint8_t>(value >> 8u));
    bytes.push_back(static_cast<uint8_t>(value));
}

void be32(std::vector<uint8_t>& bytes, uint32_t value) {
    bytes.push_back(static_cast<uint8_t>(value >> 24u));
    bytes.push_back(static_cast<uint8_t>(value >> 16u));
    bytes.push_back(static_cast<uint8_t>(value >> 8u));
    bytes.push_back(static_cast<uint8_t>(value));
}

void chunk(std::vector<uint8_t>& form, const char id[5],
           const std::vector<uint8_t>& payload) {
    form.insert(form.end(), id, id + 4);
    be32(form, static_cast<uint32_t>(payload.size()));
    form.insert(form.end(), payload.begin(), payload.end());
    if (payload.size() & 1u) form.push_back(0u);
}

std::filesystem::path temporary() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
        ("crossdsd-aiff-" + std::to_string(stamp) + ".aiff");
}

std::string utf8(const std::filesystem::path& path) {
#if defined(_WIN32)
    const auto encoded = path.u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
#else
    return path.string();
#endif
}

void test_pcm16_and_id3() {
    std::vector<uint8_t> comm;
    be16(comm, 2u);
    be32(comm, 2u);
    be16(comm, 16u);
    const uint8_t rate44100[10] = {
        0x40, 0x0e, 0xac, 0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    comm.insert(comm.end(), std::begin(rate44100), std::end(rate44100));

    std::vector<uint8_t> sound;
    be32(sound, 0u); be32(sound, 0u);
    be16(sound, 0x7fffu); be16(sound, 0x8000u);
    be16(sound, 0x4000u); be16(sound, 0xc000u);
    const std::vector<uint8_t> id3 = {'I', 'D', '3', 4, 0, 0, 0, 0, 0, 0};

    std::vector<uint8_t> body = {'A', 'I', 'F', 'F'};
    chunk(body, "COMM", comm);
    chunk(body, "ID3 ", id3);
    chunk(body, "SSND", sound);
    std::vector<uint8_t> file = {'F', 'O', 'R', 'M'};
    be32(file, static_cast<uint32_t>(body.size()));
    file.insert(file.end(), body.begin(), body.end());

    const auto path = temporary();
    { std::ofstream output(path, std::ios::binary);
      output.write(reinterpret_cast<const char*>(file.data()), file.size()); }

    crossdsd_aiff_reader* reader = nullptr;
    crossdsd_error error{}; error.struct_size = sizeof(error);
    const auto path_string = utf8(path);
    CHECK(crossdsd_aiff_reader_open_file(
        path_string.c_str(), &reader, &error) == CROSSDSD_OK);
    if (reader != nullptr) {
        crossdsd_aiff_reader_info info{}; info.struct_size = sizeof(info);
        CHECK(crossdsd_aiff_reader_get_info(reader, &info) == CROSSDSD_OK);
        CHECK(info.sample_rate_hz == 44100u);
        CHECK(info.channel_count == 2u);
        CHECK(info.bits_per_sample == 16u);
        CHECK(info.frame_count == 2u);
        CHECK(info.channel_type == 2u);
        CHECK(info.metadata_status == CROSSDSD_METADATA_ID3);
        CHECK(info.metadata_size_bytes == id3.size());

        float samples[4]{}; uint64_t frames = 0u;
        CHECK(crossdsd_aiff_reader_read(
            reader, samples, 2u, &frames, &error) == CROSSDSD_OK);
        CHECK(frames == 2u);
        CHECK(std::fabs(samples[0] - 32767.0f / 32768.0f) < 1e-7f);
        CHECK(samples[1] == -1.0f);
        CHECK(samples[2] == 0.5f);
        CHECK(samples[3] == -0.5f);
        std::vector<uint8_t> metadata(id3.size()); uint64_t size = 0u;
        CHECK(crossdsd_aiff_reader_read_metadata(
            reader, metadata.data(), metadata.size(), &size, &error) ==
            CROSSDSD_OK);
        CHECK(size == id3.size() && metadata == id3);
        CHECK(crossdsd_aiff_reader_read(
            reader, samples, 2u, &frames, &error) ==
            CROSSDSD_ERROR_END_OF_STREAM);
        crossdsd_aiff_reader_close(reader);
    }
    std::filesystem::remove(path);
}

void test_aifc_sowt_pcm16() {
    std::vector<uint8_t> comm;
    be16(comm, 2u);
    be32(comm, 2u);
    be16(comm, 16u);
    const uint8_t rate44100[10] = {
        0x40, 0x0e, 0xac, 0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    comm.insert(comm.end(), std::begin(rate44100), std::end(rate44100));
    comm.insert(comm.end(), {'s', 'o', 'w', 't'});

    std::vector<uint8_t> sound;
    be32(sound, 0u); be32(sound, 0u);
    sound.insert(sound.end(), {
        0xff, 0x7f, 0x00, 0x80,
        0x00, 0x40, 0x00, 0xc0
    });

    std::vector<uint8_t> body = {'A', 'I', 'F', 'C'};
    chunk(body, "COMM", comm);
    chunk(body, "SSND", sound);
    std::vector<uint8_t> file = {'F', 'O', 'R', 'M'};
    be32(file, static_cast<uint32_t>(body.size()));
    file.insert(file.end(), body.begin(), body.end());

    const auto path = temporary();
    { std::ofstream output(path, std::ios::binary);
      output.write(reinterpret_cast<const char*>(file.data()), file.size()); }

    crossdsd_aiff_reader* reader = nullptr;
    crossdsd_error error{}; error.struct_size = sizeof(error);
    const auto path_string = utf8(path);
    CHECK(crossdsd_aiff_reader_open_file(
        path_string.c_str(), &reader, &error) == CROSSDSD_OK);
    if (reader != nullptr) {
        float samples[4]{}; uint64_t frames = 0u;
        CHECK(crossdsd_aiff_reader_read(
            reader, samples, 2u, &frames, &error) == CROSSDSD_OK);
        CHECK(frames == 2u);
        CHECK(std::fabs(samples[0] - 32767.0f / 32768.0f) < 1e-7f);
        CHECK(samples[1] == -1.0f);
        CHECK(samples[2] == 0.5f);
        CHECK(samples[3] == -0.5f);
        crossdsd_aiff_reader_close(reader);
    }
    std::filesystem::remove(path);
}

}  // namespace

void inspect_real_file(const char* path) {
    crossdsd_aiff_reader* reader = nullptr;
    crossdsd_error error{}; error.struct_size = sizeof(error);
    CHECK(crossdsd_aiff_reader_open_file(path, &reader, &error) == CROSSDSD_OK);
    if (reader == nullptr) return;
    crossdsd_aiff_reader_info info{}; info.struct_size = sizeof(info);
    CHECK(crossdsd_aiff_reader_get_info(reader, &info) == CROSSDSD_OK);
    std::vector<float> samples(32768u * info.channel_count);
    uint64_t total = 0u;
    double peak = 0.0;
    for (;;) {
        uint64_t frames = 0u;
        const auto result = crossdsd_aiff_reader_read(
            reader, samples.data(), 32768u, &frames, &error);
        if (result == CROSSDSD_ERROR_END_OF_STREAM) break;
        CHECK(result == CROSSDSD_OK);
        if (result != CROSSDSD_OK) break;
        total += frames;
        for (uint64_t index = 0; index < frames * info.channel_count; ++index)
            peak = std::max(peak, std::fabs(static_cast<double>(samples[index])));
    }
    CHECK(total == info.frame_count);
    std::cout << path << ": " << info.sample_rate_hz << " Hz, "
              << info.bits_per_sample << " bit, " << info.channel_count
              << " ch, " << (double(info.frame_count) / info.sample_rate_hz)
              << " s, peak " << (peak == 0.0 ? -INFINITY : 20.0 * std::log10(peak))
              << " dBFS, ID3 " << info.metadata_size_bytes << " bytes\n";
    crossdsd_aiff_reader_close(reader);
}

int main(int argc, char** argv) {
    test_pcm16_and_id3();
    test_aifc_sowt_pcm16();
    for (int index = 1; index < argc; ++index) inspect_real_file(argv[index]);
    if (failures != 0) {
        std::cerr << failures << " AIFF reader test(s) failed\n";
        return 1;
    }
    std::cout << "All CrossDSD AIFF reader tests passed\n";
    return 0;
}
