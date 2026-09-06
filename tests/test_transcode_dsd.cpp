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
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <numbers>
#include <string>
#include <vector>

namespace {

int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << __FILE__ << ':' << __LINE__ \
    << ": CHECK failed: " << #x << '\n'; ++failures; } } while (false)

std::filesystem::path temporary(const char* suffix) {
    return std::filesystem::temp_directory_path() /
        ("crossdsd-transcode-dsd-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) +
         suffix);
}

std::string utf8(const std::filesystem::path& path) {
#if defined(_WIN32)
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
#else
    return path.string();
#endif
}

void write_wave(const std::filesystem::path& path, uint64_t frames) {
    std::vector<float> samples(frames * 2u);
    for (uint64_t frame = 0; frame < frames; ++frame) {
        samples[frame * 2u] = static_cast<float>(
            1.1 * std::sin(
                2.0 * std::numbers::pi * 997.0 * frame / 352'800.0));
        samples[frame * 2u + 1u] = static_cast<float>(
            0.4 * std::sin(
                2.0 * std::numbers::pi * 3001.0 * frame / 352'800.0));
    }
    crossdsd_wave_writer_config config{};
    config.struct_size = sizeof(config);
    config.sample_rate_hz = 352'800;
    config.channel_count = 2;
    config.expected_frame_count = frames;
    config.container = CROSSDSD_WAVE_RIFF;
    crossdsd_wave_writer* writer = nullptr;
    const auto path_string = utf8(path);
    CHECK(crossdsd_wave_writer_open_file(
              path_string.c_str(), &config, &writer, nullptr) == CROSSDSD_OK);
    CHECK(crossdsd_wave_writer_write(
              writer, samples.data(), frames, nullptr) == CROSSDSD_OK);
    const std::vector<uint8_t> id3{'I', 'D', '3', 4, 0, 0, 0, 0, 0, 0};
    CHECK(crossdsd_wave_writer_write_id3(
              writer, id3.data(), id3.size(), nullptr) == CROSSDSD_OK);
    CHECK(crossdsd_wave_writer_close(writer, nullptr) == CROSSDSD_OK);
}

void encode_source(
    const std::filesystem::path& wave, const std::filesystem::path& dsf,
    crossdsd_dsd_rate rate) {
    crossdsd_encode_file_config config{};
    config.struct_size = sizeof(config);
    config.dsd_rate = rate;
    config.frame_chunk_count = 257;
    crossdsd_encode_file_report report{};
    report.struct_size = sizeof(report);
    const auto wave_string = utf8(wave);
    const auto dsf_string = utf8(dsf);
    CHECK(crossdsd_encode_file(
              wave_string.c_str(), dsf_string.c_str(), &config, &report,
              nullptr) == CROSSDSD_OK);
}

struct Progress {
    std::vector<double> values;
    double cancel_at = 2.0;
};

int progress(void* context, double fraction) {
    auto* value = static_cast<Progress*>(context);
    value->values.push_back(fraction);
    return fraction < value->cancel_at;
}

void disable_pipeline(bool disabled) {
#if defined(_WIN32)
    _putenv_s("CROSSDSD_DISABLE_CPU_PIPELINE", disabled ? "1" : "");
#else
    if (disabled) {
        setenv("CROSSDSD_DISABLE_CPU_PIPELINE", "1", 1);
    } else {
        unsetenv("CROSSDSD_DISABLE_CPU_PIPELINE");
    }
#endif
}

void test_rate_matrix_and_peak() {
    constexpr uint64_t frames = 4099;
    const auto wave = temporary(".wav");
    write_wave(wave, frames);
    const crossdsd_dsd_rate rates[]{
        CROSSDSD_DSD64, CROSSDSD_DSD128, CROSSDSD_DSD256,
        CROSSDSD_DSD512};
    for (const auto input_rate : rates) {
        const auto source = temporary("-source.dsf");
        encode_source(wave, source, input_rate);
        for (const auto output_rate : rates) {
            const auto output = temporary("-output.dsf");
            crossdsd_transcode_file_config config{};
            config.struct_size = sizeof(config);
            config.dsd_rate = output_rate;
            config.packed_chunk_bytes_per_channel = 127;
            config.max_peak_enabled = 1;
            config.max_peak_dbfs = -1.0;
            Progress capture;
            config.progress_callback = progress;
            config.progress_user_data = &capture;
            crossdsd_transcode_file_report report{};
            report.struct_size = sizeof(report);
            const auto source_string = utf8(source);
            const auto output_string = utf8(output);
            CHECK(crossdsd_transcode_file(
                      source_string.c_str(), output_string.c_str(), &config,
                      &report, nullptr) == CROSSDSD_OK);
            CHECK(report.input_dsd_rate == input_rate);
            CHECK(report.output_dsd_rate == output_rate);
            CHECK(report.reconstructed_frames == frames);
            CHECK(report.output_dsd_samples_per_channel ==
                  frames * (static_cast<uint64_t>(output_rate) / 8u));
            CHECK(report.metadata_status == CROSSDSD_METADATA_ID3);
            CHECK(report.reconstructed_peak > 1.0);
            CHECK(report.full_peak_search == 0);
            CHECK(report.analysis_input_fraction > 0.0);
            CHECK(report.analysis_input_fraction <= 1.0);
            const double expected_db =
                std::min(0.0, -1.0 - report.reconstructed_peak_dbfs);
            CHECK(std::abs(report.applied_gain_db - expected_db) < 1e-12);
            CHECK(!capture.values.empty());
            CHECK(capture.values.front() == 0.0);
            CHECK(capture.values.back() == 1.0);
            CHECK(std::is_sorted(capture.values.begin(), capture.values.end()));
            crossdsd_reader* reader = nullptr;
            CHECK(crossdsd_reader_open_file(
                      output_string.c_str(), &reader, nullptr) == CROSSDSD_OK);
            crossdsd_stream_info info{};
            info.struct_size = sizeof(info);
            CHECK(crossdsd_reader_get_info(reader, &info) == CROSSDSD_OK);
            CHECK(info.dsd_rate == output_rate);
            CHECK(info.sample_count_per_channel ==
                  frames * (static_cast<uint64_t>(output_rate) / 8u));
            CHECK(info.metadata_status == CROSSDSD_METADATA_ID3);
            crossdsd_reader_close(reader);
            std::filesystem::remove(output);
        }
        std::filesystem::remove(source);
    }
    std::filesystem::remove(wave);
}

void test_no_gain_and_cancellation() {
    const auto wave = temporary("-cancel.wav");
    const auto source = temporary("-cancel-source.dsf");
    const auto output = temporary("-cancel-output.dsf");
    write_wave(wave, 2048);
    encode_source(wave, source, CROSSDSD_DSD64);
    crossdsd_transcode_file_config config{};
    config.struct_size = sizeof(config);
    config.dsd_rate = CROSSDSD_DSD128;
    config.packed_chunk_bytes_per_channel = 31;
    crossdsd_transcode_file_report report{};
    report.struct_size = sizeof(report);
    auto source_string = utf8(source);
    auto output_string = utf8(output);
    CHECK(crossdsd_transcode_file(
              source_string.c_str(), output_string.c_str(), &config, &report,
              nullptr) == CROSSDSD_OK);
    CHECK(report.max_peak_enabled == 0);
    CHECK(report.applied_gain == 1.0);
    CHECK(report.applied_gain_db == 0.0);
    std::filesystem::remove(output);

    config.fixed_gain_enabled = 1;
    config.fixed_gain_db = -1.75;
    CHECK(crossdsd_transcode_file(
              source_string.c_str(), output_string.c_str(), &config, &report,
              nullptr) == CROSSDSD_OK);
    CHECK(report.max_peak_enabled == 0);
    CHECK(report.fixed_gain_enabled == 1);
    CHECK(report.requested_fixed_gain_db == -1.75);
    CHECK(report.applied_gain_db == -1.75);
    CHECK(report.analysis_seconds == 0.0);
    CHECK(report.analysis_input_fraction == 0.0);
    std::filesystem::remove(output);

    config.max_peak_enabled = 1;
    config.max_peak_dbfs = 0.0;
    CHECK(crossdsd_transcode_file(
              source_string.c_str(), output_string.c_str(), &config, &report,
              nullptr) == CROSSDSD_ERROR_INVALID_ARGUMENT);
    config.max_peak_enabled = 0;
    config.fixed_gain_enabled = 0;

    config.max_peak_enabled = 1;
    config.max_peak_dbfs = 0.0;
    config.full_peak_search = 1;
    CHECK(crossdsd_transcode_file(
              source_string.c_str(), output_string.c_str(), &config, &report,
              nullptr) == CROSSDSD_OK);
    CHECK(report.full_peak_search == 1);
    CHECK(report.quick_scan_seconds == 0.0);
    CHECK(report.analysis_input_fraction == 1.0);
    std::filesystem::remove(output);

    Progress capture;
    capture.cancel_at = 0.25;
    config.max_peak_enabled = 1;
    config.full_peak_search = 0;
    config.max_peak_dbfs = 0.0;
    config.progress_callback = progress;
    config.progress_user_data = &capture;
    CHECK(crossdsd_transcode_file(
              source_string.c_str(), output_string.c_str(), &config, &report,
              nullptr) == CROSSDSD_ERROR_CANCELLED);
    CHECK(!std::filesystem::exists(output));

    Progress pipeline_capture;
    pipeline_capture.cancel_at = 0.25;
    config.max_peak_enabled = 0;
    config.progress_callback = progress;
    config.progress_user_data = &pipeline_capture;
    disable_pipeline(false);
    CHECK(crossdsd_transcode_file(
              source_string.c_str(), output_string.c_str(), &config, &report,
              nullptr) == CROSSDSD_ERROR_CANCELLED);
    disable_pipeline(false);
    CHECK(!std::filesystem::exists(output));
    CHECK(!pipeline_capture.values.empty());

    config.progress_callback = nullptr;
    config.max_peak_enabled = 1;
    config.max_peak_dbfs = 0.1;
    CHECK(crossdsd_transcode_file(
              source_string.c_str(), output_string.c_str(), &config, &report,
              nullptr) == CROSSDSD_ERROR_INVALID_ARGUMENT);
    CHECK(!std::filesystem::exists(output));
    std::filesystem::remove(source);
    std::filesystem::remove(wave);
}

}  // namespace

int main() {
    test_rate_matrix_and_peak();
    test_no_gain_and_cancellation();
    if (failures) return 1;
    std::cout << "All CrossDSD DSD transcoder tests passed\n";
    return 0;
}
