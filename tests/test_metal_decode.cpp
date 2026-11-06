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

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

extern "C" bool crossdsd_metal_backend_available(void);

namespace {

constexpr uint32_t channels = 2;
constexpr uint64_t packed_bytes = 1025;
constexpr uint8_t final_bits = 3;
constexpr uint64_t sample_count = (packed_bytes - 1u) * 8u + final_bits;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void check(crossdsd_result result, const crossdsd_error& error) {
    if (result != CROSSDSD_OK) {
        throw std::runtime_error(error.message[0] ? error.message : crossdsd_result_string(result));
    }
}

struct TempDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("crossdsd-metal-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    TempDirectory() { require(std::filesystem::create_directory(path), "create test directory"); }
    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

void write_source(const std::filesystem::path& path, uint32_t rate) {
    crossdsd_dsf_writer_config config{};
    config.struct_size = sizeof(config);
    config.sample_rate_hz = rate;
    config.channel_count = channels;
    crossdsd_error error{};
    error.struct_size = sizeof(error);
    crossdsd_dsf_writer* handle = nullptr;
    check(crossdsd_dsf_writer_open_file(path.c_str(), &config, &handle, &error), error);
    const auto close = [](crossdsd_dsf_writer* writer) {
        crossdsd_dsf_writer_close(writer, nullptr);
    };
    std::unique_ptr<crossdsd_dsf_writer, decltype(close)> writer(handle, close);
    std::array<uint8_t, channels * packed_bytes> data{};
    uint32_t random = 42;
    for (auto& byte : data) {
        random = random * 1664525u + 1013904223u;
        byte = static_cast<uint8_t>(random >> 24u);
    }
    for (uint32_t channel = 0; channel < channels; ++channel) {
        data[(channel + 1u) * packed_bytes - 1u] &= (1u << final_bits) - 1u;
    }
    check(crossdsd_dsf_writer_write_packed(
        writer.get(), data.data(), packed_bytes, final_bits, &error), error);
    check(crossdsd_dsf_writer_close(writer.release(), &error), error);
}

std::vector<float> decode(const std::filesystem::path& source,
                          const std::filesystem::path& output,
                          const crossdsd_decode_file_config& config,
                          uint32_t rate) {
    crossdsd_error error{};
    error.struct_size = sizeof(error);
    crossdsd_decode_file_report report{};
    report.struct_size = sizeof(report);
    check(crossdsd_decode_file(source.c_str(), output.c_str(), &config, &report, &error), error);
    require(report.backend == config.backend, "wrong decoder backend");
    require(report.output_size_bytes == std::filesystem::file_size(output),
            "wrong decoded file size in report");
    require(report.output_frames == sample_count / (rate / CROSSDSD_DXD_SAMPLE_RATE),
            "wrong decoded frame count");
    require(report.fixed_gain_enabled == config.fixed_gain_enabled &&
            report.requested_fixed_gain_db == config.fixed_gain_db, "wrong gain report");
    crossdsd_wave_reader* handle = nullptr;
    check(crossdsd_wave_reader_open_file(output.c_str(), &handle, &error), error);
    std::unique_ptr<crossdsd_wave_reader, decltype(&crossdsd_wave_reader_close)>
        reader(handle, crossdsd_wave_reader_close);
    crossdsd_wave_reader_info info{};
    info.struct_size = sizeof(info);
    check(crossdsd_wave_reader_get_info(reader.get(), &info), error);
    require(info.container == config.container && info.channel_count == channels &&
            info.sample_rate_hz == CROSSDSD_DXD_SAMPLE_RATE &&
            info.frame_count == report.output_frames && info.metadata_size_bytes > 0,
            "wrong DXD format or missing encoder metadata");
    std::vector<float> samples(info.frame_count * info.channel_count);
    uint64_t frames_read = 0;
    check(crossdsd_wave_reader_read(reader.get(), samples.data(), info.frame_count,
                                    &frames_read, &error), error);
    require(frames_read == info.frame_count, "incomplete DXD output");
    return samples;
}

void verify_streaming(const std::filesystem::path& directory) {
    const auto source = directory / "source.dsf";
    const auto output = directory / "output.wav";
    for (const uint32_t rate : {2'822'400u, 5'644'800u, 11'289'600u, 22'579'200u}) {
        write_source(source, rate);
        for (const auto profile : {CROSSDSD_FILTER_STANDARD, CROSSDSD_FILTER_RAW}) {
            crossdsd_decode_file_config config{};
            config.struct_size = sizeof(config);
            config.filter_profile = profile;
            config.container = CROSSDSD_WAVE_RF64;
            config.fixed_gain_enabled = profile == CROSSDSD_FILTER_RAW;
            config.fixed_gain_db = config.fixed_gain_enabled ? -4.0 : 0.0;
            config.backend = CROSSDSD_BACKEND_CPU;
            const auto cpu = decode(source, output, config, rate);
            config.backend = CROSSDSD_BACKEND_METAL;
            const auto one_block = decode(source, output, config, rate);
            // Small blocks exercise history spanning several prior blocks.
            // 137 bytes also checks rounding the requested block size down.
            for (uint64_t block_bytes : {8u, 137u}) {
                config.packed_chunk_bytes_per_channel = block_bytes;
                const auto streamed = decode(source, output, config, rate);
                require(streamed == one_block, "sample mismatch at a Metal block boundary");
                double squared_sum = 0.0;
                for (size_t index = 0; index < cpu.size(); ++index) {
                    const double difference = static_cast<double>(streamed[index]) - cpu[index];
                    require(std::abs(difference) <= 2e-6, "Metal peak error exceeds tolerance");
                    squared_sum += difference * difference;
                }
                require(std::sqrt(squared_sum / cpu.size()) <= 2.5e-7,
                        "Metal RMS error exceeds tolerance");
            }
        }
    }
    crossdsd_decode_file_config config{};
    config.struct_size = sizeof(config);
    config.filter_profile = CROSSDSD_FILTER_STANDARD;
    config.backend = CROSSDSD_BACKEND_METAL;
    config.packed_chunk_bytes_per_channel = 64;
    config.progress_callback = [](void*, double fraction) { return fraction < 0.5 ? 1 : 0; };
    crossdsd_error error{};
    error.struct_size = sizeof(error);
    crossdsd_decode_file_report report{};
    report.struct_size = sizeof(report);
    const auto cancelled = directory / "cancelled.wav";
    require(crossdsd_decode_file(source.c_str(), cancelled.c_str(), &config,
                                &report, &error) == CROSSDSD_ERROR_CANCELLED,
            "Metal decode did not cancel");
    require(!std::filesystem::exists(cancelled), "partial Metal output survived cancellation");
}

void verify_large_input(const std::filesystem::path& directory) {
    const auto source = directory / "large.dsf";
    const auto output = directory / "large.wav";
    write_source(source, 5'644'800u);
    // Extend a valid DSF using a sparse payload. Abort after the first GPU
    // block to test admission above the former 32-bit sample limit cheaply.
    constexpr uint64_t large_samples = uint64_t{1} << 32u;
    constexpr uint64_t audio_bytes = large_samples / 8u * channels;
    constexpr uint64_t header_bytes = 92u;
    std::filesystem::resize_file(source, header_bytes + audio_bytes);
    std::fstream stream(source, std::ios::in | std::ios::out | std::ios::binary);
    const auto patch_u64 = [&stream](std::streamoff offset, uint64_t value) {
        stream.seekp(offset);
        for (unsigned byte = 0; byte < 8; ++byte) {
            stream.put(static_cast<char>(value & 0xffu));
            value >>= 8u;
        }
    };
    patch_u64(12, header_bytes + audio_bytes);  // DSD file size
    patch_u64(64, large_samples);              // fmt sample count
    patch_u64(84, audio_bytes + 12u);          // data chunk size
    stream.close();
    require(stream.good(), "write sparse DSF header");

    double progress = 0;
    crossdsd_decode_file_config config{};
    config.struct_size = sizeof(config);
    config.filter_profile = CROSSDSD_FILTER_STANDARD;
    config.backend = CROSSDSD_BACKEND_METAL;
    config.packed_chunk_bytes_per_channel = 4096;
    config.progress_user_data = &progress;
    config.progress_callback = [](void* context, double fraction) {
        *static_cast<double*>(context) = fraction;
        return 0;
    };
    crossdsd_error error{};
    error.struct_size = sizeof(error);
    crossdsd_decode_file_report report{};
    report.struct_size = sizeof(report);
    const auto result = crossdsd_decode_file(source.c_str(), output.c_str(),
                                              &config, &report, &error);
    if (result != CROSSDSD_ERROR_CANCELLED) check(result, error);
    require(result == CROSSDSD_ERROR_CANCELLED && progress > 0 && progress < 1,
            "Metal failed to process the first block of the large DSF");
    require(!std::filesystem::exists(output), "partial large output survived cancellation");
}

}  // namespace

int main() {
    if (!crossdsd_metal_backend_available()) {
        std::cout << "Metal device unavailable\n";
        return 77;
    }
    try {
        const TempDirectory directory;
        verify_streaming(directory.path);
        verify_large_input(directory.path);
        std::cout << "Metal continuity, CPU comparison and cancellation passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
