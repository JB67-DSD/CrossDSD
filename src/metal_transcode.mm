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
#include "metadata_trace.h"
#include "platform_file.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <new>
#include <vector>

namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kAttenuationDb = 110.0;

void set_error(crossdsd_error* error, crossdsd_result code, const char* message) {
    if (!error || error->struct_size < sizeof(crossdsd_error)) return;
    error->code = code;
    error->byte_offset = 0;
    std::snprintf(error->message, sizeof(error->message), "%s", message);
}

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
        const double ideal =
            offset == 0.0
                ? 2.0 * normalized
                : std::sin(2.0 * kPi * normalized * offset) / (kPi * offset);
        const double ratio = offset / middle;
        const double window =
            bessel_i0(
                beta * std::sqrt(std::max(0.0, 1.0 - ratio * ratio))) /
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

std::vector<std::vector<float>> make_stages(
    uint32_t input_rate, crossdsd_filter_profile profile,
    double* latency_output_samples) {
    std::vector<std::vector<float>> stages;
    uint32_t rate = input_rate;
    double latency_seconds = 0.0;
    while (rate > 705'600u) {
        const double stop =
            profile == CROSSDSD_FILTER_RAW ? 0.25 * rate : 0.30 * rate;
        auto stage = design_lowpass(rate, 0.20 * rate, stop);
        latency_seconds +=
            static_cast<double>((stage.size() - 1u) / 2u) / rate;
        stages.push_back(std::move(stage));
        rate /= 2u;
    }
    auto final =
        profile == CROSSDSD_FILTER_RAW
            ? design_lowpass(705'600.0, 150'000.0, 176'400.0)
            : design_lowpass(705'600.0, 20'000.0, 40'000.0);
    latency_seconds +=
        static_cast<double>((final.size() - 1u) / 2u) / 705'600.0;
    stages.push_back(std::move(final));
    *latency_output_samples =
        latency_seconds * CROSSDSD_DXD_SAMPLE_RATE;
    return stages;
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

struct ReaderOwner {
    crossdsd_reader* value = nullptr;
    ~ReaderOwner() { crossdsd_reader_close(value); }
};

struct WriterOwner {
    crossdsd_wave_writer* value = nullptr;
    bool closed = false;
    bool committed = false;
    const char* path = nullptr;
    ~WriterOwner() {
        if (value && !closed) crossdsd_wave_writer_close(value, nullptr);
        if (!committed && path) crossdsd::platform::remove_file(path);
    }
};

}  // namespace

extern "C" bool crossdsd_metal_backend_available(void) {
    @autoreleasepool {
        return MTLCreateSystemDefaultDevice() != nil;
    }
}

extern "C" crossdsd_result crossdsd_decode_file_metal(
    const char* input_path, const char* output_path,
    const crossdsd_decode_file_config* config,
    crossdsd_decode_file_report* report, crossdsd_error* error) {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            set_error(error, CROSSDSD_ERROR_UNSUPPORTED_FORMAT,
                      "Metal is unavailable");
            return CROSSDSD_ERROR_UNSUPPORTED_FORMAT;
        }
        ReaderOwner reader;
        auto result =
            crossdsd_reader_open_file(input_path, &reader.value, error);
        if (result != CROSSDSD_OK) return result;
        crossdsd_stream_info stream{};
        stream.struct_size = sizeof(stream);
        result = crossdsd_reader_get_info(reader.value, &stream);
        if (result != CROSSDSD_OK) return result;
        if (stream.sample_count_per_channel > UINT32_MAX) {
            set_error(error, CROSSDSD_ERROR_OVERFLOW,
                      "Metal input exceeds the prototype sample limit");
            return CROSSDSD_ERROR_OVERFLOW;
        }
        const uint32_t samples =
            static_cast<uint32_t>(stream.sample_count_per_channel);
        const uint32_t channels = stream.channel_count;
        const uint64_t input_bytes =
            static_cast<uint64_t>(samples) * channels * sizeof(float);
        if (input_bytes > device.maxBufferLength ||
            input_bytes > device.recommendedMaxWorkingSetSize / 2u) {
            set_error(error, CROSSDSD_ERROR_OUT_OF_MEMORY,
                      "input is too large for the Metal whole-file backend");
            return CROSSDSD_ERROR_OUT_OF_MEMORY;
        }

        const auto wall_start = std::chrono::steady_clock::now();
        id<MTLBuffer> input_buffer =
            [device newBufferWithLength:static_cast<NSUInteger>(input_bytes)
                                options:MTLResourceStorageModeShared];
        if (!input_buffer) {
            set_error(error, CROSSDSD_ERROR_OUT_OF_MEMORY,
                      "cannot allocate Metal input buffer");
            return CROSSDSD_ERROR_OUT_OF_MEMORY;
        }
        auto* input = static_cast<float*>(input_buffer.contents);
        constexpr uint64_t chunk_bytes = 65'536u;
        std::vector<uint8_t> packed(
            static_cast<size_t>(chunk_bytes * channels));
        uint64_t sample_position = 0;
        while (true) {
            uint64_t bytes = 0;
            uint8_t valid_last = 0;
            result = crossdsd_reader_read_packed(
                reader.value, packed.data(), chunk_bytes, &bytes, &valid_last,
                error);
            if (result == CROSSDSD_ERROR_END_OF_STREAM) break;
            if (result != CROSSDSD_OK) return result;
            const uint64_t bits = (bytes - 1u) * 8u + valid_last;
            for (uint32_t channel = 0; channel < channels; ++channel) {
                const uint8_t* source =
                    packed.data() +
                    static_cast<uint64_t>(channel) * chunk_bytes;
                float* destination =
                    input + static_cast<uint64_t>(channel) * samples +
                    sample_position;
                for (uint64_t bit = 0; bit < bits; ++bit) {
                    destination[bit] =
                        ((source[bit / 8u] >> (bit % 8u)) & 1u)
                            ? 1.0f
                            : -1.0f;
                }
            }
            sample_position += bits;
            if (config->progress_callback &&
                !config->progress_callback(
                    config->progress_user_data,
                    0.5 * static_cast<double>(sample_position) / samples)) {
                set_error(error, CROSSDSD_ERROR_CANCELLED,
                          "conversion cancelled");
                return CROSSDSD_ERROR_CANCELLED;
            }
        }
        if (sample_position != samples) {
            set_error(error, CROSSDSD_ERROR_INVALID_SAMPLE_COUNT,
                      "expanded Metal sample count mismatch");
            return CROSSDSD_ERROR_INVALID_SAMPLE_COUNT;
        }

        NSError* metal_error = nil;
        id<MTLLibrary> library =
            [device newLibraryWithSource:kernel_source()
                                  options:nil
                                    error:&metal_error];
        if (!library) {
            set_error(error, CROSSDSD_ERROR_UNSUPPORTED_FORMAT,
                      metal_error.localizedDescription.UTF8String);
            return CROSSDSD_ERROR_UNSUPPORTED_FORMAT;
        }
        id<MTLFunction> function =
            [library newFunctionWithName:@"fir_decimate"];
        id<MTLComputePipelineState> pipeline =
            [device newComputePipelineStateWithFunction:function
                                                   error:&metal_error];
        if (!pipeline) {
            set_error(error, CROSSDSD_ERROR_UNSUPPORTED_FORMAT,
                      metal_error.localizedDescription.UTF8String);
            return CROSSDSD_ERROR_UNSUPPORTED_FORMAT;
        }
        id<MTLCommandQueue> queue = device.newCommandQueue;
        id<MTLCommandBuffer> command = queue.commandBuffer;

        double latency = 0.0;
        const auto stages = make_stages(
            stream.sample_rate_hz, config->filter_profile, &latency);
        std::vector<id<MTLBuffer>> audio_buffers{input_buffer};
        std::vector<id<MTLBuffer>> coefficient_buffers;
        uint32_t current_count = samples;
        for (size_t stage_index = 0; stage_index < stages.size();
             ++stage_index) {
            const uint32_t output_count = current_count / 2u;
            const uint64_t output_bytes =
                static_cast<uint64_t>(output_count) * channels *
                sizeof(float);
            id<MTLBuffer> output_buffer =
                [device
                    newBufferWithLength:static_cast<NSUInteger>(output_bytes)
                                options:MTLResourceStorageModeShared];
            const auto& coefficients = stages[stage_index];
            id<MTLBuffer> coefficient_buffer =
                [device newBufferWithBytes:coefficients.data()
                                    length:coefficients.size() * sizeof(float)
                                   options:MTLResourceStorageModeShared];
            if (!output_buffer || !coefficient_buffer) {
                set_error(error, CROSSDSD_ERROR_OUT_OF_MEMORY,
                          "cannot allocate Metal stage buffers");
                return CROSSDSD_ERROR_OUT_OF_MEMORY;
            }
            audio_buffers.push_back(output_buffer);
            coefficient_buffers.push_back(coefficient_buffer);
            FirParameters parameters{
                current_count,
                output_count,
                current_count,
                output_count,
                static_cast<uint32_t>(coefficients.size()),
                channels,
                stage_index + 1u == stages.size() ? 2.0f : 1.0f,
            };
            id<MTLComputeCommandEncoder> encoder =
                command.computeCommandEncoder;
            [encoder setComputePipelineState:pipeline];
            [encoder setBuffer:audio_buffers[stage_index]
                        offset:0
                       atIndex:0];
            [encoder setBuffer:output_buffer offset:0 atIndex:1];
            [encoder setBuffer:coefficient_buffer offset:0 atIndex:2];
            [encoder setBytes:&parameters
                       length:sizeof(parameters)
                      atIndex:3];
            const NSUInteger total =
                static_cast<NSUInteger>(output_count) * channels;
            const NSUInteger width = std::min<NSUInteger>(
                pipeline.maxTotalThreadsPerThreadgroup, 256);
            [encoder dispatchThreads:MTLSizeMake(total, 1, 1)
               threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
            [encoder endEncoding];
            current_count = output_count;
        }
        [command commit];
        [command waitUntilCompleted];
        if (command.status != MTLCommandBufferStatusCompleted) {
            set_error(
                error, CROSSDSD_ERROR_IO,
                command.error.localizedDescription.UTF8String);
            return CROSSDSD_ERROR_IO;
        }
        if (config->progress_callback &&
            !config->progress_callback(config->progress_user_data, 0.5)) {
            set_error(error, CROSSDSD_ERROR_CANCELLED,
                      "conversion cancelled");
            return CROSSDSD_ERROR_CANCELLED;
        }
        const double gpu_seconds =
            command.GPUEndTime > command.GPUStartTime
                ? command.GPUEndTime - command.GPUStartTime
                : 0.0;

        const auto writing_start = std::chrono::steady_clock::now();
        crossdsd_wave_writer_config writer_config{};
        writer_config.struct_size = sizeof(writer_config);
        writer_config.sample_rate_hz = CROSSDSD_DXD_SAMPLE_RATE;
        writer_config.channel_count = channels;
        writer_config.expected_frame_count = current_count;
        writer_config.container = config->container;
        writer_config.channel_type = stream.channel_type;
        WriterOwner writer;
        result = crossdsd_wave_writer_open_file(
            output_path, &writer_config, &writer.value, error);
        if (result != CROSSDSD_OK) return result;
        writer.path = output_path;
        const float* output =
            static_cast<const float*>(audio_buffers.back().contents);
        constexpr uint64_t output_block_frames = 65'536u;
        std::vector<float> interleaved(
            static_cast<size_t>(output_block_frames * channels));
        double peaks[CROSSDSD_MAX_CHANNELS]{};
        for (uint64_t position = 0; position < current_count;
             position += output_block_frames) {
            const uint64_t count =
                std::min<uint64_t>(
                    output_block_frames, current_count - position);
            for (uint64_t frame = 0; frame < count; ++frame) {
                for (uint32_t channel = 0; channel < channels; ++channel) {
                    const float value =
                        output[static_cast<uint64_t>(channel) * current_count +
                               position + frame];
                    interleaved[frame * channels + channel] = value;
                    peaks[channel] =
                        std::max(peaks[channel], std::abs(static_cast<double>(value)));
                }
            }
            result = crossdsd_wave_writer_write(
                writer.value, interleaved.data(), count, error);
            if (result != CROSSDSD_OK) return result;
            if (config->progress_callback &&
                !config->progress_callback(
                    config->progress_user_data,
                    0.5 + 0.5 *
                              static_cast<double>(position + count) /
                              current_count)) {
                set_error(error, CROSSDSD_ERROR_CANCELLED,
                          "conversion cancelled");
                return CROSSDSD_ERROR_CANCELLED;
            }
        }
        std::vector<uint8_t> metadata;
        if (stream.metadata_status == CROSSDSD_METADATA_ID3 &&
            stream.metadata_size != 0) {
            metadata.resize(static_cast<size_t>(stream.metadata_size));
            uint64_t metadata_read = 0;
            result = crossdsd_reader_read_metadata(
                reader.value, metadata.data(), stream.metadata_size,
                &metadata_read, error);
            if (result != CROSSDSD_OK) return result;
            metadata.resize(static_cast<size_t>(metadata_read));
        }
        const char* input_rate = stream.sample_rate_hz == 2'822'400u
            ? "DSD64" : stream.sample_rate_hz == 5'644'800u
                ? "DSD128" : stream.sample_rate_hz == 11'289'600u
                    ? "DSD256" : "DSD512";
        const char* profile = config->filter_profile == CROSSDSD_FILTER_RAW
            ? "Raw" : "Standard";
        char settings[160]{};
        std::snprintf(settings, sizeof(settings),
                      "CrossDSD %s - %s to DXD - %s filter",
                      CROSSDSD_VERSION, input_rate, profile);
        metadata = crossdsd_id3_with_encoder_settings(
            metadata, settings, "DXD");
        result = crossdsd_wave_writer_write_id3(
            writer.value, metadata.data(), metadata.size(), error);
        if (result != CROSSDSD_OK) return result;
        crossdsd_wave_writer_info writer_info{};
        writer_info.struct_size = sizeof(writer_info);
        crossdsd_wave_writer_get_info(writer.value, &writer_info);
        result = crossdsd_wave_writer_close(writer.value, error);
        writer.closed = true;
        writer.value = nullptr;
        if (result != CROSSDSD_OK) return result;
        writer.committed = true;

        const double seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - wall_start).count();
        crossdsd_decode_file_report value{};
        value.struct_size = sizeof(value);
        value.api_version = CROSSDSD_CONVERT_API_VERSION;
        value.input_sample_rate_hz = stream.sample_rate_hz;
        value.output_sample_rate_hz = CROSSDSD_DXD_SAMPLE_RATE;
        value.channel_count = channels;
        value.stage_count = static_cast<uint32_t>(stages.size());
        value.filter_profile = config->filter_profile;
        value.container = writer_info.container;
        value.backend = CROSSDSD_BACKEND_METAL;
        value.input_samples_per_channel =
            stream.sample_count_per_channel;
        value.output_frames = current_count;
        value.output_size_bytes =
            writer_info.data_size_bytes + writer_info.metadata_size_bytes +
            (writer_info.container == CROSSDSD_WAVE_RF64 ? 92u : 56u);
        value.metadata_size_bytes =
            stream.metadata_status == CROSSDSD_METADATA_ID3
                ? stream.metadata_size
                : 0u;
        value.metadata_status = stream.metadata_status;
        value.duration_seconds = stream.duration_seconds;
        value.decode_seconds = seconds;
        value.processing_seconds = gpu_seconds;
        value.writing_seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - writing_start).count();
        value.preparation_seconds = std::max(
            0.0, seconds - value.processing_seconds - value.writing_seconds);
        value.realtime_factor =
            seconds > 0.0 ? stream.duration_seconds / seconds : 0.0;
        value.latency_output_samples = latency;
        std::copy(peaks, peaks + channels, value.channel_peaks);
        *report = value;
        set_error(error, CROSSDSD_OK, "");
        return CROSSDSD_OK;
    }
}
