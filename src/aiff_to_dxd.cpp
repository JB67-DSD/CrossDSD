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
#include <cstdio>
#include <filesystem>
#include <limits>
#include <memory>
#include <new>
#include <vector>

namespace {

constexpr uint32_t kInputRate = 44100;
constexpr uint32_t kOutputRate = 352800;
constexpr uint32_t kFactor = 8;
constexpr uint32_t kTapCount = 2049;
constexpr uint32_t kDelay = (kTapCount - 1) / 2;
constexpr uint32_t kHistoryFrames = (kTapCount + kFactor - 1) / kFactor;
constexpr double kPi = 3.141592653589793238462643383279502884;

void set_error(crossdsd_error* error, crossdsd_result code,
               const char* message) {
    if (!error || error->struct_size < sizeof(*error)) return;
    error->code = code;
    error->byte_offset = 0;
    std::snprintf(error->message, sizeof(error->message), "%s", message);
}

double bessel_i0(double value) {
    const double x = std::fabs(value);
    if (x < 3.75) {
        const double y = (x / 3.75) * (x / 3.75);
        return 1.0 + y * (3.5156229 + y * (3.0899424 +
               y * (1.2067492 + y * (0.2659732 +
               y * (0.0360768 + y * 0.0045813)))));
    }
    const double y = 3.75 / x;
    return (std::exp(x) / std::sqrt(x)) *
        (0.39894228 + y * (0.01328592 + y * (0.00225319 +
         y * (-0.00157565 + y * (0.00916281 + y * (-0.02057706 +
         y * (0.02635537 + y * (-0.01647633 + y * 0.00392377))))))));
}

std::vector<std::vector<double>> make_polyphase_filter() {
    constexpr double cutoff_hz = 21025.0;
    constexpr double beta = 12.0;
    const double normalized = cutoff_hz / kOutputRate;
    const double denominator = bessel_i0(beta);
    std::vector<double> impulse(kTapCount);
    for (uint32_t n = 0; n < kTapCount; ++n) {
        const double centered = static_cast<double>(n) - kDelay;
        const double argument = 2.0 * normalized * centered;
        const double sinc = std::fabs(argument) < 1e-15
            ? 1.0 : std::sin(kPi * argument) / (kPi * argument);
        const double ratio = centered / kDelay;
        const double window = bessel_i0(
            beta * std::sqrt(std::max(0.0, 1.0 - ratio * ratio))) /
            denominator;
        impulse[n] = kFactor * 2.0 * normalized * sinc * window;
    }
    std::vector<std::vector<double>> phases(kFactor);
    for (uint32_t phase = 0; phase < kFactor; ++phase) {
        for (uint32_t n = phase; n < kTapCount; n += kFactor)
            phases[phase].push_back(impulse[n]);
        double sum = 0.0;
        for (double coefficient : phases[phase]) sum += coefficient;
        for (double& coefficient : phases[phase]) coefficient /= sum;
    }
    return phases;
}

struct ReaderCloser {
    void operator()(crossdsd_aiff_reader* value) const {
        crossdsd_aiff_reader_close(value);
    }
};

}  // namespace

extern "C" crossdsd_result crossdsd_convert_aiff_to_dxd_file(
    const char* input_path, const char* output_path,
    const crossdsd_aiff_to_dxd_config* config,
    crossdsd_aiff_to_dxd_report* report, crossdsd_error* error) {
    if (!input_path || !output_path || !config || !report ||
        config->struct_size < sizeof(*config) ||
        report->struct_size < sizeof(*report)) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                  "invalid AIFF to DXD arguments");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    const uint32_t report_size = report->struct_size;
    *report = {};
    report->struct_size = report_size;
    report->api_version = CROSSDSD_CONVERT_API_VERSION;
    const auto started = std::chrono::steady_clock::now();
    const auto progress = [&](double fraction) {
        return !config->progress_callback || config->progress_callback(
            config->progress_user_data, fraction) != 0;
    };
    if (config->fixed_gain_enabled &&
        (!std::isfinite(config->fixed_gain_db) || config->fixed_gain_db > 0.0)) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                  "AIFF fixed gain must be finite and <= 0 dB");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    const double fixed_gain = config->fixed_gain_enabled
        ? std::pow(10.0, config->fixed_gain_db / 20.0) : 1.0;
    if (!progress(0.0)) {
        set_error(error, CROSSDSD_ERROR_CANCELLED, "conversion cancelled");
        return CROSSDSD_ERROR_CANCELLED;
    }

    crossdsd_aiff_reader* raw_reader = nullptr;
    crossdsd_result result = crossdsd_aiff_reader_open_file(
        input_path, &raw_reader, error);
    if (result != CROSSDSD_OK) return result;
    std::unique_ptr<crossdsd_aiff_reader, ReaderCloser> reader(raw_reader);
    crossdsd_aiff_reader_info input{};
    input.struct_size = sizeof(input);
    result = crossdsd_aiff_reader_get_info(reader.get(), &input);
    if (result != CROSSDSD_OK) return result;
    if (input.sample_rate_hz != kInputRate) {
        set_error(error, CROSSDSD_ERROR_UNSUPPORTED_RATE,
                  "AIFF input must be 44.1 kHz");
        return CROSSDSD_ERROR_UNSUPPORTED_RATE;
    }
    if (input.frame_count > std::numeric_limits<uint64_t>::max() / kFactor) {
        set_error(error, CROSSDSD_ERROR_OVERFLOW,
                  "DXD frame count overflow");
        return CROSSDSD_ERROR_OVERFLOW;
    }
    const uint64_t output_frames = input.frame_count * kFactor;
    crossdsd_wave_writer_config writer_config{};
    writer_config.struct_size = sizeof(writer_config);
    writer_config.container = config->container;
    writer_config.sample_rate_hz = kOutputRate;
    writer_config.channel_count = input.channel_count;
    writer_config.channel_type = input.channel_count == 1 ? 1u :
                                 input.channel_count == 2 ? 2u : 0u;
    writer_config.expected_frame_count = output_frames;
    crossdsd_wave_writer* writer = nullptr;
    result = crossdsd_wave_writer_open_file(
        output_path, &writer_config, &writer, error);
    if (result != CROSSDSD_OK) return result;

    const auto close_writer = [&](crossdsd_result current) {
        crossdsd_error close_error{};
        close_error.struct_size = sizeof(close_error);
        const crossdsd_result closed = crossdsd_wave_writer_close(
            writer, current == CROSSDSD_OK ? error : &close_error);
        writer = nullptr;
        return current == CROSSDSD_OK ? closed : current;
    };

    try {
        const uint64_t chunk_frames = config->input_chunk_frames == 0
            ? 16384u : config->input_chunk_frames;
        if (chunk_frames > SIZE_MAX / input.channel_count) {
            set_error(error, CROSSDSD_ERROR_OVERFLOW, "AIFF chunk too large");
            return close_writer(CROSSDSD_ERROR_OVERFLOW);
        }
        const auto phases = make_polyphase_filter();
        std::vector<float> source(
            static_cast<size_t>(chunk_frames * input.channel_count));
        std::vector<double> history(
            static_cast<size_t>(input.channel_count) * kHistoryFrames, 0.0);
        std::vector<float> output;
        output.reserve(static_cast<size_t>(chunk_frames * kFactor *
                                            input.channel_count));
        uint32_t head = kHistoryFrames - 1;
        uint64_t raw_index = 0;
        uint64_t written = 0;
        uint64_t consumed = 0;

        const auto feed_frame = [&](const float* frame) {
            head = (head + 1) % kHistoryFrames;
            for (uint32_t channel = 0; channel < input.channel_count; ++channel) {
                history[static_cast<size_t>(channel) * kHistoryFrames + head] =
                    frame ? frame[channel] : 0.0;
                if (frame) report->channel_input_peaks[channel] = std::max(
                    report->channel_input_peaks[channel],
                    std::fabs(static_cast<double>(frame[channel])));
            }
            for (uint32_t phase = 0; phase < kFactor; ++phase, ++raw_index) {
                const bool keep = raw_index >= kDelay && written < output_frames;
                if (!keep) continue;
                for (uint32_t channel = 0; channel < input.channel_count; ++channel) {
                    double value = 0.0;
                    const size_t base = static_cast<size_t>(channel) * kHistoryFrames;
                    for (size_t tap = 0; tap < phases[phase].size(); ++tap) {
                        const uint32_t index = static_cast<uint32_t>(
                            (head + kHistoryFrames - tap) % kHistoryFrames);
                        value += phases[phase][tap] * history[base + index];
                    }
                    value *= fixed_gain;
                    output.push_back(static_cast<float>(value));
                    report->channel_output_peaks[channel] = std::max(
                        report->channel_output_peaks[channel], std::fabs(value));
                }
                ++written;
            }
        };

        while (consumed < input.frame_count) {
            uint64_t frames_read = 0;
            result = crossdsd_aiff_reader_read(
                reader.get(), source.data(), chunk_frames, &frames_read, error);
            if (result != CROSSDSD_OK) return close_writer(result);
            if (frames_read == 0) break;
            for (uint64_t frame = 0; frame < frames_read; ++frame)
                feed_frame(source.data() + frame * input.channel_count);
            consumed += frames_read;
            if (!output.empty()) {
                result = crossdsd_wave_writer_write(
                    writer, output.data(), output.size() / input.channel_count,
                    error);
                if (result != CROSSDSD_OK) return close_writer(result);
                output.clear();
            }
            if (!progress(std::min(0.99, 0.99 * static_cast<double>(consumed) /
                                           std::max<uint64_t>(1, input.frame_count)))) {
                set_error(error, CROSSDSD_ERROR_CANCELLED,
                          "conversion cancelled");
                return close_writer(CROSSDSD_ERROR_CANCELLED);
            }
        }
        if (consumed != input.frame_count) {
            set_error(error, CROSSDSD_ERROR_TRUNCATED,
                      "AIFF audio ended before its declared duration");
            return close_writer(CROSSDSD_ERROR_TRUNCATED);
        }
        for (uint32_t frame = 0; frame < kDelay / kFactor; ++frame)
            feed_frame(nullptr);
        if (!output.empty()) {
            result = crossdsd_wave_writer_write(
                writer, output.data(), output.size() / input.channel_count,
                error);
            if (result != CROSSDSD_OK) return close_writer(result);
        }
        if (written != output_frames) {
            set_error(error, CROSSDSD_ERROR_INVALID_SAMPLE_COUNT,
                      "unexpected DXD frame count");
            return close_writer(CROSSDSD_ERROR_INVALID_SAMPLE_COUNT);
        }

        if (input.metadata_size_bytes != 0) {
            if (input.metadata_size_bytes > SIZE_MAX) {
                set_error(error, CROSSDSD_ERROR_OVERFLOW,
                          "AIFF metadata is too large");
                return close_writer(CROSSDSD_ERROR_OVERFLOW);
            }
            std::vector<uint8_t> metadata(
                static_cast<size_t>(input.metadata_size_bytes));
            uint64_t metadata_read = 0;
            result = crossdsd_aiff_reader_read_metadata(
                reader.get(), metadata.data(), metadata.size(), &metadata_read,
                error);
            if (result != CROSSDSD_OK) return close_writer(result);
            result = crossdsd_wave_writer_write_id3(
                writer, metadata.data(), metadata_read, error);
            if (result != CROSSDSD_OK) return close_writer(result);
        }
    } catch (const std::bad_alloc&) {
        set_error(error, CROSSDSD_ERROR_OUT_OF_MEMORY,
                  "not enough memory for AIFF conversion");
        return close_writer(CROSSDSD_ERROR_OUT_OF_MEMORY);
    }

    crossdsd_wave_writer_info writer_info{};
    writer_info.struct_size = sizeof(writer_info);
    result = crossdsd_wave_writer_get_info(writer, &writer_info);
    if (result != CROSSDSD_OK) return close_writer(result);
    result = close_writer(CROSSDSD_OK);
    if (result != CROSSDSD_OK) return result;
    /* The file is already finalized: 1.0 is a completion notification, not
       another cancellation point. */
    (void)progress(1.0);
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    report->input_sample_rate_hz = input.sample_rate_hz;
    report->output_sample_rate_hz = kOutputRate;
    report->channel_count = input.channel_count;
    report->input_bits_per_sample = input.bits_per_sample;
    report->container = writer_info.container;
    report->input_frames = input.frame_count;
    report->output_frames = writer_info.frame_count;
    std::error_code size_error;
    std::filesystem::path finalized_path;
#if defined(_WIN32)
    finalized_path = std::filesystem::u8path(output_path);
#else
    finalized_path = std::filesystem::path(output_path);
#endif
    report->output_size_bytes =
        std::filesystem::file_size(finalized_path, size_error);
    if (size_error)
        report->output_size_bytes = writer_info.data_size_bytes +
                                    writer_info.metadata_size_bytes + 80u;
    report->metadata_size_bytes = writer_info.metadata_size_bytes;
    report->metadata_status = input.metadata_status;
    report->duration_seconds = static_cast<double>(input.frame_count) /
                               input.sample_rate_hz;
    report->conversion_seconds = elapsed;
    report->realtime_factor = elapsed > 0.0
        ? report->duration_seconds / elapsed : 0.0;
    report->latency_output_samples = kDelay;
    return CROSSDSD_OK;
}
