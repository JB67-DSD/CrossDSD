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

#include "crossdsd/dsp.h"
#include "crossdsd/formats.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint32_t kDxdRate = 352'800u;
/*
 * 2.5 ms is exactly representable as a whole number of packed bytes at DSD64,
 * DSD128 and DSD256 (882, 1764 and 3528 bytes respectively). This avoids the
 * cumulative timeline drift caused by rounded 1 ms windows.
 */
constexpr uint32_t kBaseWindowsPerSecond = 400u;
constexpr uint32_t kRegionBaseWindows = 100u;
constexpr uint32_t kDefaultMarginRegions = 4u;
constexpr double kDefaultCandidateRelativeDb = -12.0;
constexpr double kAdaptiveGlobalRelativeDb = -4.0;
constexpr uint32_t kAdaptiveBlockRegions = 120u;  // 30 seconds.
constexpr uint32_t kAdaptiveLocalCandidates = 3u;
constexpr uint32_t kAdaptiveLocalMarginRegions = 2u;  // 0.5 seconds.

struct Reader {
    crossdsd_reader* value = nullptr;
    ~Reader() { crossdsd_reader_close(value); }
};

struct Decoder {
    crossdsd_decoder* value = nullptr;
    ~Decoder() { crossdsd_decoder_destroy(value); }
};

struct FineResult {
    double peak = 0.0;
    uint64_t peak_frame = 0;
    uint32_t peak_channel = 0;
    uint64_t input_samples = 0;
    double seconds = 0.0;
    bool ok = true;
    std::string message;
};

std::array<int8_t, 256> bit_sums() {
    std::array<int8_t, 256> result{};
    for (unsigned value = 0; value < result.size(); ++value) {
        int ones = 0;
        for (unsigned bit = 0; bit < 8; ++bit) {
            ones += (value >> bit) & 1u;
        }
        result[value] = static_cast<int8_t>(ones * 2 - 8);
    }
    return result;
}

std::vector<uint32_t> benchmark_workers() {
    const char* value = std::getenv("CROSSDSD_PEAK_BENCH_WORKERS");
    if (!value || !*value) return {1u, 2u, 4u, 8u};
    std::vector<uint32_t> result;
    std::string text(value);
    size_t begin = 0;
    while (begin <= text.size()) {
        const size_t end = text.find(',', begin);
        const std::string token = text.substr(
            begin, end == std::string::npos ? end : end - begin);
        const unsigned long parsed = std::strtoul(token.c_str(), nullptr, 10);
        if (parsed > 0 && parsed <= 64) result.push_back(
            static_cast<uint32_t>(parsed));
        if (end == std::string::npos) break;
        begin = end + 1u;
    }
    if (result.empty()) result.push_back(1u);
    return result;
}

bool open(
    const char* path, Reader& reader, crossdsd_stream_info& info,
    crossdsd_error& error) {
    if (crossdsd_reader_open_file(path, &reader.value, &error) != CROSSDSD_OK) {
        std::cerr << error.message << '\n';
        return false;
    }
    info.struct_size = sizeof(info);
    if (crossdsd_reader_get_info(reader.value, &info) != CROSSDSD_OK) {
        std::cerr << "cannot inspect DSF input\n";
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 5) {
        std::cerr
            << "usage: crossdsd-quickpeak-benchmark INPUT.dsf "
               "[THRESHOLD_DB|adaptive [MARGIN_REGIONS [--quick-only]]]\n";
        return 2;
    }
    const bool adaptive =
        argc >= 3 && std::string(argv[2]) == "adaptive";
    const double candidate_relative_db =
        adaptive ? kAdaptiveGlobalRelativeDb
        : argc >= 3 ? std::strtod(argv[2], nullptr)
                  : kDefaultCandidateRelativeDb;
    const uint32_t margin_regions =
        argc >= 4 ? static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 10))
                  : kDefaultMarginRegions;
    const bool quick_only =
        argc >= 5 && std::string(argv[4]) == "--quick-only";
    crossdsd_error error{};
    error.struct_size = sizeof(error);
    Reader quick_reader;
    crossdsd_stream_info stream{};
    if (!open(argv[1], quick_reader, stream, error)) return 1;

    const uint64_t bytes_per_second = stream.sample_rate_hz / 8u;
    const uint64_t base_bytes = std::max<uint64_t>(
        1u, bytes_per_second / kBaseWindowsPerSecond);
    const uint64_t chunk_bytes = base_bytes * 1024u;
    if (chunk_bytes >
        std::numeric_limits<size_t>::max() / stream.channel_count) {
        std::cerr << "input has too many channels\n";
        return 1;
    }
    const auto lookup = bit_sums();
    std::vector<uint8_t> packed(
        static_cast<size_t>(chunk_bytes * stream.channel_count));
    std::vector<std::vector<int64_t>> base_sums(stream.channel_count);
    std::vector<std::vector<double>> base_fast_scores(stream.channel_count);
    std::vector<int64_t> accumulators(stream.channel_count, 0);
    std::vector<uint64_t> accumulator_bytes(stream.channel_count, 0);
    constexpr std::array<uint64_t, 3> fast_group_bytes = {4u, 16u, 64u};
    std::vector<std::array<int64_t, 3>> fast_sums(stream.channel_count);
    std::vector<std::array<uint64_t, 3>> fast_counts(stream.channel_count);
    std::vector<double> fast_maxima(stream.channel_count, 0.0);

    const auto quick_start = std::chrono::steady_clock::now();
    while (true) {
        uint64_t bytes = 0;
        uint8_t valid_bits = 0;
        const auto status = crossdsd_reader_read_packed(
            quick_reader.value, packed.data(), chunk_bytes, &bytes,
            &valid_bits, &error);
        if (status == CROSSDSD_ERROR_END_OF_STREAM) break;
        if (status != CROSSDSD_OK) {
            std::cerr << error.message << '\n';
            return 1;
        }
        for (uint32_t channel = 0; channel < stream.channel_count; ++channel) {
            const uint8_t* values =
                packed.data() + static_cast<uint64_t>(channel) * chunk_bytes;
            for (uint64_t index = 0; index < bytes; ++index) {
                const int64_t signed_sum = lookup[values[index]];
                accumulators[channel] += signed_sum;
                for (size_t scale = 0; scale < fast_group_bytes.size();
                     ++scale) {
                    fast_sums[channel][scale] += signed_sum;
                    if (++fast_counts[channel][scale] ==
                        fast_group_bytes[scale]) {
                        fast_maxima[channel] = std::max(
                            fast_maxima[channel],
                            std::abs(static_cast<double>(
                                fast_sums[channel][scale])) /
                                static_cast<double>(
                                    fast_group_bytes[scale] * 8u));
                        fast_sums[channel][scale] = 0;
                        fast_counts[channel][scale] = 0;
                    }
                }
                if (++accumulator_bytes[channel] == base_bytes) {
                    base_sums[channel].push_back(accumulators[channel]);
                    base_fast_scores[channel].push_back(
                        fast_maxima[channel]);
                    accumulators[channel] = 0;
                    accumulator_bytes[channel] = 0;
                    fast_maxima[channel] = 0.0;
                }
            }
        }
    }
    for (uint32_t channel = 0; channel < stream.channel_count; ++channel) {
        if (accumulator_bytes[channel] != 0) {
            base_sums[channel].push_back(accumulators[channel]);
            base_fast_scores[channel].push_back(fast_maxima[channel]);
        }
    }
    const size_t base_count = base_sums[0].size();
    const size_t region_count =
        (base_count + kRegionBaseWindows - 1u) / kRegionBaseWindows;
    std::vector<double> scores(region_count, 0.0);
    constexpr std::array<size_t, 3> scales = {1u, 4u, 16u};
    for (uint32_t channel = 0; channel < stream.channel_count; ++channel) {
        for (size_t region = 0; region < region_count; ++region) {
            const size_t begin = region * kRegionBaseWindows;
            const size_t end =
                std::min(base_count, begin + kRegionBaseWindows);
            for (size_t position = begin; position < end; ++position) {
                scores[region] = std::max(
                    scores[region],
                    base_fast_scores[channel][position]);
            }
            for (size_t scale : scales) {
                for (size_t position = begin;
                     position + scale <= end; position += scale) {
                    int64_t sum = 0;
                    for (size_t index = 0; index < scale; ++index) {
                        sum += base_sums[channel][position + index];
                    }
                    const double denominator =
                        static_cast<double>(base_bytes * scale * 8u);
                    scores[region] = std::max(
                        scores[region],
                        std::abs(static_cast<double>(sum)) / denominator);
                }
            }
        }
    }
    const double maximum_score =
        *std::max_element(scores.begin(), scores.end());
    const double threshold =
        maximum_score * std::pow(10.0, candidate_relative_db / 20.0);
    std::vector<uint8_t> candidates(region_count, 0u);
    for (size_t region = 0; region < region_count; ++region) {
        if (scores[region] < threshold) continue;
        const size_t leading_margin =
            adaptive ? kAdaptiveLocalMarginRegions * 4u : margin_regions;
        const size_t begin =
            region > leading_margin ? region - leading_margin : 0u;
        const size_t end =
            std::min(region_count, region + margin_regions + 1u);
        std::fill(
            candidates.begin() + static_cast<ptrdiff_t>(begin),
            candidates.begin() + static_cast<ptrdiff_t>(end), 1u);
    }
    if (adaptive) {
        for (size_t block_begin = 0; block_begin < region_count;
             block_begin += kAdaptiveBlockRegions) {
            const size_t block_end =
                std::min(region_count, block_begin + kAdaptiveBlockRegions);
            std::vector<size_t> ranked(block_end - block_begin);
            for (size_t index = 0; index < ranked.size(); ++index) {
                ranked[index] = block_begin + index;
            }
            std::sort(
                ranked.begin(), ranked.end(),
                [&scores](size_t left, size_t right) {
                    return scores[left] > scores[right];
                });
            std::vector<size_t> selected;
            for (size_t region : ranked) {
                const bool too_close = std::any_of(
                    selected.begin(), selected.end(),
                    [region, margin_regions](size_t other) {
                        const size_t distance =
                            region > other ? region - other : other - region;
                        return distance <= margin_regions * 2u;
                    });
                if (too_close) continue;
                selected.push_back(region);
                const size_t begin =
                    region > kAdaptiveLocalMarginRegions
                        ? region - kAdaptiveLocalMarginRegions
                        : 0u;
                const size_t end =
                    std::min(
                        region_count,
                        region + kAdaptiveLocalMarginRegions + 1u);
                std::fill(
                    candidates.begin() + static_cast<ptrdiff_t>(begin),
                    candidates.begin() + static_cast<ptrdiff_t>(end), 1u);
                if (selected.size() == kAdaptiveLocalCandidates) break;
            }
        }
        /*
         * Restarting an exact reconstruction interval requires two seconds of
         * pre-roll. Joining gaps no longer than that is both cheaper and safer
         * than warming a second decoder from scratch.
         */
        constexpr size_t merge_gap_regions = 8u;
        size_t previous = region_count;
        for (size_t region = 0; region < region_count; ++region) {
            if (!candidates[region]) continue;
            if (previous != region_count &&
                region > previous + 1u &&
                region - previous - 1u <= merge_gap_regions) {
                std::fill(
                    candidates.begin() + static_cast<ptrdiff_t>(previous + 1u),
                    candidates.begin() + static_cast<ptrdiff_t>(region), 1u);
            }
            while (region + 1u < region_count && candidates[region + 1u]) {
                ++region;
            }
            previous = region;
        }
    }
    const size_t candidate_count =
        static_cast<size_t>(std::count(candidates.begin(), candidates.end(), 1u));
    std::vector<std::pair<size_t, size_t>> candidate_intervals;
    for (size_t region = 0; region < region_count;) {
        if (!candidates[region]) {
            ++region;
            continue;
        }
        const size_t begin = region;
        while (region < region_count && candidates[region]) ++region;
        candidate_intervals.emplace_back(begin, region);
    }
    const double quick_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - quick_start).count();
    if (quick_only) {
        std::cout << std::fixed << std::setprecision(9)
                  << "input=" << argv[1] << '\n'
                  << "duration_seconds=" << stream.duration_seconds << '\n'
                  << "quick_seconds=" << quick_seconds << '\n'
                  << "region_seconds=0.250000000\n"
                  << "candidate_threshold_db=" << candidate_relative_db << '\n'
                  << "adaptive=" << (adaptive ? 1 : 0) << '\n'
                  << "margin_regions=" << margin_regions << '\n'
                  << "candidate_regions=" << candidate_count << '\n'
                  << "total_regions=" << region_count << '\n'
                  << "candidate_fraction="
                  << (region_count == 0
                          ? 0.0
                          : static_cast<double>(candidate_count) / region_count)
                  << '\n'
                  << "candidate_interval_count="
                  << candidate_intervals.size() << '\n'
                  << "candidate_intervals_seconds=";
        for (size_t index = 0; index < candidate_intervals.size(); ++index) {
            if (index != 0) std::cout << ',';
            std::cout
                << candidate_intervals[index].first * 0.25 << '-'
                << std::min(
                       stream.duration_seconds,
                       candidate_intervals[index].second * 0.25);
        }
        std::cout << '\n';
        return 0;
    }

    Reader exact_reader;
    crossdsd_stream_info exact_stream{};
    if (!open(argv[1], exact_reader, exact_stream, error)) return 1;
    crossdsd_decoder_config decoder_config{};
    decoder_config.struct_size = sizeof(decoder_config);
    decoder_config.input_sample_rate_hz = stream.sample_rate_hz;
    decoder_config.channel_count = stream.channel_count;
    decoder_config.level_mode = CROSSDSD_LEVEL_PCM_CONTINUITY;
    decoder_config.filter_profile = CROSSDSD_FILTER_STANDARD;
    Decoder decoder;
    if (crossdsd_decoder_create(
            &decoder_config, &decoder.value, &error) != CROSSDSD_OK) {
        std::cerr << error.message << '\n';
        return 1;
    }
    crossdsd_decoder_info decoder_info{};
    decoder_info.struct_size = sizeof(decoder_info);
    crossdsd_decoder_get_info(decoder.value, &decoder_info);
    const uint64_t expanded_stride = chunk_bytes * 8u;
    const uint64_t output_capacity =
        expanded_stride / decoder_info.decimation_factor + 1u;
    std::vector<int8_t> expanded(
        static_cast<size_t>(expanded_stride * stream.channel_count));
    std::vector<double> reconstructed(
        static_cast<size_t>(output_capacity * stream.channel_count));
    std::vector<double> channel_peaks(stream.channel_count, 0.0);
    double exact_peak = 0.0;
    double selected_peak = 0.0;
    uint64_t exact_peak_frame = 0;
    uint32_t exact_peak_channel = 0;
    uint64_t absolute_frame = 0;
    const uint64_t region_frames =
        static_cast<uint64_t>(kDxdRate) * kRegionBaseWindows /
        kBaseWindowsPerSecond;

    const auto exact_start = std::chrono::steady_clock::now();
    while (true) {
        uint64_t bytes = 0;
        uint8_t valid_bits = 0;
        const auto status = crossdsd_reader_read_packed(
            exact_reader.value, packed.data(), chunk_bytes, &bytes,
            &valid_bits, &error);
        if (status == CROSSDSD_ERROR_END_OF_STREAM) break;
        if (status != CROSSDSD_OK) {
            std::cerr << error.message << '\n';
            return 1;
        }
        const uint64_t bits = (bytes - 1u) * 8u + valid_bits;
        for (uint32_t channel = 0; channel < stream.channel_count; ++channel) {
            crossdsd_expand_dsd_lsb(
                packed.data() + static_cast<uint64_t>(channel) * chunk_bytes,
                bits,
                expanded.data() +
                    static_cast<uint64_t>(channel) * expanded_stride,
                expanded_stride);
        }
        uint64_t produced = 0;
        if (crossdsd_decoder_process(
                decoder.value, expanded.data(), bits, expanded_stride,
                reconstructed.data(), output_capacity, output_capacity,
                &produced, &error) != CROSSDSD_OK) {
            std::cerr << error.message << '\n';
            return 1;
        }
        for (uint32_t channel = 0; channel < stream.channel_count; ++channel) {
            const double* values =
                reconstructed.data() +
                static_cast<uint64_t>(channel) * output_capacity;
            for (uint64_t frame = 0; frame < produced; ++frame) {
                const double value = std::abs(values[frame]);
                channel_peaks[channel] =
                    std::max(channel_peaks[channel], value);
                if (value > exact_peak) {
                    exact_peak = value;
                    exact_peak_frame = absolute_frame + frame;
                    exact_peak_channel = channel;
                }
                const size_t region = static_cast<size_t>(
                    (absolute_frame + frame) / region_frames);
                if (region < candidates.size() && candidates[region]) {
                    selected_peak = std::max(selected_peak, value);
                }
            }
        }
        absolute_frame += produced;
    }
    const double exact_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - exact_start).count();

    constexpr uint64_t fine_preroll_frames = 2u * kDxdRate;
    auto run_fine = [&](const std::vector<std::pair<size_t, size_t>>& intervals,
                        uint32_t requested_workers) {
        FineResult combined;
        if (intervals.empty()) return combined;
        const uint32_t worker_count = std::max<uint32_t>(
            1u, std::min<uint32_t>(requested_workers, intervals.size()));
        std::atomic<size_t> next_interval{0};
        std::mutex merge_mutex;
        const auto start = std::chrono::steady_clock::now();
        auto worker = [&]() {
            FineResult local;
            crossdsd_error local_error{};
            local_error.struct_size = sizeof(local_error);
            Reader local_reader;
            crossdsd_stream_info local_stream{};
            if (!open(argv[1], local_reader, local_stream, local_error)) {
                local.ok = false;
                local.message = local_error.message;
            }
            std::vector<uint8_t> local_packed(
                static_cast<size_t>(chunk_bytes * stream.channel_count));
            std::vector<int8_t> local_expanded(
                static_cast<size_t>(expanded_stride * stream.channel_count));
            std::vector<double> local_reconstructed(
                static_cast<size_t>(output_capacity * stream.channel_count));
            while (local.ok) {
                const size_t interval_index = next_interval.fetch_add(1);
                if (interval_index >= intervals.size()) break;
                const auto& interval = intervals[interval_index];
                const uint64_t measure_begin_frame = interval.first * region_frames;
                const uint64_t measure_end_frame = std::min<uint64_t>(
                    interval.second * region_frames,
                    (stream.sample_count_per_channel +
                     decoder_info.decimation_factor - 1u) /
                        decoder_info.decimation_factor);
                const uint64_t read_begin_frame =
                    measure_begin_frame > fine_preroll_frames
                        ? measure_begin_frame - fine_preroll_frames : 0u;
                uint64_t input_position =
                    read_begin_frame * decoder_info.decimation_factor;
                const uint64_t read_end_sample = std::min<uint64_t>(
                    stream.sample_count_per_channel,
                    measure_end_frame * decoder_info.decimation_factor);
                if (input_position >= read_end_sample) continue;
                if (crossdsd_reader_seek_sample(
                        local_reader.value, input_position, &local_error) !=
                    CROSSDSD_OK) {
                    local.ok = false;
                    local.message = local_error.message;
                    break;
                }
                Decoder local_decoder;
                if (crossdsd_decoder_create(
                        &decoder_config, &local_decoder.value, &local_error) !=
                    CROSSDSD_OK) {
                    local.ok = false;
                    local.message = local_error.message;
                    break;
                }
                uint64_t output_position = read_begin_frame;
                while (input_position < read_end_sample) {
                    const uint64_t capacity = std::min(
                        chunk_bytes, (read_end_sample - input_position) / 8u);
                    if (!capacity) break;
                    uint64_t bytes = 0;
                    uint8_t valid = 0;
                    if (crossdsd_reader_read_packed(
                            local_reader.value, local_packed.data(), capacity,
                            &bytes, &valid, &local_error) != CROSSDSD_OK) {
                        local.ok = false;
                        local.message = local_error.message;
                        break;
                    }
                    const uint64_t bits = bytes * 8u;
                    for (uint32_t channel = 0; channel < stream.channel_count;
                         ++channel) {
                        if (crossdsd_expand_dsd_lsb(
                                local_packed.data() +
                                    static_cast<uint64_t>(channel) * capacity,
                                bits,
                                local_expanded.data() +
                                    static_cast<uint64_t>(channel) * expanded_stride,
                                expanded_stride) != CROSSDSD_OK) {
                            local.ok = false;
                            local.message = "cannot expand selective DSD input";
                            break;
                        }
                    }
                    if (!local.ok) break;
                    uint64_t produced = 0;
                    if (crossdsd_decoder_process(
                            local_decoder.value, local_expanded.data(), bits,
                            expanded_stride, local_reconstructed.data(),
                            output_capacity, output_capacity, &produced,
                            &local_error) != CROSSDSD_OK) {
                        local.ok = false;
                        local.message = local_error.message;
                        break;
                    }
                    for (uint32_t channel = 0; channel < stream.channel_count;
                         ++channel) {
                        const double* values = local_reconstructed.data() +
                            static_cast<uint64_t>(channel) * output_capacity;
                        for (uint64_t frame = 0; frame < produced; ++frame) {
                            const uint64_t global_frame = output_position + frame;
                            if (global_frame < measure_begin_frame ||
                                global_frame >= measure_end_frame) continue;
                            const double value = std::abs(values[frame]);
                            if (value > local.peak ||
                                (value == local.peak &&
                                 (channel < local.peak_channel ||
                                  (channel == local.peak_channel &&
                                   global_frame < local.peak_frame)))) {
                                local.peak = value;
                                local.peak_frame = global_frame;
                                local.peak_channel = channel;
                            }
                        }
                    }
                    input_position += bits;
                    output_position += produced;
                    local.input_samples += bits;
                }
            }
            std::lock_guard<std::mutex> lock(merge_mutex);
            combined.input_samples += local.input_samples;
            if (!local.ok && combined.ok) {
                combined.ok = false;
                combined.message = local.message;
            }
            if (local.peak > combined.peak ||
                (local.peak == combined.peak &&
                 (local.peak_channel < combined.peak_channel ||
                  (local.peak_channel == combined.peak_channel &&
                   local.peak_frame < combined.peak_frame)))) {
                combined.peak = local.peak;
                combined.peak_frame = local.peak_frame;
                combined.peak_channel = local.peak_channel;
            }
        };
        std::vector<std::thread> workers;
        workers.reserve(worker_count);
        for (uint32_t index = 0; index < worker_count; ++index)
            workers.emplace_back(worker);
        for (auto& thread : workers) thread.join();
        combined.seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
        return combined;
    };

    const std::vector<uint32_t> worker_counts = benchmark_workers();
    const std::string benchmark_scope =
        std::getenv("CROSSDSD_PEAK_BENCH_SCOPE")
            ? std::getenv("CROSSDSD_PEAK_BENCH_SCOPE") : "both";
    const bool benchmark_quick = benchmark_scope != "full";
    const bool benchmark_full = benchmark_scope != "quick";
    std::vector<FineResult> fine_results;
    if (benchmark_quick) {
        for (uint32_t workers : worker_counts) {
            fine_results.push_back(run_fine(candidate_intervals, workers));
            if (!fine_results.back().ok) {
                std::cerr << fine_results.back().message << '\n';
                return 1;
            }
        }
    }
    const FineResult fine = benchmark_quick ? fine_results.front()
                                            : run_fine(candidate_intervals, 1u);
    const double fine_peak = fine.peak;
    const uint64_t fine_peak_frame = fine.peak_frame;
    const uint32_t fine_peak_channel = fine.peak_channel;
    const uint64_t fine_input_samples = fine.input_samples;
    const double fine_seconds = fine.seconds;

    std::vector<std::pair<size_t, size_t>> exhaustive_intervals;
    constexpr size_t exhaustive_regions_per_interval = 120u;
    for (size_t begin = 0; begin < region_count;
         begin += exhaustive_regions_per_interval) {
        exhaustive_intervals.emplace_back(
            begin, std::min(region_count,
                            begin + exhaustive_regions_per_interval));
    }
    std::vector<FineResult> exhaustive_results;
    if (benchmark_full) {
        for (uint32_t workers : worker_counts) {
            exhaustive_results.push_back(run_fine(exhaustive_intervals, workers));
            if (!exhaustive_results.back().ok) {
                std::cerr << exhaustive_results.back().message << '\n';
                return 1;
            }
        }
    }
    const size_t peak_region =
        static_cast<size_t>(exact_peak_frame / region_frames);
    const bool peak_selected =
        peak_region < candidates.size() && candidates[peak_region] != 0;
    const double selected_error_db =
        selected_peak > 0.0
            ? 20.0 * std::log10(selected_peak / exact_peak)
            : -std::numeric_limits<double>::infinity();
    const double fine_error_db =
        fine_peak > 0.0
            ? 20.0 * std::log10(fine_peak / exact_peak)
            : -std::numeric_limits<double>::infinity();
    const double candidate_fraction =
        region_count == 0
            ? 0.0
            : static_cast<double>(candidate_count) / region_count;

    std::cout << std::fixed << std::setprecision(9)
              << "input=" << argv[1] << '\n'
              << "duration_seconds=" << stream.duration_seconds << '\n'
              << "quick_seconds=" << quick_seconds << '\n'
              << "exact_seconds=" << exact_seconds << '\n'
              << "quick_to_exact_ratio="
              << (exact_seconds > 0.0 ? quick_seconds / exact_seconds : 0.0)
              << '\n'
              << "region_seconds=0.250000000\n"
              << "candidate_threshold_db=" << candidate_relative_db << '\n'
              << "adaptive=" << (adaptive ? 1 : 0) << '\n'
              << "margin_regions=" << margin_regions << '\n'
              << "candidate_regions=" << candidate_count << '\n'
              << "total_regions=" << region_count << '\n'
              << "candidate_fraction=" << candidate_fraction << '\n'
              << "candidate_interval_count=" << candidate_intervals.size()
              << '\n'
              << "candidate_intervals_seconds=";
    for (size_t index = 0; index < candidate_intervals.size(); ++index) {
        if (index != 0) std::cout << ',';
        std::cout
            << candidate_intervals[index].first * 0.25 << '-'
            << std::min(
                   stream.duration_seconds,
                   candidate_intervals[index].second * 0.25);
    }
    std::cout << '\n'
              << "exact_peak=" << exact_peak << '\n'
              << "exact_peak_dbfs=" << 20.0 * std::log10(exact_peak) << '\n'
              << "exact_peak_seconds="
              << static_cast<double>(exact_peak_frame) / kDxdRate << '\n'
              << "exact_peak_frame=" << exact_peak_frame << '\n'
              << "exact_peak_channel=" << exact_peak_channel << '\n'
              << "exact_peak_region_selected=" << (peak_selected ? 1 : 0)
              << '\n'
              << "selected_peak=" << selected_peak << '\n'
              << "selected_peak_dbfs="
              << 20.0 * std::log10(selected_peak) << '\n'
              << "selected_error_db=" << selected_error_db << '\n'
              << "fine_seconds=" << fine_seconds << '\n'
              << "fine_input_fraction="
              << static_cast<double>(fine_input_samples) /
                     stream.sample_count_per_channel
              << '\n'
              << "fine_peak=" << fine_peak << '\n'
              << "fine_peak_dbfs=" << 20.0 * std::log10(fine_peak) << '\n'
              << "fine_peak_seconds="
              << static_cast<double>(fine_peak_frame) / kDxdRate << '\n'
              << "fine_peak_channel=" << fine_peak_channel << '\n'
              << "fine_error_db=" << fine_error_db << '\n';
    for (size_t index = 0; index < worker_counts.size(); ++index) {
        const uint32_t effective_quick = std::min<uint32_t>(
            worker_counts[index], candidate_intervals.size());
        const uint32_t effective_full = std::min<uint32_t>(
            worker_counts[index], exhaustive_intervals.size());
        if (benchmark_quick) std::cout
            << "parallel_quick_requested_workers=" << worker_counts[index]
            << ",effective_workers=" << effective_quick
            << ",seconds=" << fine_results[index].seconds
            << ",speedup="
            << (fine_results[index].seconds > 0.0
                    ? fine_results.front().seconds /
                          fine_results[index].seconds : 0.0)
            << ",peak=" << fine_results[index].peak
            << ",frame=" << fine_results[index].peak_frame
            << ",channel=" << fine_results[index].peak_channel << '\n';
        if (benchmark_full) std::cout
            << "parallel_full_requested_workers=" << worker_counts[index]
            << ",effective_workers=" << effective_full
            << ",seconds=" << exhaustive_results[index].seconds
            << ",speedup="
            << (exhaustive_results[index].seconds > 0.0
                    ? exhaustive_results.front().seconds /
                          exhaustive_results[index].seconds : 0.0)
            << ",peak=" << exhaustive_results[index].peak
            << ",frame=" << exhaustive_results[index].peak_frame
            << ",channel=" << exhaustive_results[index].peak_channel << '\n';
    }
    const bool fine_matches =
        (exact_peak == 0.0 && fine_peak == 0.0) ||
        std::abs(fine_error_db) < 1e-9;
    const auto exact_match = [&](const FineResult& value) {
        return value.peak == exact_peak &&
               value.peak_frame == exact_peak_frame &&
               value.peak_channel == exact_peak_channel;
    };
    const bool all_parallel_match =
        std::all_of(fine_results.begin(), fine_results.end(), exact_match) &&
        std::all_of(exhaustive_results.begin(), exhaustive_results.end(),
                    exact_match);
    return peak_selected && selected_peak == exact_peak && fine_matches &&
                   all_parallel_match ? 0 : 3;
}
