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
#include "quick_peak_analysis.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>

namespace {

constexpr uint32_t kDxdRate = 352'800u;

void set_error(crossdsd_error* error, crossdsd_result code,
               const char* message) {
    if (!error || error->struct_size < sizeof(crossdsd_error)) return;
    error->code = code;
    error->byte_offset = 0;
    std::snprintf(error->message, sizeof(error->message), "%s", message);
}

struct ReaderOwner {
    crossdsd_reader* value = nullptr;
    ~ReaderOwner() { crossdsd_reader_close(value); }
};

struct ProgressAdapter {
    crossdsd_progress_callback callback = nullptr;
    void* user_data = nullptr;
    double last_fraction = 0.0;
};

struct BatchProgressAdapter {
    crossdsd_progress_callback callback = nullptr;
    void* user_data = nullptr;
    uint32_t file_index = 0;
    uint32_t file_count = 1;
    double last_fraction = 0.0;
};

int analysis_progress(void* opaque, double fraction) {
    auto* adapter = static_cast<ProgressAdapter*>(opaque);
    if (!adapter->callback) return 1;
    adapter->last_fraction = std::max(
        adapter->last_fraction,
        std::min(1.0, std::max(0.0, fraction * 2.0)));
    return adapter->callback(adapter->user_data, adapter->last_fraction);
}

int batch_progress(void* opaque, double fraction) {
    auto* adapter = static_cast<BatchProgressAdapter*>(opaque);
    if (!adapter->callback) return 1;
    const double overall =
        (static_cast<double>(adapter->file_index) +
         std::min(1.0, std::max(0.0, fraction))) /
        static_cast<double>(adapter->file_count);
    adapter->last_fraction = std::max(adapter->last_fraction, overall);
    return adapter->callback(adapter->user_data, adapter->last_fraction);
}

double dbfs(double peak) {
    return peak > 0.0
        ? 20.0 * std::log10(peak)
        : -std::numeric_limits<double>::infinity();
}

}  // namespace

extern "C" {

crossdsd_result crossdsd_analyze_file(
    const char* input_path, const crossdsd_analyze_file_config* config,
    crossdsd_analyze_file_report* report, crossdsd_error* error) {
    if (!input_path || !config || !report ||
        config->struct_size < sizeof(crossdsd_analyze_file_config) ||
        report->struct_size < sizeof(crossdsd_analyze_file_report)) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                  "invalid DSD analysis configuration");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    const uint64_t chunk_bytes =
        config->packed_chunk_bytes_per_channel == 0
            ? 65'536u
            : config->packed_chunk_bytes_per_channel;
    if (chunk_bytes > std::numeric_limits<size_t>::max() / 8u) {
        set_error(error, CROSSDSD_ERROR_OVERFLOW,
                  "analysis block is too large");
        return CROSSDSD_ERROR_OVERFLOW;
    }

    ReaderOwner reader;
    auto result = crossdsd_reader_open_file(input_path, &reader.value, error);
    if (result != CROSSDSD_OK) return result;
    crossdsd_stream_info stream{};
    stream.struct_size = sizeof(stream);
    result = crossdsd_reader_get_info(reader.value, &stream);
    if (result != CROSSDSD_OK) return result;
    if (stream.sample_count_per_channel == 0) {
        set_error(error, CROSSDSD_ERROR_INVALID_SAMPLE_COUNT,
                  "analysis input contains no DSD samples");
        return CROSSDSD_ERROR_INVALID_SAMPLE_COUNT;
    }
    if (config->progress_callback &&
        !config->progress_callback(config->progress_user_data, 0.0)) {
        set_error(error, CROSSDSD_ERROR_CANCELLED, "analysis cancelled");
        return CROSSDSD_ERROR_CANCELLED;
    }

    ProgressAdapter adapter{
        config->progress_callback, config->progress_user_data, 0.0};
    crossdsd::internal::QuickPeakAnalysis analysis;
    const auto begin = std::chrono::steady_clock::now();
    result = config->full_peak_search
        ? crossdsd::internal::run_full_peak_analysis(
              input_path, stream, chunk_bytes,
              config->progress_callback ? analysis_progress : nullptr,
              &adapter, analysis, error)
        : crossdsd::internal::run_quick_peak_analysis(
              input_path, stream, chunk_bytes,
              config->progress_callback ? analysis_progress : nullptr,
              &adapter, analysis, error);
    if (result != CROSSDSD_OK) return result;
    if (config->progress_callback &&
        !config->progress_callback(config->progress_user_data, 1.0)) {
        set_error(error, CROSSDSD_ERROR_CANCELLED, "analysis cancelled");
        return CROSSDSD_ERROR_CANCELLED;
    }
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - begin).count();

    crossdsd_analyze_file_report value{};
    value.struct_size = sizeof(value);
    value.api_version = CROSSDSD_CONVERT_API_VERSION;
    value.input_sample_rate_hz = stream.sample_rate_hz;
    value.reconstructed_sample_rate_hz = kDxdRate;
    value.channel_count = stream.channel_count;
    value.channel_type = stream.channel_type;
    value.input_dsd_rate = stream.dsd_rate;
    value.input_dsd_samples_per_channel = stream.sample_count_per_channel;
    value.reconstructed_frames = analysis.reconstructed_frames;
    value.duration_seconds = stream.duration_seconds;
    value.analysis_seconds = elapsed;
    value.realtime_factor = elapsed > 0.0 ? stream.duration_seconds / elapsed : 0.0;
    value.full_peak_search = config->full_peak_search ? 1u : 0u;
    value.quick_scan_seconds = analysis.scan_seconds;
    value.analyzed_input_fraction = std::min(1.0, analysis.fine_input_fraction);
    value.worker_count = analysis.worker_count;
    for (uint32_t channel = 0; channel < stream.channel_count; ++channel) {
        const double peak = analysis.peaks[channel];
        value.channel_reconstructed_peaks[channel] = peak;
        value.channel_reconstructed_peak_dbfs[channel] = dbfs(peak);
        value.channel_peak_frames[channel] = analysis.peak_frames[channel];
        value.channel_peak_seconds[channel] =
            static_cast<double>(analysis.peak_frames[channel]) / kDxdRate;
        if (channel == 0 || peak > value.reconstructed_peak) {
            value.reconstructed_peak = peak;
            value.peak_channel = channel;
            value.peak_frame = analysis.peak_frames[channel];
            value.peak_seconds = value.channel_peak_seconds[channel];
        }
    }
    value.reconstructed_peak_dbfs = dbfs(value.reconstructed_peak);
    *report = value;
    set_error(error, CROSSDSD_OK, "");
    return CROSSDSD_OK;
}

crossdsd_result crossdsd_analyze_batch(
    const char* const* input_paths, uint32_t input_count,
    const crossdsd_analyze_batch_config* config,
    crossdsd_analyze_file_report* file_reports,
    double* individual_gain_db, crossdsd_analyze_batch_report* report,
    crossdsd_error* error) {
    if (!input_paths || input_count == 0 || !config || !file_reports ||
        !individual_gain_db || !report ||
        config->struct_size < sizeof(crossdsd_analyze_batch_config) ||
        report->struct_size < sizeof(crossdsd_analyze_batch_report) ||
        !std::isfinite(config->target_peak_dbfs) ||
        config->target_peak_dbfs > 0.0) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                  "invalid DSD batch analysis configuration");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    for (uint32_t index = 0; index < input_count; ++index) {
        if (!input_paths[index] || !*input_paths[index]) {
            set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                      "invalid DSD batch input path");
            return CROSSDSD_ERROR_INVALID_ARGUMENT;
        }
    }
    if (config->progress_callback &&
        !config->progress_callback(config->progress_user_data, 0.0)) {
        set_error(error, CROSSDSD_ERROR_CANCELLED, "analysis cancelled");
        return CROSSDSD_ERROR_CANCELLED;
    }

    crossdsd_analyze_batch_report value{};
    value.struct_size = sizeof(value);
    value.api_version = CROSSDSD_CONVERT_API_VERSION;
    value.input_file_count = input_count;
    value.full_peak_search = config->full_peak_search ? 1u : 0u;
    value.target_peak_dbfs = config->target_peak_dbfs;
    const auto begin = std::chrono::steady_clock::now();
    BatchProgressAdapter adapter{
        config->progress_callback, config->progress_user_data,
        0u, input_count, 0.0};
    for (uint32_t index = 0; index < input_count; ++index) {
        adapter.file_index = index;
        crossdsd_analyze_file_config file_config{};
        file_config.struct_size = sizeof(file_config);
        file_config.packed_chunk_bytes_per_channel =
            config->packed_chunk_bytes_per_channel;
        file_config.full_peak_search = config->full_peak_search;
        file_config.progress_callback =
            config->progress_callback ? batch_progress : nullptr;
        file_config.progress_user_data = &adapter;
        file_reports[index] = {};
        file_reports[index].struct_size = sizeof(crossdsd_analyze_file_report);
        const auto result = crossdsd_analyze_file(
            input_paths[index], &file_config, &file_reports[index], error);
        if (result != CROSSDSD_OK) return result;
        const double peak_db = file_reports[index].reconstructed_peak_dbfs;
        individual_gain_db[index] = std::isfinite(peak_db)
            ? std::min(0.0, config->target_peak_dbfs - peak_db)
            : 0.0;
        value.common_gain_db = std::min(
            value.common_gain_db, individual_gain_db[index]);
        if (index == 0 || file_reports[index].reconstructed_peak >
                              value.maximum_reconstructed_peak) {
            value.maximum_reconstructed_peak =
                file_reports[index].reconstructed_peak;
            value.maximum_reconstructed_peak_dbfs = peak_db;
            value.maximum_file_index = index;
        }
        value.total_duration_seconds += file_reports[index].duration_seconds;
        value.completed_file_count = index + 1u;
    }
    value.analysis_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - begin).count();
    value.realtime_factor = value.analysis_seconds > 0.0
        ? value.total_duration_seconds / value.analysis_seconds
        : 0.0;
    *report = value;
    set_error(error, CROSSDSD_OK, "");
    return CROSSDSD_OK;
}

}  // extern "C"
