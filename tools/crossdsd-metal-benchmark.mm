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

#include "crossdsd/formats.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

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

constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kAttenuationDb = 110.0;

double bessel_i0(double value) {
    double sum = 1.0;
    double term = 1.0;
    const double quarter = value * value / 4.0;
    for (unsigned k = 1; k < 80; ++k) {
        term *= quarter / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term <= sum * 1e-17) break;
    }
    return sum;
}

double kaiser_beta(double attenuation_db) {
    if (attenuation_db > 50.0) return 0.1102 * (attenuation_db - 8.7);
    if (attenuation_db >= 21.0) {
        const double value = attenuation_db - 21.0;
        return 0.5842 * std::pow(value, 0.4) + 0.07886 * value;
    }
    return 0.0;
}

std::vector<float> design_lowpass(
    double sample_rate, double pass_edge, double stop_edge) {
    const double transition = stop_edge - pass_edge;
    const double delta_omega = 2.0 * kPi * transition / sample_rate;
    uint64_t order = static_cast<uint64_t>(
        std::ceil((kAttenuationDb - 8.0) / (2.285 * delta_omega)));
    if (order % 2u != 0) ++order;
    const uint64_t taps = order + 1u;
    const double middle = static_cast<double>(order) / 2.0;
    const double normalized = ((pass_edge + stop_edge) / 2.0) / sample_rate;
    const double beta = kaiser_beta(kAttenuationDb);
    const double denominator = bessel_i0(beta);
    std::vector<double> temporary(static_cast<size_t>(taps));
    double sum = 0.0;
    for (uint64_t index = 0; index < taps; ++index) {
        const double offset = static_cast<double>(index) - middle;
        const double ideal = offset == 0.0
                                 ? 2.0 * normalized
                                 : std::sin(2.0 * kPi * normalized * offset) /
                                       (kPi * offset);
        const double ratio = offset / middle;
        const double window =
            bessel_i0(beta * std::sqrt(std::max(0.0, 1.0 - ratio * ratio))) /
            denominator;
        temporary[static_cast<size_t>(index)] = ideal * window;
        sum += ideal * window;
    }
    std::vector<float> result(static_cast<size_t>(taps));
    for (size_t index = 0; index < result.size(); ++index) {
        result[index] = static_cast<float>(temporary[index] / sum);
    }
    return result;
}

std::vector<std::vector<float>> make_stages(uint32_t input_rate) {
    std::vector<std::vector<float>> stages;
    uint32_t rate = input_rate;
    while (rate > 705'600u) {
        stages.push_back(design_lowpass(rate, 0.20 * rate, 0.30 * rate));
        rate /= 2u;
    }
    stages.push_back(design_lowpass(705'600.0, 20'000.0, 40'000.0));
    return stages;
}

uint64_t fnv1a(uint64_t hash, const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

NSString* kernel_source() {
    return @R"METAL(
#include <metal_stdlib>
using namespace metal;

struct FirParameters {
    uint input_count;
    uint output_count;
    uint input_stride;
    uint output_stride;
    uint tap_count;
    uint channel_count;
    float gain;
};

kernel void fir_decimate(
    device const float* input [[buffer(0)]],
    device float* output [[buffer(1)]],
    device const float* coefficients [[buffer(2)]],
    constant FirParameters& p [[buffer(3)]],
    uint gid [[thread_position_in_grid]])
{
    const uint total = p.output_count * p.channel_count;
    if (gid >= total) return;
    const uint channel = gid / p.output_count;
    const uint output_index = gid - channel * p.output_count;
    const uint newest = output_index * 2u + 1u;
    const uint input_base = channel * p.input_stride;
    float sum = 0.0f;
    const uint usable = min(p.tap_count, newest + 1u);
    for (uint tap = 0; tap < usable; ++tap) {
        sum += coefficients[tap] * input[input_base + newest - tap];
    }
    output[channel * p.output_stride + output_index] = sum * p.gain;
}
)METAL";
}

struct FirParameters {
    uint32_t input_count;
    uint32_t output_count;
    uint32_t input_stride;
    uint32_t output_stride;
    uint32_t tap_count;
    uint32_t channel_count;
    float gain;
};

int print_error(const char* label, NSError* error) {
    std::fprintf(stderr, "%s: %s\n", label,
                 error ? error.localizedDescription.UTF8String : "unknown error");
    return 1;
}

int probe_device(id<MTLDevice> device) {
    id<MTLCommandQueue> queue = device.newCommandQueue;
    const auto start = std::chrono::steady_clock::now();
    constexpr unsigned repetitions = 200;
    for (unsigned index = 0; index < repetitions; ++index) {
        id<MTLCommandBuffer> command = queue.commandBuffer;
        [command commit];
        [command waitUntilCompleted];
    }
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    std::printf(
        "{\"device\":\"%s\",\"registry_id\":%llu,\"unified_memory\":%s,"
        "\"recommended_working_set_bytes\":%llu,\"max_buffer_bytes\":%llu,"
        "\"empty_submission_us\":%.6f}\n",
        device.name.UTF8String,
        static_cast<unsigned long long>(device.registryID),
        device.hasUnifiedMemory ? "true" : "false",
        static_cast<unsigned long long>(device.recommendedMaxWorkingSetSize),
        static_cast<unsigned long long>(device.maxBufferLength),
        seconds * 1e6 / repetitions);
    return 0;
}

int decode_file(id<MTLDevice> device, const char* path, const char* output_path,
                uint32_t requested_channels, uint32_t repetitions) {
    crossdsd_reader* reader = nullptr;
    crossdsd_error parse_error{};
    parse_error.struct_size = sizeof(parse_error);
    auto result = crossdsd_reader_open_file(path, &reader, &parse_error);
    if (result != CROSSDSD_OK) {
        std::fprintf(stderr, "%s: %s\n", path, parse_error.message);
        return 1;
    }
    crossdsd_stream_info info{};
    info.struct_size = sizeof(info);
    crossdsd_reader_get_info(reader, &info);
    if (info.sample_count_per_channel > std::numeric_limits<uint32_t>::max()) {
        std::fprintf(stderr, "prototype input is too long\n");
        crossdsd_reader_close(reader);
        return 1;
    }

    const uint32_t samples = static_cast<uint32_t>(info.sample_count_per_channel);
    const uint32_t source_channels = info.channel_count;
    const uint32_t channels =
        requested_channels == 0 ? source_channels : requested_channels;
    const uint64_t input_values = static_cast<uint64_t>(samples) * channels;
    const uint64_t input_bytes = input_values * sizeof(float);
    if (input_bytes > device.maxBufferLength) {
        std::fprintf(stderr, "input exceeds Metal maximum buffer length\n");
        crossdsd_reader_close(reader);
        return 1;
    }
    const auto wall_start = std::chrono::steady_clock::now();
    id<MTLBuffer> input_buffer =
        [device newBufferWithLength:static_cast<NSUInteger>(input_bytes)
                            options:MTLResourceStorageModeShared];
    if (!input_buffer) {
        std::fprintf(stderr, "cannot allocate Metal input buffer\n");
        crossdsd_reader_close(reader);
        return 1;
    }
    auto* input = static_cast<float*>(input_buffer.contents);
    constexpr uint64_t chunk_bytes = 65'536;
    std::vector<uint8_t> packed(static_cast<size_t>(chunk_bytes * source_channels));
    uint64_t sample_position = 0;
    while (true) {
        uint64_t bytes = 0;
        uint8_t valid_last = 0;
        result = crossdsd_reader_read_packed(
            reader, packed.data(), chunk_bytes, &bytes, &valid_last, &parse_error);
        if (result == CROSSDSD_ERROR_END_OF_STREAM) break;
        if (result != CROSSDSD_OK) {
            std::fprintf(stderr, "cannot read DSD: %s\n", parse_error.message);
            crossdsd_reader_close(reader);
            return 1;
        }
        const uint64_t bits = (bytes - 1u) * 8u + valid_last;
        for (uint32_t channel = 0; channel < source_channels; ++channel) {
            const uint8_t* source =
                packed.data() + static_cast<uint64_t>(channel) * chunk_bytes;
            float* destination = input + static_cast<uint64_t>(channel) * samples +
                                 sample_position;
            for (uint64_t bit = 0; bit < bits; ++bit) {
                destination[bit] =
                    ((source[bit / 8u] >> (bit % 8u)) & 1u) ? 1.0f : -1.0f;
            }
        }
        sample_position += bits;
    }
    crossdsd_reader_close(reader);
    if (sample_position != samples) {
        std::fprintf(stderr, "expanded sample count mismatch\n");
        return 1;
    }
    for (uint32_t channel = source_channels; channel < channels; ++channel) {
        const uint32_t source_channel = channel % source_channels;
        std::memcpy(input + static_cast<uint64_t>(channel) * samples,
                    input + static_cast<uint64_t>(source_channel) * samples,
                    static_cast<size_t>(samples) * sizeof(float));
    }
    const double preparation_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - wall_start).count();

    NSError* metal_error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:kernel_source()
                                                  options:nil
                                                    error:&metal_error];
    if (!library) return print_error("Metal library", metal_error);
    id<MTLFunction> function = [library newFunctionWithName:@"fir_decimate"];
    id<MTLComputePipelineState> pipeline =
        [device newComputePipelineStateWithFunction:function error:&metal_error];
    if (!pipeline) return print_error("Metal pipeline", metal_error);
    id<MTLCommandQueue> queue = device.newCommandQueue;

    const auto stages = make_stages(info.sample_rate_hz);
    std::vector<id<MTLBuffer>> audio_buffers;
    std::vector<id<MTLBuffer>> coefficient_buffers;
    audio_buffers.push_back(input_buffer);
    uint32_t current_count = samples;
    for (size_t stage_index = 0; stage_index < stages.size(); ++stage_index) {
        const uint32_t output_count = current_count / 2u;
        const uint64_t output_bytes =
            static_cast<uint64_t>(output_count) * channels * sizeof(float);
        id<MTLBuffer> output_buffer =
            [device newBufferWithLength:static_cast<NSUInteger>(output_bytes)
                                options:MTLResourceStorageModeShared];
        const auto& coefficients = stages[stage_index];
        id<MTLBuffer> coefficient_buffer =
            [device newBufferWithBytes:coefficients.data()
                                length:coefficients.size() * sizeof(float)
                               options:MTLResourceStorageModeShared];
        if (!output_buffer || !coefficient_buffer) {
            std::fprintf(stderr, "cannot allocate Metal stage buffers\n");
            return 1;
        }
        audio_buffers.push_back(output_buffer);
        coefficient_buffers.push_back(coefficient_buffer);
        current_count = output_count;
    }

    const auto submit_start = std::chrono::steady_clock::now();
    double gpu_seconds = 0.0;
    for (uint32_t repetition = 0; repetition < repetitions; ++repetition) {
        id<MTLCommandBuffer> command = queue.commandBuffer;
        uint32_t stage_input_count = samples;
        for (size_t stage_index = 0; stage_index < stages.size(); ++stage_index) {
            const uint32_t output_count = stage_input_count / 2u;
            const auto& coefficients = stages[stage_index];
            FirParameters parameters{
                stage_input_count,
                output_count,
                stage_input_count,
                output_count,
                static_cast<uint32_t>(coefficients.size()),
                channels,
                stage_index + 1u == stages.size() ? 2.0f : 1.0f,
            };
            id<MTLComputeCommandEncoder> encoder = command.computeCommandEncoder;
            [encoder setComputePipelineState:pipeline];
            [encoder setBuffer:audio_buffers[stage_index] offset:0 atIndex:0];
            [encoder setBuffer:audio_buffers[stage_index + 1u] offset:0 atIndex:1];
            [encoder setBuffer:coefficient_buffers[stage_index] offset:0 atIndex:2];
            [encoder setBytes:&parameters length:sizeof(parameters) atIndex:3];
            const NSUInteger total =
                static_cast<NSUInteger>(output_count) * channels;
            const NSUInteger width = std::min<NSUInteger>(
                pipeline.maxTotalThreadsPerThreadgroup, 256);
            [encoder dispatchThreads:MTLSizeMake(total, 1, 1)
               threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
            [encoder endEncoding];
            stage_input_count = output_count;
        }
        [command commit];
        [command waitUntilCompleted];
        if (command.status != MTLCommandBufferStatusCompleted) {
            return print_error("Metal command", command.error);
        }
        gpu_seconds +=
            command.GPUEndTime > command.GPUStartTime
                ? command.GPUEndTime - command.GPUStartTime
                : 0.0;
    }
    const double submit_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - submit_start).count();
    if (gpu_seconds == 0.0) gpu_seconds = submit_seconds;
    gpu_seconds /= repetitions;
    const double submit_seconds_per_run = submit_seconds / repetitions;
    const double benchmark_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - wall_start).count();
    const double amortized_total_seconds = benchmark_seconds / repetitions;

    const float* output =
        static_cast<const float*>(audio_buffers.back().contents);
    std::vector<uint64_t> hashes(channels, 14695981039346656037ull);
    std::vector<double> peaks(channels, 0.0);
    for (uint32_t channel = 0; channel < channels; ++channel) {
        const float* channel_output =
            output + static_cast<uint64_t>(channel) * current_count;
        hashes[channel] = fnv1a(
            hashes[channel], channel_output,
            static_cast<size_t>(current_count) * sizeof(float));
        for (uint32_t sample = 0; sample < current_count; ++sample) {
            peaks[channel] =
                std::max(peaks[channel], std::abs(static_cast<double>(channel_output[sample])));
        }
    }
    if (output_path) {
        FILE* stream = std::fopen(output_path, "wb");
        if (!stream) {
            std::fprintf(stderr, "cannot open Metal output file\n");
            return 1;
        }
        for (uint32_t sample = 0; sample < current_count; ++sample) {
            for (uint32_t channel = 0; channel < channels; ++channel) {
                const float value =
                    output[static_cast<uint64_t>(channel) * current_count + sample];
                if (std::fwrite(&value, sizeof(value), 1, stream) != 1) {
                    std::fclose(stream);
                    std::fprintf(stderr, "cannot write Metal output\n");
                    return 1;
                }
            }
        }
        std::fclose(stream);
    }

    const double duration = static_cast<double>(samples) / info.sample_rate_hz;
    std::printf(
        "{\"device\":\"%s\",\"input_rate_hz\":%u,\"output_rate_hz\":352800,"
        "\"channels\":%u,\"source_channels\":%u,\"repetitions\":%u,"
        "\"stages\":%zu,\"input_samples\":%u,"
        "\"output_samples\":%u,\"preparation_seconds\":%.9f,"
        "\"submit_wait_seconds\":%.9f,\"gpu_seconds\":%.9f,"
        "\"amortized_total_seconds\":%.9f,\"benchmark_seconds\":%.9f,"
        "\"gpu_realtime_factor\":%.9f,"
        "\"total_realtime_factor\":%.9f,\"channel_hashes\":[",
        device.name.UTF8String, info.sample_rate_hz, channels, source_channels,
        repetitions, stages.size(), samples, current_count, preparation_seconds,
        submit_seconds_per_run, gpu_seconds, amortized_total_seconds,
        benchmark_seconds, duration / gpu_seconds,
        duration / amortized_total_seconds);
    for (uint32_t channel = 0; channel < channels; ++channel) {
        if (channel) std::putchar(',');
        std::printf("\"%016llx\"",
                    static_cast<unsigned long long>(hashes[channel]));
    }
    std::printf("],\"channel_peaks\":[");
    for (uint32_t channel = 0; channel < channels; ++channel) {
        if (channel) std::putchar(',');
        std::printf("%.9g", peaks[channel]);
    }
    std::printf("]}\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            std::fprintf(stderr, "Metal is unavailable\n");
            return 1;
        }
        if (argc == 2 && std::strcmp(argv[1], "--probe") == 0) {
            return probe_device(device);
        }
        const char* output_path = nullptr;
        const char* input_path = nullptr;
        uint32_t requested_channels = 0;
        uint32_t repetitions = 1;
        for (int index = 1; index < argc; ++index) {
            if (std::strncmp(argv[index], "--output=", 9) == 0) {
                output_path = argv[index] + 9;
            } else if (std::strncmp(argv[index], "--channels=", 11) == 0) {
                requested_channels =
                    static_cast<uint32_t>(std::strtoul(argv[index] + 11, nullptr, 10));
            } else if (std::strncmp(argv[index], "--repetitions=", 14) == 0) {
                repetitions =
                    static_cast<uint32_t>(std::strtoul(argv[index] + 14, nullptr, 10));
            } else if (!input_path) {
                input_path = argv[index];
            } else {
                input_path = nullptr;
                break;
            }
        }
        if (!input_path) {
            std::fprintf(
                stderr,
                "usage: crossdsd-metal-benchmark --probe | "
                "[--output=FILE.f32le] [--channels=N] [--repetitions=N] "
                "FILE.dsf\n");
            return 2;
        }
        if (repetitions == 0 || requested_channels > 64) {
            std::fprintf(stderr, "invalid channel count or repetition count\n");
            return 2;
        }
        return decode_file(
            device, input_path, output_path, requested_channels, repetitions);
    }
}
