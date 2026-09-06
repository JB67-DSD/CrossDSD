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
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

uint64_t fnv1a(uint64_t hash, const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

int fail(const char* operation, const crossdsd_error& error) {
    std::fprintf(stderr, "%s: %s at byte %llu (%s)\n", operation, error.message,
                 static_cast<unsigned long long>(error.byte_offset),
                 crossdsd_result_string(error.code));
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    crossdsd_level_mode level = CROSSDSD_LEVEL_PCM_CONTINUITY;
    crossdsd_filter_profile profile = CROSSDSD_FILTER_STANDARD;
    uint64_t chunk_bytes = 4093;
    const char* path = nullptr;
    const char* output_path = nullptr;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--level=dsd-raw") == 0) {
            level = CROSSDSD_LEVEL_DSD_RAW;
        } else if (std::strcmp(argv[index], "--profile=raw") == 0) {
            profile = CROSSDSD_FILTER_RAW;
        } else if (std::strncmp(argv[index], "--chunk-bytes=", 14) == 0) {
            try {
                chunk_bytes = std::stoull(argv[index] + 14);
            } catch (...) {
                std::fprintf(stderr, "invalid chunk size\n");
                return 2;
            }
        } else if (std::strncmp(argv[index], "--output=", 9) == 0) {
            output_path = argv[index] + 9;
        } else if (!path) {
            path = argv[index];
        } else {
            path = nullptr;
            break;
        }
    }
    if (!path || chunk_bytes == 0) {
        std::fprintf(stderr,
                     "usage: crossdsd-decode-check [--level=dsd-raw] "
                     "[--profile=raw] "
                     "[--chunk-bytes=N] [--output=FILE.f64le] FILE.dsf\n");
        return 2;
    }
    crossdsd_error error{};
    error.struct_size = sizeof(error);
    crossdsd_reader* reader = nullptr;
    auto result = crossdsd_reader_open_file(path, &reader, &error);
    if (result != CROSSDSD_OK) return fail("open", error);

    crossdsd_stream_info stream{};
    stream.struct_size = sizeof(stream);
    crossdsd_reader_get_info(reader, &stream);
    crossdsd_decoder_config config{};
    config.struct_size = sizeof(config);
    config.input_sample_rate_hz = stream.sample_rate_hz;
    config.channel_count = stream.channel_count;
    config.level_mode = level;
    config.filter_profile = profile;
    crossdsd_decoder* decoder = nullptr;
    result = crossdsd_decoder_create(&config, &decoder, &error);
    if (result != CROSSDSD_OK) {
        crossdsd_reader_close(reader);
        return fail("create decoder", error);
    }
    FILE* output_file = nullptr;
    if (output_path) {
        output_file = std::fopen(output_path, "wb");
        if (!output_file) {
            std::fprintf(stderr, "cannot open validation output\n");
            crossdsd_decoder_destroy(decoder);
            crossdsd_reader_close(reader);
            return 1;
        }
    }
    crossdsd_decoder_info decoder_info{};
    decoder_info.struct_size = sizeof(decoder_info);
    crossdsd_decoder_get_info(decoder, &decoder_info);

    const uint64_t packed_size = chunk_bytes * stream.channel_count;
    const uint64_t expanded_stride = chunk_bytes * 8u;
    const uint64_t expanded_size = expanded_stride * stream.channel_count;
    const uint64_t output_capacity =
        expanded_stride / decoder_info.decimation_factor + 1u;
    std::vector<uint8_t> packed(static_cast<size_t>(packed_size));
    std::vector<int8_t> expanded(static_cast<size_t>(expanded_size));
    std::vector<double> output(
        static_cast<size_t>(output_capacity * stream.channel_count));
    std::vector<uint64_t> hashes(stream.channel_count, 14695981039346656037ull);
    std::vector<double> peaks(stream.channel_count, 0.0);

    const auto decode_start = std::chrono::steady_clock::now();
    while (true) {
        uint64_t bytes = 0;
        uint8_t valid_last = 0;
        result = crossdsd_reader_read_packed(
            reader, packed.data(), chunk_bytes, &bytes, &valid_last, &error);
        if (result == CROSSDSD_ERROR_END_OF_STREAM) break;
        if (result != CROSSDSD_OK) {
            if (output_file) std::fclose(output_file);
            crossdsd_decoder_destroy(decoder);
            crossdsd_reader_close(reader);
            return fail("read packed DSD", error);
        }
        const uint64_t bits = (bytes - 1u) * 8u + valid_last;
        for (uint32_t channel = 0; channel < stream.channel_count; ++channel) {
            result = crossdsd_expand_dsd_lsb(
                packed.data() + static_cast<uint64_t>(channel) * chunk_bytes,
                bits,
                expanded.data() + static_cast<uint64_t>(channel) * expanded_stride,
                expanded_stride);
            if (result != CROSSDSD_OK) {
                std::fprintf(stderr, "cannot expand DSD\n");
                if (output_file) std::fclose(output_file);
                crossdsd_decoder_destroy(decoder);
                crossdsd_reader_close(reader);
                return 1;
            }
        }
        uint64_t produced = 0;
        result = crossdsd_decoder_process(
            decoder, expanded.data(), bits, expanded_stride, output.data(),
            output_capacity, output_capacity, &produced, &error);
        if (result != CROSSDSD_OK) {
            if (output_file) std::fclose(output_file);
            crossdsd_decoder_destroy(decoder);
            crossdsd_reader_close(reader);
            return fail("decode", error);
        }
        for (uint32_t channel = 0; channel < stream.channel_count; ++channel) {
            const double* channel_output =
                output.data() + static_cast<uint64_t>(channel) * output_capacity;
            hashes[channel] = fnv1a(
                hashes[channel], channel_output,
                static_cast<size_t>(produced) * sizeof(double));
            for (uint64_t sample = 0; sample < produced; ++sample) {
                peaks[channel] = std::max(peaks[channel], std::abs(channel_output[sample]));
            }
        }
        if (output_file) {
            for (uint64_t sample = 0; sample < produced; ++sample) {
                for (uint32_t channel = 0; channel < stream.channel_count; ++channel) {
                    const double value =
                        output[static_cast<uint64_t>(channel) * output_capacity + sample];
                    if (std::fwrite(&value, sizeof(value), 1, output_file) != 1) {
                        std::fprintf(stderr, "cannot write validation output\n");
                        std::fclose(output_file);
                        crossdsd_decoder_destroy(decoder);
                        crossdsd_reader_close(reader);
                        return 1;
                    }
                }
            }
        }
    }
    const auto decode_end = std::chrono::steady_clock::now();
    const double decode_seconds =
        std::chrono::duration<double>(decode_end - decode_start).count();
    crossdsd_decoder_get_info(decoder, &decoder_info);
    double passband_min_db = 0.0;
    double passband_max_db = -std::numeric_limits<double>::infinity();
    for (unsigned step = 0; step <= 400; ++step) {
        double magnitude = 0.0;
        double phase = 0.0;
        crossdsd_decoder_frequency_response(
            decoder, 20'000.0 * step / 400.0, &magnitude, &phase);
        const double db = 20.0 * std::log10(std::max(magnitude, 1e-300));
        passband_min_db = std::min(passband_min_db, db);
        passband_max_db = std::max(passband_max_db, db);
    }
    double stopband_max_db = -std::numeric_limits<double>::infinity();
    for (unsigned step = 0; step <= 1000; ++step) {
        double magnitude = 0.0;
        double phase = 0.0;
        crossdsd_decoder_frequency_response(
            decoder, 40'000.0 + (176'400.0 - 40'000.0) * step / 1000.0,
            &magnitude, &phase);
        stopband_max_db = std::max(
            stopband_max_db, 20.0 * std::log10(std::max(magnitude, 1e-300)));
    }
    std::printf(
        "{\"input_rate_hz\":%u,\"output_rate_hz\":%u,\"channels\":%u,"
        "\"stages\":%u,\"gain\":%.17g,\"latency_output_samples\":%.12f,"
        "\"passband_ripple_db\":%.12g,\"stopband_max_db\":%.12g,"
        "\"decode_seconds\":%.9f,\"realtime_factor\":%.9f,"
        "\"input_samples\":%llu,\"output_samples\":%llu,\"channel_hashes\":[",
        decoder_info.input_sample_rate_hz, decoder_info.output_sample_rate_hz,
        decoder_info.channel_count, decoder_info.stage_count, decoder_info.applied_gain,
        decoder_info.latency_output_samples,
        passband_max_db - passband_min_db, stopband_max_db,
        decode_seconds,
        (static_cast<double>(decoder_info.input_samples_consumed_per_channel) /
         decoder_info.input_sample_rate_hz) / decode_seconds,
        static_cast<unsigned long long>(decoder_info.input_samples_consumed_per_channel),
        static_cast<unsigned long long>(decoder_info.output_samples_produced_per_channel));
    for (uint32_t channel = 0; channel < stream.channel_count; ++channel) {
        if (channel) std::putchar(',');
        std::printf("\"%016llx\"", static_cast<unsigned long long>(hashes[channel]));
    }
    std::printf("],\"channel_peaks\":[");
    for (uint32_t channel = 0; channel < stream.channel_count; ++channel) {
        if (channel) std::putchar(',');
        std::printf("%.17g", peaks[channel]);
    }
    std::printf("]}\n");

    if (output_file && std::fclose(output_file) != 0) {
        std::fprintf(stderr, "cannot close validation output\n");
        crossdsd_decoder_destroy(decoder);
        crossdsd_reader_close(reader);
        return 1;
    }
    crossdsd_decoder_destroy(decoder);
    crossdsd_reader_close(reader);
    return 0;
}
