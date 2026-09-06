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

#include "quick_peak_analysis.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint32_t kDxdRate = 352'800u;
constexpr uint32_t kWindowsPerSecond = 400u;
constexpr uint32_t kWindowsPerRegion = 100u;
constexpr uint32_t kRegionsPerBlock = 120u;
constexpr uint32_t kLocalCandidates = 3u;
constexpr uint32_t kMarginRegions = 2u;
constexpr uint32_t kLeadingMarginRegions = 8u;
constexpr double kGlobalThresholdDb = -4.0;

void fail(crossdsd_error* error, crossdsd_result code, const char* message) {
    if (!error || error->struct_size < sizeof(*error)) return;
    error->code = code;
    error->byte_offset = 0;
    std::snprintf(error->message, sizeof(error->message), "%s", message);
}

struct Reader {
    crossdsd_reader* value = nullptr;
    ~Reader() { crossdsd_reader_close(value); }
};
struct Decoder {
    crossdsd_decoder* value = nullptr;
    ~Decoder() { crossdsd_decoder_destroy(value); }
};

struct FrameInterval {
    uint64_t begin = 0;
    uint64_t end = 0;
};

uint32_t adaptive_worker_count(uint32_t channels, size_t intervals) {
    const uint32_t hardware = std::max(1u, std::thread::hardware_concurrency());
    const uint32_t channel_budget = std::max(1u, hardware / std::max(1u, channels));
    return static_cast<uint32_t>(std::max<size_t>(
        1u, std::min<size_t>({8u, channel_budget, intervals})));
}

crossdsd_result analyze_intervals(
    const char* input_path, const crossdsd_stream_info& stream,
    uint64_t chunk_bytes, const std::vector<FrameInterval>& intervals,
    double progress_begin, double progress_scale,
    crossdsd_progress_callback progress_callback, void* progress_user_data,
    double* peaks, uint64_t* peak_frames, uint64_t& processed_samples,
    uint32_t& used_workers,
    crossdsd_error* error) {
    if (intervals.empty()) {
        used_workers = 1;
        return CROSSDSD_OK;
    }
    const uint32_t factor = stream.sample_rate_hz / kDxdRate;
    const uint64_t max_bits = chunk_bytes * 8u;
    const uint64_t output_capacity = max_bits / factor + 1u;
    constexpr uint64_t preroll = 2u * kDxdRate;
    uint64_t total_samples = 0;
    for (const auto& interval : intervals) {
        const uint64_t read_begin = interval.begin > preroll
            ? interval.begin - preroll : 0u;
        const uint64_t begin_sample = read_begin * factor;
        const uint64_t end_sample = std::min<uint64_t>(
            stream.sample_count_per_channel, interval.end * factor);
        if (end_sample > begin_sample) total_samples += end_sample - begin_sample;
    }
    used_workers = adaptive_worker_count(stream.channel_count, intervals.size());
    std::atomic<size_t> next{0};
    std::atomic<uint64_t> completed{0};
    std::atomic<bool> cancelled{false};
    std::mutex merge_mutex;
    std::mutex callback_mutex;
    crossdsd_result shared_result = CROSSDSD_OK;
    std::string shared_message;

    auto worker = [&]() {
        double local_peaks[CROSSDSD_MAX_CHANNELS]{};
        uint64_t local_peak_frames[CROSSDSD_MAX_CHANNELS]{};
        uint64_t local_samples = 0;
        crossdsd_error local_error{};
        local_error.struct_size = sizeof(local_error);
        Reader reader;
        auto local_result = crossdsd_reader_open_file(
            input_path, &reader.value, &local_error);
        crossdsd_decoder_config dc{};
        dc.struct_size = sizeof(dc);
        dc.input_sample_rate_hz = stream.sample_rate_hz;
        dc.channel_count = stream.channel_count;
        dc.level_mode = CROSSDSD_LEVEL_PCM_CONTINUITY;
        dc.filter_profile = CROSSDSD_FILTER_STANDARD;
        std::vector<uint8_t> packed;
        std::vector<int8_t> expanded;
        std::vector<double> pcm;
        try {
            packed.resize(static_cast<size_t>(chunk_bytes * stream.channel_count));
            expanded.resize(static_cast<size_t>(max_bits * stream.channel_count));
            pcm.resize(static_cast<size_t>(output_capacity * stream.channel_count));
        } catch (const std::bad_alloc&) {
            local_result = CROSSDSD_ERROR_OUT_OF_MEMORY;
            std::snprintf(local_error.message, sizeof(local_error.message),
                          "out of memory");
        }
        while (local_result == CROSSDSD_OK && !cancelled.load()) {
            const size_t index = next.fetch_add(1);
            if (index >= intervals.size()) break;
            const auto interval = intervals[index];
            const uint64_t read_begin = interval.begin > preroll
                ? interval.begin - preroll : 0u;
            uint64_t input_pos = read_begin * factor;
            const uint64_t input_end = std::min<uint64_t>(
                stream.sample_count_per_channel, interval.end * factor);
            if (input_pos >= input_end) continue;
            local_result = crossdsd_reader_seek_sample(
                reader.value, input_pos, &local_error);
            if (local_result != CROSSDSD_OK) break;
            Decoder decoder;
            local_result = crossdsd_decoder_create(
                &dc, &decoder.value, &local_error);
            if (local_result != CROSSDSD_OK) break;
            uint64_t output_pos = read_begin;
            while (input_pos < input_end && !cancelled.load()) {
                const uint64_t capacity = std::min(
                    chunk_bytes, (input_end - input_pos) / 8u);
                if (!capacity) break;
                uint64_t bytes = 0;
                uint8_t valid = 0;
                local_result = crossdsd_reader_read_packed(
                    reader.value, packed.data(), capacity, &bytes, &valid,
                    &local_error);
                if (local_result != CROSSDSD_OK) break;
                const uint64_t bits = bytes * 8u;
                for (uint32_t channel = 0; channel < stream.channel_count;
                     ++channel) {
                    local_result = crossdsd_expand_dsd_lsb(
                        packed.data() + static_cast<uint64_t>(channel) * capacity,
                        bits, expanded.data() +
                            static_cast<uint64_t>(channel) * max_bits, max_bits);
                    if (local_result != CROSSDSD_OK) break;
                }
                if (local_result != CROSSDSD_OK) break;
                uint64_t produced = 0;
                local_result = crossdsd_decoder_process(
                    decoder.value, expanded.data(), bits, max_bits, pcm.data(),
                    output_capacity, output_capacity, &produced, &local_error);
                if (local_result != CROSSDSD_OK) break;
                for (uint32_t channel = 0; channel < stream.channel_count;
                     ++channel) {
                    const double* values = pcm.data() +
                        static_cast<uint64_t>(channel) * output_capacity;
                    for (uint64_t frame = 0; frame < produced; ++frame) {
                        const uint64_t absolute = output_pos + frame;
                        if (absolute >= interval.begin && absolute < interval.end) {
                            const double magnitude = std::abs(values[frame]);
                            if (magnitude > local_peaks[channel]) {
                                local_peaks[channel] = magnitude;
                                local_peak_frames[channel] = absolute;
                            }
                        }
                    }
                }
                input_pos += bits;
                output_pos += produced;
                local_samples += bits;
                const uint64_t done = completed.fetch_add(bits) + bits;
                if (progress_callback) {
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    if (!cancelled.load() && !progress_callback(
                            progress_user_data,
                            progress_begin + progress_scale * std::min(
                                1.0, static_cast<double>(done) /
                                    std::max<uint64_t>(1u, total_samples)))) {
                        cancelled.store(true);
                    }
                }
            }
        }
        std::lock_guard<std::mutex> lock(merge_mutex);
        processed_samples += local_samples;
        for (uint32_t channel = 0; channel < stream.channel_count; ++channel) {
            if (local_peaks[channel] > peaks[channel]) {
                peaks[channel] = local_peaks[channel];
                peak_frames[channel] = local_peak_frames[channel];
            }
        }
        if (local_result != CROSSDSD_OK && shared_result == CROSSDSD_OK) {
            shared_result = local_result;
            shared_message = local_error.message;
            cancelled.store(true);
        }
    };
    std::vector<std::thread> workers;
    workers.reserve(used_workers);
    for (uint32_t index = 0; index < used_workers; ++index)
        workers.emplace_back(worker);
    for (auto& item : workers) item.join();
    if (shared_result != CROSSDSD_OK) {
        fail(error, shared_result, shared_message.c_str());
        return shared_result;
    }
    if (cancelled.load()) {
        fail(error, CROSSDSD_ERROR_CANCELLED, "conversion cancelled");
        return CROSSDSD_ERROR_CANCELLED;
    }
    return CROSSDSD_OK;
}

std::array<int8_t, 256> bit_sums() {
    std::array<int8_t, 256> result{};
    for (unsigned value = 0; value < result.size(); ++value) {
        int ones = 0;
        for (unsigned bit = 0; bit < 8; ++bit) ones += (value >> bit) & 1u;
        result[value] = static_cast<int8_t>(ones * 2 - 8);
    }
    return result;
}

}  // namespace

namespace crossdsd::internal {

crossdsd_result run_quick_peak_analysis(
    const char* input_path, const crossdsd_stream_info& stream,
    uint64_t chunk_bytes, crossdsd_progress_callback progress_callback,
    void* progress_user_data, QuickPeakAnalysis& analysis,
    crossdsd_error* error) {
    try {
        Reader reader;
        auto result = crossdsd_reader_open_file(input_path, &reader.value, error);
        if (result != CROSSDSD_OK) return result;
        const uint64_t bytes_per_second = stream.sample_rate_hz / 8u;
        const uint64_t base_bytes = bytes_per_second / kWindowsPerSecond;
        const uint64_t scan_chunk = std::max<uint64_t>(chunk_bytes, base_bytes);
        std::vector<uint8_t> packed(
            static_cast<size_t>(scan_chunk * stream.channel_count));
        const auto lookup = bit_sums();
        std::vector<std::vector<int64_t>> sums(stream.channel_count);
        std::vector<std::vector<double>> fast(stream.channel_count);
        std::vector<int64_t> accum(stream.channel_count, 0);
        std::vector<uint64_t> counts(stream.channel_count, 0);
        constexpr std::array<uint64_t, 3> groups = {4u, 16u, 64u};
        std::vector<std::array<int64_t, 3>> group_sums(stream.channel_count);
        std::vector<std::array<uint64_t, 3>> group_counts(stream.channel_count);
        std::vector<double> maxima(stream.channel_count, 0.0);
        uint64_t scanned = 0;
        const auto scan_start = std::chrono::steady_clock::now();
        while (true) {
            uint64_t bytes = 0;
            uint8_t valid = 0;
            result = crossdsd_reader_read_packed(
                reader.value, packed.data(), scan_chunk, &bytes, &valid, error);
            if (result == CROSSDSD_ERROR_END_OF_STREAM) break;
            if (result != CROSSDSD_OK) return result;
            for (uint32_t channel = 0; channel < stream.channel_count; ++channel) {
                const uint8_t* input = packed.data() +
                    static_cast<uint64_t>(channel) * scan_chunk;
                for (uint64_t index = 0; index < bytes; ++index) {
                    const int64_t value = lookup[input[index]];
                    accum[channel] += value;
                    for (size_t scale = 0; scale < groups.size(); ++scale) {
                        group_sums[channel][scale] += value;
                        if (++group_counts[channel][scale] == groups[scale]) {
                            maxima[channel] = std::max(
                                maxima[channel],
                                std::abs(static_cast<double>(group_sums[channel][scale])) /
                                    static_cast<double>(groups[scale] * 8u));
                            group_sums[channel][scale] = 0;
                            group_counts[channel][scale] = 0;
                        }
                    }
                    if (++counts[channel] == base_bytes) {
                        sums[channel].push_back(accum[channel]);
                        fast[channel].push_back(maxima[channel]);
                        accum[channel] = 0;
                        counts[channel] = 0;
                        maxima[channel] = 0.0;
                    }
                }
            }
            scanned += bytes * 8u;
            if (progress_callback && !progress_callback(
                    progress_user_data,
                    0.05 * std::min(1.0, static_cast<double>(scanned) /
                        stream.sample_count_per_channel))) {
                fail(error, CROSSDSD_ERROR_CANCELLED, "conversion cancelled");
                return CROSSDSD_ERROR_CANCELLED;
            }
        }
        for (uint32_t channel = 0; channel < stream.channel_count; ++channel) {
            if (counts[channel]) {
                sums[channel].push_back(accum[channel]);
                fast[channel].push_back(maxima[channel]);
            }
        }
        analysis.scan_seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - scan_start).count();

        const size_t base_count = sums[0].size();
        const size_t region_count =
            (base_count + kWindowsPerRegion - 1u) / kWindowsPerRegion;
        std::vector<double> scores(region_count, 0.0);
        constexpr std::array<size_t, 3> slow_scales = {1u, 4u, 16u};
        for (uint32_t channel = 0; channel < stream.channel_count; ++channel) {
            for (size_t region = 0; region < region_count; ++region) {
                const size_t begin = region * kWindowsPerRegion;
                const size_t end = std::min(base_count, begin + kWindowsPerRegion);
                for (size_t pos = begin; pos < end; ++pos)
                    scores[region] = std::max(scores[region], fast[channel][pos]);
                for (size_t scale : slow_scales) {
                    for (size_t pos = begin; pos + scale <= end; pos += scale) {
                        int64_t value = 0;
                        for (size_t i = 0; i < scale; ++i) value += sums[channel][pos + i];
                        scores[region] = std::max(
                            scores[region], std::abs(static_cast<double>(value)) /
                                static_cast<double>(base_bytes * scale * 8u));
                    }
                }
            }
        }
        const double maximum = *std::max_element(scores.begin(), scores.end());
        const double threshold = maximum * std::pow(10.0, kGlobalThresholdDb / 20.0);
        std::vector<uint8_t> selected(region_count, 0u);
        auto select = [&](size_t region, size_t before, size_t after) {
            const size_t begin = region > before ? region - before : 0u;
            const size_t end = std::min(region_count, region + after + 1u);
            std::fill(selected.begin() + static_cast<ptrdiff_t>(begin),
                      selected.begin() + static_cast<ptrdiff_t>(end), 1u);
        };
        for (size_t region = 0; region < region_count; ++region)
            if (scores[region] >= threshold)
                select(region, kLeadingMarginRegions, kMarginRegions);
        for (size_t block = 0; block < region_count; block += kRegionsPerBlock) {
            const size_t end = std::min(region_count, block + kRegionsPerBlock);
            std::vector<size_t> ranked(end - block);
            for (size_t i = 0; i < ranked.size(); ++i) ranked[i] = block + i;
            std::sort(ranked.begin(), ranked.end(), [&](size_t a, size_t b) {
                return scores[a] > scores[b];
            });
            std::vector<size_t> local;
            for (size_t region : ranked) {
                const bool close = std::any_of(local.begin(), local.end(), [&](size_t other) {
                    return (region > other ? region - other : other - region) <= 4u;
                });
                if (close) continue;
                local.push_back(region);
                select(region, kMarginRegions, kMarginRegions);
                if (local.size() == kLocalCandidates) break;
            }
        }
        size_t previous = region_count;
        for (size_t region = 0; region < region_count; ++region) {
            if (!selected[region]) continue;
            if (previous != region_count && region > previous + 1u &&
                region - previous - 1u <= 8u)
                std::fill(selected.begin() + static_cast<ptrdiff_t>(previous + 1u),
                          selected.begin() + static_cast<ptrdiff_t>(region), 1u);
            while (region + 1u < region_count && selected[region + 1u]) ++region;
            previous = region;
        }
        std::vector<std::pair<size_t, size_t>> intervals;
        for (size_t region = 0; region < region_count;) {
            if (!selected[region]) { ++region; continue; }
            const size_t begin = region;
            while (region < region_count && selected[region]) ++region;
            intervals.emplace_back(begin, region);
        }

        const uint32_t factor = stream.sample_rate_hz / kDxdRate;
        const uint64_t region_frames = kDxdRate / 4u;
        uint64_t fine_samples = 0;
        uint64_t total_fine_samples = 0;
        std::vector<FrameInterval> frame_intervals;
        for (const auto& interval : intervals) {
            const uint64_t measure_begin = interval.first * region_frames;
            const uint64_t measure_end = std::min<uint64_t>(
                interval.second * region_frames,
                (stream.sample_count_per_channel + factor - 1u) / factor);
            frame_intervals.push_back({measure_begin, measure_end});
            const uint64_t read_begin = measure_begin > 2u * kDxdRate
                ? measure_begin - 2u * kDxdRate : 0u;
            const uint64_t begin_sample = read_begin * factor;
            const uint64_t end_sample = std::min<uint64_t>(
                stream.sample_count_per_channel, measure_end * factor);
            if (end_sample > begin_sample)
                total_fine_samples += end_sample - begin_sample;
        }
        const auto fine_start = std::chrono::steady_clock::now();
        result = analyze_intervals(
            input_path, stream, chunk_bytes, frame_intervals, 0.05, 0.45,
            progress_callback, progress_user_data, analysis.peaks,
            analysis.peak_frames, fine_samples, analysis.worker_count, error);
        if (result != CROSSDSD_OK) return result;
        analysis.fine_seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - fine_start).count();
        analysis.fine_input_fraction = static_cast<double>(fine_samples) /
            stream.sample_count_per_channel;
        analysis.reconstructed_frames =
            (stream.sample_count_per_channel + factor - 1u) / factor;
        if (progress_callback && !progress_callback(progress_user_data, 0.5)) {
            fail(error, CROSSDSD_ERROR_CANCELLED, "conversion cancelled");
            return CROSSDSD_ERROR_CANCELLED;
        }
        fail(error, CROSSDSD_OK, "");
        return CROSSDSD_OK;
    } catch (const std::bad_alloc&) {
        fail(error, CROSSDSD_ERROR_OUT_OF_MEMORY, "out of memory");
        return CROSSDSD_ERROR_OUT_OF_MEMORY;
    }
}

crossdsd_result run_full_peak_analysis(
    const char* input_path, const crossdsd_stream_info& stream,
    uint64_t chunk_bytes, crossdsd_progress_callback progress_callback,
    void* progress_user_data, QuickPeakAnalysis& analysis,
    crossdsd_error* error) {
    try {
        const uint32_t factor = stream.sample_rate_hz / kDxdRate;
        const uint64_t total_frames =
            (stream.sample_count_per_channel + factor - 1u) / factor;
        constexpr uint64_t interval_frames = 30u * kDxdRate;
        std::vector<FrameInterval> intervals;
        for (uint64_t begin = 0; begin < total_frames;
             begin += interval_frames) {
            intervals.push_back({
                begin, std::min(total_frames, begin + interval_frames)});
        }
        uint64_t samples = 0;
        const auto start = std::chrono::steady_clock::now();
        const auto result = analyze_intervals(
            input_path, stream, chunk_bytes, intervals, 0.0, 0.5,
            progress_callback, progress_user_data, analysis.peaks,
            analysis.peak_frames, samples, analysis.worker_count, error);
        analysis.fine_seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
        analysis.fine_input_fraction = static_cast<double>(samples) /
            std::max<uint64_t>(1u, stream.sample_count_per_channel);
        analysis.reconstructed_frames = total_frames;
        return result;
    } catch (const std::bad_alloc&) {
        fail(error, CROSSDSD_ERROR_OUT_OF_MEMORY, "out of memory");
        return CROSSDSD_ERROR_OUT_OF_MEMORY;
    }
}

}  // namespace crossdsd::internal
