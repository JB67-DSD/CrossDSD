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
        ("crossdsd-analyze-" +
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

void create_source(
    const std::filesystem::path& wave, const std::filesystem::path& dsf,
    double scale = 1.0) {
    constexpr uint64_t frames = 352'800u;
    std::vector<float> samples(frames * 2u);
    for (uint64_t frame = 0; frame < frames; ++frame) {
        const double seconds = static_cast<double>(frame) / 352'800.0;
        const double left_gain = scale *
            (seconds >= 0.25 && seconds < 0.35 ? 0.80 : 0.02);
        const double right_gain = scale *
            (seconds >= 0.65 && seconds < 0.75 ? 0.40 : 0.02);
        samples[frame * 2u] = static_cast<float>(
            left_gain * std::sin(2.0 * std::numbers::pi * 997.0 * seconds));
        samples[frame * 2u + 1u] = static_cast<float>(
            right_gain * std::sin(2.0 * std::numbers::pi * 1301.0 * seconds));
    }
    crossdsd_wave_writer_config writer_config{};
    writer_config.struct_size = sizeof(writer_config);
    writer_config.sample_rate_hz = 352'800u;
    writer_config.channel_count = 2u;
    writer_config.expected_frame_count = frames;
    writer_config.container = CROSSDSD_WAVE_RIFF;
    crossdsd_wave_writer* writer = nullptr;
    const auto wave_string = utf8(wave);
    CHECK(crossdsd_wave_writer_open_file(
              wave_string.c_str(), &writer_config, &writer, nullptr) ==
          CROSSDSD_OK);
    CHECK(crossdsd_wave_writer_write(
              writer, samples.data(), frames, nullptr) == CROSSDSD_OK);
    CHECK(crossdsd_wave_writer_close(writer, nullptr) == CROSSDSD_OK);

    crossdsd_encode_file_config encode_config{};
    encode_config.struct_size = sizeof(encode_config);
    encode_config.dsd_rate = CROSSDSD_DSD64;
    crossdsd_encode_file_report encode_report{};
    encode_report.struct_size = sizeof(encode_report);
    const auto dsf_string = utf8(dsf);
    CHECK(crossdsd_encode_file(
              wave_string.c_str(), dsf_string.c_str(), &encode_config,
              &encode_report, nullptr) == CROSSDSD_OK);
}

struct Progress {
    std::vector<double> values;
    double cancel_at = 2.0;
};

int progress(void* opaque, double fraction) {
    auto* value = static_cast<Progress*>(opaque);
    value->values.push_back(fraction);
    return fraction < value->cancel_at;
}

crossdsd_result analyze(
    const std::filesystem::path& source, bool full,
    crossdsd_analyze_file_report& report, Progress* capture = nullptr) {
    crossdsd_analyze_file_config config{};
    config.struct_size = sizeof(config);
    config.packed_chunk_bytes_per_channel = 4096u;
    config.full_peak_search = full ? 1u : 0u;
    config.progress_callback = capture ? progress : nullptr;
    config.progress_user_data = capture;
    report = {};
    report.struct_size = sizeof(report);
    crossdsd_error error{};
    error.struct_size = sizeof(error);
    const auto source_string = utf8(source);
    return crossdsd_analyze_file(
        source_string.c_str(), &config, &report, &error);
}

void test_quick_full_positions_and_cancellation() {
    const auto wave = temporary(".wav");
    const auto dsf = temporary(".dsf");
    create_source(wave, dsf);

    Progress quick_progress;
    crossdsd_analyze_file_report quick{};
    CHECK(analyze(dsf, false, quick, &quick_progress) == CROSSDSD_OK);
    CHECK(quick.api_version == CROSSDSD_CONVERT_API_VERSION);
    CHECK(quick.input_dsd_rate == CROSSDSD_DSD64);
    CHECK(quick.reconstructed_sample_rate_hz == 352'800u);
    CHECK(quick.channel_count == 2u);
    CHECK(quick.full_peak_search == 0u);
    CHECK(quick.reconstructed_peak == quick.channel_reconstructed_peaks[0]);
    CHECK(quick.channel_reconstructed_peaks[0] >
          quick.channel_reconstructed_peaks[1]);
    CHECK(quick.channel_peak_seconds[0] >= 0.24);
    CHECK(quick.channel_peak_seconds[0] <= 0.36);
    CHECK(quick.channel_peak_seconds[1] >= 0.64);
    CHECK(quick.channel_peak_seconds[1] <= 0.76);
    CHECK(quick.peak_channel == 0u);
    CHECK(quick.peak_frame == quick.channel_peak_frames[0]);
    CHECK(!quick_progress.values.empty());
    CHECK(quick_progress.values.front() == 0.0);
    CHECK(quick_progress.values.back() == 1.0);
    CHECK(std::is_sorted(
        quick_progress.values.begin(), quick_progress.values.end()));

    crossdsd_analyze_file_report full{};
    CHECK(analyze(dsf, true, full) == CROSSDSD_OK);
    CHECK(full.full_peak_search == 1u);
    CHECK(full.quick_scan_seconds == 0.0);
    CHECK(full.analyzed_input_fraction == 1.0);
    CHECK(std::abs(full.reconstructed_peak - quick.reconstructed_peak) < 1e-12);
    CHECK(full.peak_frame == quick.peak_frame);
    CHECK(full.channel_peak_frames[1] == quick.channel_peak_frames[1]);

    Progress cancelled;
    cancelled.cancel_at = 0.20;
    crossdsd_analyze_file_report cancelled_report{};
    CHECK(analyze(dsf, false, cancelled_report, &cancelled) ==
          CROSSDSD_ERROR_CANCELLED);

    std::filesystem::remove(dsf);
    std::filesystem::remove(wave);
}

void test_album_gain_and_progress() {
    const auto loud_wave = temporary("-loud.wav");
    const auto loud_dsf = temporary("-loud.dsf");
    const auto quiet_wave = temporary("-quiet.wav");
    const auto quiet_dsf = temporary("-quiet.dsf");
    create_source(loud_wave, loud_dsf, 1.0);
    create_source(quiet_wave, quiet_dsf, 0.25);
    const std::string loud = utf8(loud_dsf);
    const std::string quiet = utf8(quiet_dsf);
    const char* paths[]{loud.c_str(), quiet.c_str()};
    crossdsd_analyze_batch_config config{};
    config.struct_size = sizeof(config);
    config.target_peak_dbfs = -1.0;
    Progress capture;
    config.progress_callback = progress;
    config.progress_user_data = &capture;
    crossdsd_analyze_file_report files[2]{};
    double gains[2]{};
    crossdsd_analyze_batch_report report{};
    report.struct_size = sizeof(report);
    CHECK(crossdsd_analyze_batch(
              paths, 2u, &config, files, gains, &report, nullptr) ==
          CROSSDSD_OK);
    CHECK(report.input_file_count == 2u);
    CHECK(report.completed_file_count == 2u);
    CHECK(report.maximum_file_index == 0u);
    CHECK(files[0].reconstructed_peak > files[1].reconstructed_peak);
    CHECK(gains[0] <= gains[1]);
    CHECK(report.common_gain_db == gains[0]);
    CHECK(report.target_peak_dbfs == -1.0);
    CHECK(report.total_duration_seconds == 2.0);
    CHECK(!capture.values.empty());
    CHECK(capture.values.front() == 0.0);
    CHECK(capture.values.back() == 1.0);
    CHECK(std::is_sorted(capture.values.begin(), capture.values.end()));

    Progress cancelled;
    cancelled.cancel_at = 0.60;
    config.progress_user_data = &cancelled;
    report = {};
    report.struct_size = sizeof(report);
    CHECK(crossdsd_analyze_batch(
              paths, 2u, &config, files, gains, &report, nullptr) ==
          CROSSDSD_ERROR_CANCELLED);

    config.progress_callback = nullptr;
    config.target_peak_dbfs = 0.1;
    report = {};
    report.struct_size = sizeof(report);
    CHECK(crossdsd_analyze_batch(
              paths, 2u, &config, files, gains, &report, nullptr) ==
          CROSSDSD_ERROR_INVALID_ARGUMENT);

    std::filesystem::remove(loud_dsf);
    std::filesystem::remove(loud_wave);
    std::filesystem::remove(quiet_dsf);
    std::filesystem::remove(quiet_wave);
}

}  // namespace

int main() {
    test_quick_full_positions_and_cancellation();
    test_album_gain_and_progress();
    if (failures) return 1;
    std::cout << "All CrossDSD DSD analysis tests passed\n";
    return 0;
}
