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
#include "wave_writer_internal.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
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
    uint output_count;
    uint input_stride;
    uint output_stride;
    uint input_prefix;
    uint output_prefix;
    uint valid_history;
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
    const uint input_base = channel * p.input_stride + p.input_prefix;
    float sum = 0.0f;
    const uint usable = min(p.tap_count, newest + 1u + p.valid_history);
    for (uint tap = 0; tap < usable; ++tap) {
        sum += coefficients[tap] * input[input_base + newest - tap];
    }
    output[channel * p.output_stride + p.output_prefix + output_index] =
        sum * p.gain;
}
)METAL";
}

struct FirParameters {
    uint32_t output_count;
    uint32_t input_stride;
    uint32_t output_stride;
    uint32_t input_prefix;
    uint32_t output_prefix;
    uint32_t valid_history;
    uint32_t tap_count;
    uint32_t channel_count;
    float gain;
};

struct AudioBuffer {
    id<MTLBuffer> storage = nil;
    uint32_t stride = 0;
    uint32_t prefix = 0;
    uint32_t valid_history = 0;

    float* channel(uint32_t index) const {
        return static_cast<float*>(storage.contents) +
            static_cast<size_t>(index) * stride;
    }

    void retain_history(uint32_t count, uint32_t channels) {
        const uint32_t keep = std::min(prefix, valid_history + count);
        for (uint32_t index = 0; index < channels; ++index) {
            float* samples = channel(index);
            std::memmove(samples + prefix - keep,
                         samples + prefix + count - keep,
                         keep * sizeof(float));
        }
        valid_history = keep;
    }
};

crossdsd_result process_block(
    id<MTLCommandQueue> queue, id<MTLComputePipelineState> pipeline,
    const std::vector<id<MTLBuffer>>& coefficients,
    std::vector<AudioBuffer>& buffers, uint32_t samples, uint32_t channels,
    double& gpu_seconds, crossdsd_error* error) {
    @autoreleasepool {
        id<MTLCommandBuffer> command = queue.commandBuffer;
        uint32_t input_count = samples;
        const NSUInteger width = std::min<NSUInteger>(
            pipeline.maxTotalThreadsPerThreadgroup, 256);
        for (size_t stage = 0; stage < coefficients.size(); ++stage) {
            const auto& input = buffers[stage];
            const auto& output = buffers[stage + 1u];
            const uint32_t output_count = input_count / 2u;
            const FirParameters parameters{
                output_count, input.stride, output.stride,
                input.prefix, output.prefix, input.valid_history,
                input.prefix + 1u, channels,
                stage + 1u == coefficients.size() ? 2.0f : 1.0f,
            };
            id<MTLComputeCommandEncoder> encoder =
                command.computeCommandEncoder;
            if (!encoder) {
                set_error(error, CROSSDSD_ERROR_IO,
                          "cannot encode Metal block");
                return CROSSDSD_ERROR_IO;
            }
            [encoder setComputePipelineState:pipeline];
            [encoder setBuffer:input.storage offset:0 atIndex:0];
            [encoder setBuffer:output.storage offset:0 atIndex:1];
            [encoder setBuffer:coefficients[stage] offset:0 atIndex:2];
            [encoder setBytes:&parameters length:sizeof(parameters) atIndex:3];
            [encoder dispatchThreads:MTLSizeMake(output_count * channels, 1, 1)
               threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
            [encoder endEncoding];
            input_count = output_count;
        }
        [command commit];
        [command waitUntilCompleted];
        if (command.status != MTLCommandBufferStatusCompleted) {
            set_error(error, CROSSDSD_ERROR_IO,
                      command.error.localizedDescription.UTF8String ?:
                          "Metal block did not complete");
            return CROSSDSD_ERROR_IO;
        }
        if (command.GPUEndTime > command.GPUStartTime) {
            gpu_seconds += command.GPUEndTime - command.GPUStartTime;
        }
        // The GPU has finished reading the history and writing every stage.
        // Keep the newest inputs of each FIR in the prefix for the next block.
        for (size_t stage = 0; stage < coefficients.size(); ++stage) {
            buffers[stage].retain_history(samples >> stage, channels);
        }
        return CROSSDSD_OK;
    }
}

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
    crossdsd_decode_file_report* report, crossdsd_error* error) try {
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
        const auto wall_start = std::chrono::steady_clock::now();
        const uint32_t channels = stream.channel_count;
        const uint64_t samples = stream.sample_count_per_channel;
        double latency = 0.0;
        const auto stages = make_stages(
            stream.sample_rate_hz, config->filter_profile, &latency);
        const uint32_t decimation = 1u << stages.size();
        const uint32_t largest_prefix = static_cast<uint32_t>(
            std::max_element(stages.begin(), stages.end(),
                [](const auto& a, const auto& b) {
                    return a.size() < b.size();
                })->size() - 1u);

        // Bound the expanded input including FIR history. The later stages
        // use successively smaller buffers. Full blocks are aligned to every
        // decimation stage, including DSD512, so their phase never resets.
        constexpr uint64_t alignment = 8u;  // packed bytes = 64 DSD samples
        constexpr uint64_t default_chunk_bytes = 256u * 1024u;
        const uint64_t input_budget = std::min<uint64_t>({
            32u * 1024u * 1024u, device.maxBufferLength,
            device.recommendedMaxWorkingSetSize / 4u});
        const uint64_t sample_capacity = input_budget / (channels * sizeof(float));
        if (sample_capacity < largest_prefix + 8u * alignment) {
            set_error(error, CROSSDSD_ERROR_OUT_OF_MEMORY,
                      "Metal memory budget cannot hold a decode block");
            return CROSSDSD_ERROR_OUT_OF_MEMORY;
        }
        const uint64_t requested = config->packed_chunk_bytes_per_channel == 0
            ? default_chunk_bytes : config->packed_chunk_bytes_per_channel;
        const uint64_t source_bytes_aligned =
            ((samples - 1u) / (8u * alignment) + 1u) * alignment;
        const uint64_t chunk_bytes = std::min({
            std::max(alignment, requested), source_bytes_aligned,
            (sample_capacity - largest_prefix) / 8u}) / alignment * alignment;
        const uint32_t block_samples = static_cast<uint32_t>(chunk_bytes * 8u);
        const uint64_t expected_frames = samples / decimation;

        NSError* metal_error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:kernel_source()
            options:nil error:&metal_error];
        if (!library) {
            set_error(error, CROSSDSD_ERROR_UNSUPPORTED_FORMAT,
                      metal_error.localizedDescription.UTF8String ?:
                          "cannot compile Metal decoder");
            return CROSSDSD_ERROR_UNSUPPORTED_FORMAT;
        }
        id<MTLFunction> function = [library newFunctionWithName:@"fir_decimate"];
        id<MTLComputePipelineState> pipeline = function
            ? [device newComputePipelineStateWithFunction:function error:&metal_error]
            : nil;
        id<MTLCommandQueue> queue = device.newCommandQueue;
        if (!pipeline || !queue) {
            set_error(error, CROSSDSD_ERROR_UNSUPPORTED_FORMAT,
                      metal_error.localizedDescription.UTF8String ?:
                          "cannot initialize Metal decoder");
            return CROSSDSD_ERROR_UNSUPPORTED_FORMAT;
        }

        std::vector<AudioBuffer> buffers(stages.size() + 1u);
        std::vector<id<MTLBuffer>> coefficients;
        for (size_t stage = 0; stage < buffers.size(); ++stage) {
            auto& buffer = buffers[stage];
            if (stage < stages.size()) {
                const auto& taps = stages[stage];
                buffer.prefix = static_cast<uint32_t>(taps.size() - 1u);
                id<MTLBuffer> filter = [device newBufferWithBytes:taps.data()
                    length:taps.size() * sizeof(float) options:MTLResourceStorageModeShared];
                if (!filter) {
                    set_error(error, CROSSDSD_ERROR_OUT_OF_MEMORY,
                              "cannot allocate Metal filter coefficients");
                    return CROSSDSD_ERROR_OUT_OF_MEMORY;
                }
                coefficients.push_back(filter);
            }
            buffer.stride = buffer.prefix + (block_samples >> stage);
            buffer.storage = [device newBufferWithLength:
                static_cast<NSUInteger>(buffer.stride) * channels * sizeof(float)
                options:MTLResourceStorageModeShared];
            if (!buffer.storage) {
                set_error(error, CROSSDSD_ERROR_OUT_OF_MEMORY,
                          "cannot allocate Metal decode buffers");
                return CROSSDSD_ERROR_OUT_OF_MEMORY;
            }
        }
        std::vector<uint8_t> packed(static_cast<size_t>(chunk_bytes * channels));
        std::vector<float> interleaved(
            static_cast<size_t>(block_samples / decimation) * channels);
        const double fixed_gain = config->fixed_gain_enabled
            ? std::pow(10.0, config->fixed_gain_db / 20.0) : 1.0;

        crossdsd_wave_writer_config writer_config{};
        writer_config.struct_size = sizeof(writer_config);
        writer_config.sample_rate_hz = CROSSDSD_DXD_SAMPLE_RATE;
        writer_config.channel_count = channels;
        writer_config.expected_frame_count = expected_frames;
        writer_config.container = config->container;
        writer_config.channel_type = stream.channel_type;
        WriterOwner writer;
        result = crossdsd_wave_writer_open_file(
            output_path, &writer_config, &writer.value, error);
        if (result != CROSSDSD_OK) return result;
        writer.path = output_path;

        double peaks[CROSSDSD_MAX_CHANNELS]{};
        double gpu_seconds = 0.0;
        double writing_seconds = 0.0;
        uint64_t sample_position = 0;
        while (sample_position < samples) {
            uint64_t bytes = 0;
            uint8_t valid_last = 0;
            result = crossdsd_reader_read_packed(
                reader.value, packed.data(), chunk_bytes, &bytes, &valid_last,
                error);
            if (result != CROSSDSD_OK && result != CROSSDSD_ERROR_END_OF_STREAM) {
                return result;
            }
            const uint64_t count = bytes == 0 ? 0 : (bytes - 1u) * 8u + valid_last;
            if (count == 0 || count > samples - sample_position ||
                (count != block_samples && count != samples - sample_position)) {
                set_error(error, CROSSDSD_ERROR_INVALID_SAMPLE_COUNT,
                          "unexpected sample count in Metal decode block");
                return CROSSDSD_ERROR_INVALID_SAMPLE_COUNT;
            }
            const uint32_t frames = static_cast<uint32_t>(count / decimation);
            if (frames != 0) {
                auto& input = buffers.front();
                for (uint32_t channel = 0; channel < channels; ++channel) {
                    const uint8_t* source = packed.data() + channel * chunk_bytes;
                    float* destination = input.channel(channel) + input.prefix;
                    for (uint32_t bit = 0; bit < count; ++bit) {
                        destination[bit] = ((source[bit / 8u] >> (bit % 8u)) & 1u)
                            ? 1.0f : -1.0f;
                    }
                }
                result = process_block(queue, pipeline, coefficients, buffers,
                    static_cast<uint32_t>(count), channels, gpu_seconds, error);
                if (result != CROSSDSD_OK) return result;
                const auto& output = buffers.back();
                for (uint32_t channel = 0; channel < channels; ++channel) {
                    const float* planar = output.channel(channel);
                    for (uint32_t frame = 0; frame < frames; ++frame) {
                        const double value = fixed_gain * planar[frame];
                        interleaved[frame * channels + channel] = static_cast<float>(value);
                        peaks[channel] = std::max(peaks[channel], std::abs(value));
                    }
                }
                const auto writing_start = std::chrono::steady_clock::now();
                result = crossdsd_wave_writer_write(
                    writer.value, interleaved.data(), frames, error);
                writing_seconds += std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - writing_start).count();
                if (result != CROSSDSD_OK) return result;
            }
            sample_position += count;
            if (config->progress_callback && !config->progress_callback(
                    config->progress_user_data,
                    static_cast<double>(sample_position) / samples)) {
                set_error(error, CROSSDSD_ERROR_CANCELLED,
                          "conversion cancelled");
                return CROSSDSD_ERROR_CANCELLED;
            }
        }
        std::vector<uint8_t> metadata;
        if (stream.metadata_status == CROSSDSD_METADATA_ID3 &&
            stream.metadata_size != 0) {
            if (stream.metadata_size > std::numeric_limits<size_t>::max()) {
                set_error(error, CROSSDSD_ERROR_OVERFLOW, "metadata is too large");
                return CROSSDSD_ERROR_OVERFLOW;
            }
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
        const auto writing_start = std::chrono::steady_clock::now();
        result = crossdsd_wave_writer_write_id3(
            writer.value, metadata.data(), metadata.size(), error);
        if (result != CROSSDSD_OK) return result;
        crossdsd_wave_writer_info writer_info{};
        writer_info.struct_size = sizeof(writer_info);
        crossdsd_wave_writer_get_info(writer.value, &writer_info);
        const auto output_size = crossdsd::wave_writer_size_bytes(*writer.value);
        result = crossdsd_wave_writer_close(writer.value, error);
        writer.closed = true;
        writer.value = nullptr;
        if (result != CROSSDSD_OK) return result;
        writer.committed = true;
        writing_seconds += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - writing_start).count();

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
        value.output_frames = writer_info.frame_count;
        value.output_size_bytes = output_size;
        value.metadata_size_bytes =
            stream.metadata_status == CROSSDSD_METADATA_ID3
                ? stream.metadata_size
                : 0u;
        value.metadata_status = stream.metadata_status;
        value.duration_seconds = stream.duration_seconds;
        value.decode_seconds = seconds;
        value.processing_seconds = gpu_seconds;
        value.writing_seconds = writing_seconds;
        value.preparation_seconds = std::max(
            0.0, seconds - value.processing_seconds - value.writing_seconds);
        value.realtime_factor =
            seconds > 0.0 ? stream.duration_seconds / seconds : 0.0;
        value.latency_output_samples = latency;
        std::copy(peaks, peaks + channels, value.channel_peaks);
        value.fixed_gain_enabled = config->fixed_gain_enabled;
        value.requested_fixed_gain_db = config->fixed_gain_enabled
            ? config->fixed_gain_db : 0.0;
        *report = value;
        set_error(error, CROSSDSD_OK, "");
        return CROSSDSD_OK;
    }
} catch (const std::bad_alloc&) {
    set_error(error, CROSSDSD_ERROR_OUT_OF_MEMORY, "out of memory");
    return CROSSDSD_ERROR_OUT_OF_MEMORY;
}
