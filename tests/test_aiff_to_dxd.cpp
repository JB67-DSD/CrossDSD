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
#include "crossdsd/convert.h"

#include <algorithm>
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

constexpr double kPi = 3.14159265358979323846;

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

std::filesystem::path temporary(const char* suffix) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
        ("crossdsd-aiff-dxd-" + std::to_string(stamp) + suffix);
}

std::string utf8(const std::filesystem::path& path) {
#if defined(_WIN32)
    const auto encoded = path.u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
#else
    return path.string();
#endif
}

void write_test_aiff(const std::filesystem::path& path, uint32_t frames,
                     uint32_t rate, const std::vector<uint8_t>& id3) {
    std::vector<uint8_t> comm;
    be16(comm, 2u);
    be32(comm, frames);
    be16(comm, 16u);
    const uint8_t rate44100[10] = {
        0x40, 0x0e, 0xac, 0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    const uint8_t rate48000[10] = {
        0x40, 0x0e, 0xbb, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    comm.insert(comm.end(), rate == 44100 ? std::begin(rate44100)
                                          : std::begin(rate48000),
                rate == 44100 ? std::end(rate44100) : std::end(rate48000));

    std::vector<uint8_t> sound;
    be32(sound, 0u); be32(sound, 0u);
    for (uint32_t frame = 0; frame < frames; ++frame) {
        const double t = static_cast<double>(frame) / rate;
        const auto left = static_cast<int16_t>(std::lround(
            0.5 * 32767.0 * std::sin(2.0 * kPi * 1000.0 * t)));
        const auto right = static_cast<int16_t>(std::lround(
            0.4 * 32767.0 * std::sin(2.0 * kPi * 20000.0 * t)));
        be16(sound, static_cast<uint16_t>(left));
        be16(sound, static_cast<uint16_t>(right));
    }
    std::vector<uint8_t> body = {'A', 'I', 'F', 'F'};
    chunk(body, "COMM", comm);
    if (!id3.empty()) chunk(body, "ID3 ", id3);
    chunk(body, "SSND", sound);
    std::vector<uint8_t> file = {'F', 'O', 'R', 'M'};
    be32(file, static_cast<uint32_t>(body.size()));
    file.insert(file.end(), body.begin(), body.end());
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(file.data()), file.size());
}

double projection(const std::vector<float>& samples, uint32_t channels,
                  uint32_t channel, uint32_t rate, double frequency,
                  uint64_t first, uint64_t count) {
    double cosine = 0.0, sine = 0.0;
    for (uint64_t index = 0; index < count; ++index) {
        const double phase = 2.0 * kPi * frequency * (first + index) / rate;
        const double value = samples[(first + index) * channels + channel];
        cosine += value * std::cos(phase);
        sine += value * std::sin(phase);
    }
    return 2.0 * std::hypot(cosine, sine) / count;
}

void test_conversion() {
    constexpr uint32_t input_frames = 16384;
    const std::vector<uint8_t> id3 = {'I','D','3',4,0,0,0,0,0,0};
    const auto input_path = temporary(".aiff");
    const auto output_path = temporary(".wav");
    write_test_aiff(input_path, input_frames, 44100, id3);

    crossdsd_aiff_to_dxd_config config{};
    config.struct_size = sizeof(config);
    config.container = CROSSDSD_WAVE_AUTO;
    config.input_chunk_frames = 257u;  // Deliberately crosses many chunks.
    crossdsd_aiff_to_dxd_report report{};
    report.struct_size = sizeof(report);
    crossdsd_error error{}; error.struct_size = sizeof(error);
    const auto input_string = utf8(input_path);
    const auto output_string = utf8(output_path);
    CHECK(crossdsd_convert_aiff_to_dxd_file(
        input_string.c_str(), output_string.c_str(), &config, &report,
        &error) == CROSSDSD_OK);
    CHECK(report.input_sample_rate_hz == 44100u);
    CHECK(report.output_sample_rate_hz == 352800u);
    CHECK(report.channel_count == 2u);
    CHECK(report.input_frames == input_frames);
    CHECK(report.output_frames == uint64_t(input_frames) * 8u);
    CHECK(report.metadata_size_bytes >= id3.size());
    CHECK(report.output_size_bytes == std::filesystem::file_size(output_path));

    crossdsd_wave_reader* reader = nullptr;
    CHECK(crossdsd_wave_reader_open_file(
        output_string.c_str(), &reader, &error) == CROSSDSD_OK);
    if (reader != nullptr) {
        crossdsd_wave_reader_info info{}; info.struct_size = sizeof(info);
        CHECK(crossdsd_wave_reader_get_info(reader, &info) == CROSSDSD_OK);
        CHECK(info.sample_rate_hz == 352800u);
        CHECK(info.bits_per_sample == 32u);
        CHECK(info.channel_count == 2u);
        CHECK(info.frame_count == uint64_t(input_frames) * 8u);
        CHECK(info.metadata_status == CROSSDSD_METADATA_ID3);

        std::vector<float> samples(
            static_cast<size_t>(info.frame_count * info.channel_count));
        uint64_t total = 0;
        while (total < info.frame_count) {
            uint64_t got = 0;
            const auto result = crossdsd_wave_reader_read(
                reader, samples.data() + total * info.channel_count,
                info.frame_count - total, &got, &error);
            CHECK(result == CROSSDSD_OK);
            if (result != CROSSDSD_OK || got == 0) break;
            total += got;
        }
        CHECK(total == info.frame_count);
        const uint64_t first = 4096u;
        const uint64_t count = info.frame_count - 8192u;
        const double left = projection(samples, 2u, 0u, 352800u,
                                       1000.0, first, count);
        const double right = projection(samples, 2u, 1u, 352800u,
                                        20000.0, first, count);
        const double image = projection(samples, 2u, 0u, 352800u,
                                        43100.0, first, count);
        CHECK(std::fabs(left - 0.5) < 0.002);
        CHECK(std::fabs(right - 0.4) < 0.004);
        CHECK(image < 0.00005);

        std::vector<uint8_t> metadata(info.metadata_size_bytes);
        uint64_t metadata_read = 0;
        CHECK(crossdsd_wave_reader_read_metadata(
            reader, metadata.data(), metadata.size(), &metadata_read,
            &error) == CROSSDSD_OK);
        CHECK(metadata_read >= id3.size());
        CHECK(std::equal(id3.begin(), id3.end(), metadata.begin()));
        crossdsd_wave_reader_close(reader);
    }
    std::filesystem::remove(input_path);
    std::filesystem::remove(output_path);
}

void test_rejects_other_rate() {
    const auto input_path = temporary("-48.aiff");
    const auto output_path = temporary("-48.wav");
    write_test_aiff(input_path, 32u, 48000u, {});
    crossdsd_aiff_to_dxd_config config{}; config.struct_size = sizeof(config);
    crossdsd_aiff_to_dxd_report report{}; report.struct_size = sizeof(report);
    crossdsd_error error{}; error.struct_size = sizeof(error);
    const auto input_string = utf8(input_path);
    const auto output_string = utf8(output_path);
    CHECK(crossdsd_convert_aiff_to_dxd_file(
        input_string.c_str(), output_string.c_str(), &config, &report,
        &error) == CROSSDSD_ERROR_UNSUPPORTED_RATE);
    std::filesystem::remove(input_path);
    std::filesystem::remove(output_path);
}

}  // namespace

int main() {
    test_conversion();
    test_rejects_other_rate();
    if (failures != 0) {
        std::cerr << failures << " AIFF to DXD test(s) failed\n";
        return 1;
    }
    std::cout << "All CrossDSD AIFF to DXD tests passed\n";
    return 0;
}
