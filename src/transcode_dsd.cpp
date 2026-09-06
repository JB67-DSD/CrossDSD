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
#include "quick_peak_analysis.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <deque>
#include <thread>
#include <vector>

namespace {

void set_error(crossdsd_error* error, crossdsd_result code, const char* message) {
    if (!error || error->struct_size < sizeof(crossdsd_error)) return;
    error->code = code;
    error->byte_offset = 0;
    std::snprintf(error->message, sizeof(error->message), "%s", message);
}

struct ReaderOwner {
    crossdsd_reader* value = nullptr;
    ~ReaderOwner() { crossdsd_reader_close(value); }
};

struct DecoderOwner {
    crossdsd_decoder* value = nullptr;
    ~DecoderOwner() { crossdsd_decoder_destroy(value); }
};

struct EncoderOwner {
    crossdsd_encoder* value = nullptr;
    ~EncoderOwner() { crossdsd_encoder_destroy(value); }
};

struct WriterOwner {
    crossdsd_dsf_writer* value = nullptr;
    bool closed = false;
    bool committed = false;
    const char* path = nullptr;
    ~WriterOwner() {
        if (value && !closed) crossdsd_dsf_writer_close(value, nullptr);
        if (!committed && path) crossdsd::platform::remove_file(path);
    }
};

uint32_t rate_hz(crossdsd_dsd_rate rate) {
    switch (rate) {
        case CROSSDSD_DSD64: return 2'822'400u;
        case CROSSDSD_DSD128: return 5'644'800u;
        case CROSSDSD_DSD256: return 11'289'600u;
        case CROSSDSD_DSD512: return 22'579'200u;
        default: return 0;
    }
}

crossdsd_modulator_profile modulator_profile(crossdsd_dsd_rate rate) {
    switch (rate) {
        case CROSSDSD_DSD128:
            return CROSSDSD_MODULATOR_DSD128_CRFB_ORDER5_HINF_1_30;
        case CROSSDSD_DSD256:
            return CROSSDSD_MODULATOR_DSD256_CRFB_ORDER7_HINF_1_20;
        case CROSSDSD_DSD512:
            return CROSSDSD_MODULATOR_DSD512_CRFB_ORDER7_HINF_1_20;
        default:
            return CROSSDSD_MODULATOR_CRFB_ORDER7_HINF_1_32;
    }
}

struct PassResult {
    uint64_t frames = 0;
    double peaks[CROSSDSD_MAX_CHANNELS]{};
};

crossdsd_result run_pass(
    const char* input_path, const crossdsd_stream_info& stream,
    uint64_t chunk_bytes, double gain, crossdsd_encoder* encoder,
    crossdsd_dsf_writer* writer, double progress_begin, double progress_scale,
    crossdsd_progress_callback progress_callback, void* progress_user_data,
    PassResult& pass, crossdsd_error* error) {
    ReaderOwner reader;
    auto result = crossdsd_reader_open_file(input_path, &reader.value, error);
    if (result != CROSSDSD_OK) return result;

    crossdsd_decoder_config decoder_config{};
    decoder_config.struct_size = sizeof(decoder_config);
    decoder_config.input_sample_rate_hz = stream.sample_rate_hz;
    decoder_config.channel_count = stream.channel_count;
    decoder_config.level_mode = CROSSDSD_LEVEL_PCM_CONTINUITY;
    decoder_config.filter_profile = CROSSDSD_FILTER_STANDARD;
    DecoderOwner decoder;
    result = crossdsd_decoder_create(
        &decoder_config, &decoder.value, error);
    if (result != CROSSDSD_OK) return result;
    crossdsd_decoder_info decoder_info{};
    decoder_info.struct_size = sizeof(decoder_info);
    result = crossdsd_decoder_get_info(decoder.value, &decoder_info);
    if (result != CROSSDSD_OK) return result;

    if (chunk_bytes >
            std::numeric_limits<size_t>::max() / stream.channel_count ||
        chunk_bytes * stream.channel_count >
            std::numeric_limits<size_t>::max() / 8u) {
        set_error(error, CROSSDSD_ERROR_OVERFLOW,
                  "transcode buffers are too large");
        return CROSSDSD_ERROR_OVERFLOW;
    }
    const uint64_t expanded_stride = chunk_bytes * 8u;
    const uint64_t output_capacity =
        expanded_stride / decoder_info.decimation_factor + 1u;
    if (output_capacity >
        std::numeric_limits<size_t>::max() / stream.channel_count) {
        set_error(error, CROSSDSD_ERROR_OVERFLOW,
                  "transcode output buffer is too large");
        return CROSSDSD_ERROR_OVERFLOW;
    }

    std::vector<uint8_t> packed(
        static_cast<size_t>(chunk_bytes * stream.channel_count));
    std::vector<int8_t> expanded(
        static_cast<size_t>(expanded_stride * stream.channel_count));
    std::vector<double> planar(
        static_cast<size_t>(output_capacity * stream.channel_count));

    crossdsd_encoder_info encoder_info{};
    uint64_t packed_stride = 0;
    std::vector<uint8_t> encoded;
    if (encoder) {
        encoder_info.struct_size = sizeof(encoder_info);
        result = crossdsd_encoder_get_info(encoder, &encoder_info);
        if (result != CROSSDSD_OK) return result;
        const uint64_t bytes_per_frame =
            encoder_info.interpolation_factor / 8u;
        if (output_capacity >
            (std::numeric_limits<uint64_t>::max() - 1u) / bytes_per_frame) {
            set_error(error, CROSSDSD_ERROR_OVERFLOW,
                      "transcode encoded buffer is too large");
            return CROSSDSD_ERROR_OVERFLOW;
        }
        packed_stride = output_capacity * bytes_per_frame + 1u;
        if (packed_stride >
            std::numeric_limits<size_t>::max() / stream.channel_count) {
            set_error(error, CROSSDSD_ERROR_OVERFLOW,
                      "transcode encoded buffer is too large");
            return CROSSDSD_ERROR_OVERFLOW;
        }
        encoded.resize(
            static_cast<size_t>(packed_stride * stream.channel_count));
    }

    uint64_t consumed = 0;
    while (true) {
        uint64_t bytes = 0;
        uint8_t valid_last = 0;
        result = crossdsd_reader_read_packed(
            reader.value, packed.data(), chunk_bytes, &bytes, &valid_last,
            error);
        if (result == CROSSDSD_ERROR_END_OF_STREAM) break;
        if (result != CROSSDSD_OK) return result;
        const uint64_t bits = (bytes - 1u) * 8u + valid_last;
        for (uint32_t channel = 0; channel < stream.channel_count; ++channel) {
            result = crossdsd_expand_dsd_lsb(
                packed.data() + static_cast<uint64_t>(channel) * chunk_bytes,
                bits,
                expanded.data() +
                    static_cast<uint64_t>(channel) * expanded_stride,
                expanded_stride);
            if (result != CROSSDSD_OK) {
                set_error(error, result, "cannot expand packed DSD");
                return result;
            }
        }
        uint64_t produced = 0;
        result = crossdsd_decoder_process(
            decoder.value, expanded.data(), bits, expanded_stride,
            planar.data(), output_capacity, output_capacity, &produced,
            error);
        if (result != CROSSDSD_OK) return result;
        for (uint32_t channel = 0; channel < stream.channel_count; ++channel) {
            double* samples =
                planar.data() +
                static_cast<uint64_t>(channel) * output_capacity;
            for (uint64_t frame = 0; frame < produced; ++frame) {
                pass.peaks[channel] =
                    std::max(pass.peaks[channel], std::abs(samples[frame]));
                if (encoder) samples[frame] *= gain;
            }
        }
        pass.frames += produced;
        if (encoder && produced != 0) {
            uint64_t encoded_bytes = 0;
            result = crossdsd_encoder_process_f64(
                encoder, planar.data(), produced, output_capacity,
                encoded.data(), packed_stride, packed_stride,
                &encoded_bytes, error);
            if (result != CROSSDSD_OK) return result;
            if (encoded_bytes != 0) {
                for (uint32_t channel = 1; channel < stream.channel_count;
                     ++channel) {
                    std::memmove(
                        encoded.data() +
                            static_cast<uint64_t>(channel) * encoded_bytes,
                        encoded.data() +
                            static_cast<uint64_t>(channel) * packed_stride,
                        static_cast<size_t>(encoded_bytes));
                }
                result = crossdsd_dsf_writer_write_packed(
                    writer, encoded.data(), encoded_bytes, 8, error);
                if (result != CROSSDSD_OK) return result;
            }
        }
        consumed += bits;
        if (progress_callback) {
            const double source_fraction =
                stream.sample_count_per_channel == 0
                    ? 1.0
                    : std::min(
                          1.0, static_cast<double>(consumed) /
                                   stream.sample_count_per_channel);
            if (!progress_callback(
                    progress_user_data,
                    progress_begin + progress_scale * source_fraction)) {
                set_error(error, CROSSDSD_ERROR_CANCELLED,
                          "conversion cancelled");
                return CROSSDSD_ERROR_CANCELLED;
            }
        }
    }
    return CROSSDSD_OK;
}

struct PipelineBlock {
    std::vector<double> planar;
    uint64_t frames = 0;
    uint64_t source_bits = 0;
};

struct PipelineProfile {
    double read_seconds = 0.0;
    double expand_seconds = 0.0;
    double decode_seconds = 0.0;
    double producer_wait_seconds = 0.0;
    double consumer_wait_seconds = 0.0;
    double gain_seconds = 0.0;
    double encode_seconds = 0.0;
    double compact_seconds = 0.0;
    double write_seconds = 0.0;
    uint64_t blocks = 0;
};

double elapsed_seconds(std::chrono::steady_clock::time_point begin) {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now() - begin).count();
}

void write_pipeline_profile(
    const char* path, const crossdsd_stream_info& stream,
    const crossdsd_encoder_info& encoder, const PipelineProfile& profile) {
    if (!path || !*path) return;
    std::FILE* output = std::fopen(path, "wb");
    if (!output) return;
    std::fprintf(output,
        "{\n"
        "  \"input_sample_rate_hz\": %u,\n"
        "  \"output_sample_rate_hz\": %u,\n"
        "  \"channel_count\": %u,\n"
        "  \"blocks\": %llu,\n"
        "  \"read_seconds\": %.9f,\n"
        "  \"expand_seconds\": %.9f,\n"
        "  \"decode_seconds\": %.9f,\n"
        "  \"producer_wait_seconds\": %.9f,\n"
        "  \"consumer_wait_seconds\": %.9f,\n"
        "  \"gain_seconds\": %.9f,\n"
        "  \"encode_seconds\": %.9f,\n"
        "  \"compact_seconds\": %.9f,\n"
        "  \"write_seconds\": %.9f\n"
        "}\n",
        stream.sample_rate_hz, encoder.output_sample_rate_hz,
        stream.channel_count,
        static_cast<unsigned long long>(profile.blocks),
        profile.read_seconds, profile.expand_seconds, profile.decode_seconds,
        profile.producer_wait_seconds, profile.consumer_wait_seconds,
        profile.gain_seconds, profile.encode_seconds, profile.compact_seconds,
        profile.write_seconds);
    std::fclose(output);
}

crossdsd_result run_pass_pipelined(
    const char* input_path, const crossdsd_stream_info& stream,
    uint64_t chunk_bytes, double gain, crossdsd_encoder* encoder,
    crossdsd_dsf_writer* writer, double progress_begin, double progress_scale,
    crossdsd_progress_callback progress_callback, void* progress_user_data,
    PassResult& pass, crossdsd_error* error) {
    if (!encoder || !writer) return run_pass(
        input_path, stream, chunk_bytes, gain, encoder, writer,
        progress_begin, progress_scale, progress_callback, progress_user_data,
        pass, error);
    std::deque<PipelineBlock> queue;
    constexpr size_t queue_capacity = 2u;
    std::mutex mutex;
    std::condition_variable readable;
    std::condition_variable writable;
    bool producer_finished = false;
    bool stopped = false;
    crossdsd_result producer_result = CROSSDSD_OK;
    crossdsd_error producer_error{};
    producer_error.struct_size = sizeof(producer_error);
    PipelineProfile profile;
    const char* profile_path = std::getenv("CROSSDSD_CPU_PROFILE_PATH");
    const bool profile_enabled = profile_path && *profile_path;
    const auto profile_now = [&]() {
        return profile_enabled ? std::chrono::steady_clock::now()
                               : std::chrono::steady_clock::time_point{};
    };
    const auto profile_add = [&](double& destination,
                                 std::chrono::steady_clock::time_point begin) {
        if (profile_enabled) destination += elapsed_seconds(begin);
    };
    crossdsd_encoder_info encoder_info{};
    encoder_info.struct_size = sizeof(encoder_info);
    auto result = crossdsd_encoder_get_info(encoder, &encoder_info);
    if (result != CROSSDSD_OK) return result;

    std::thread producer([&]() {
        ReaderOwner reader;
        producer_result = crossdsd_reader_open_file(
            input_path, &reader.value, &producer_error);
        crossdsd_decoder_config config{};
        config.struct_size = sizeof(config);
        config.input_sample_rate_hz = stream.sample_rate_hz;
        config.channel_count = stream.channel_count;
        config.level_mode = CROSSDSD_LEVEL_PCM_CONTINUITY;
        config.filter_profile = CROSSDSD_FILTER_STANDARD;
        DecoderOwner decoder;
        if (producer_result == CROSSDSD_OK)
            producer_result = crossdsd_decoder_create(
                &config, &decoder.value, &producer_error);
        crossdsd_decoder_info info{};
        info.struct_size = sizeof(info);
        if (producer_result == CROSSDSD_OK)
            producer_result = crossdsd_decoder_get_info(decoder.value, &info);
        const uint64_t expanded_stride = chunk_bytes * 8u;
        const uint64_t output_capacity = info.decimation_factor
            ? expanded_stride / info.decimation_factor + 1u : 0u;
        std::vector<uint8_t> packed;
        std::vector<int8_t> expanded;
        try {
            packed.resize(static_cast<size_t>(chunk_bytes * stream.channel_count));
            expanded.resize(static_cast<size_t>(
                expanded_stride * stream.channel_count));
        } catch (const std::bad_alloc&) {
            producer_result = CROSSDSD_ERROR_OUT_OF_MEMORY;
            std::snprintf(producer_error.message,
                          sizeof(producer_error.message), "out of memory");
        }
        while (producer_result == CROSSDSD_OK) {
            uint64_t bytes = 0;
            uint8_t valid = 0;
            auto profile_start = profile_now();
            producer_result = crossdsd_reader_read_packed(
                reader.value, packed.data(), chunk_bytes, &bytes, &valid,
                &producer_error);
            profile_add(profile.read_seconds, profile_start);
            if (producer_result == CROSSDSD_ERROR_END_OF_STREAM) {
                producer_result = CROSSDSD_OK;
                break;
            }
            if (producer_result != CROSSDSD_OK) break;
            const uint64_t bits = (bytes - 1u) * 8u + valid;
            profile_start = profile_now();
            for (uint32_t channel = 0; channel < stream.channel_count;
                 ++channel) {
                producer_result = crossdsd_expand_dsd_lsb(
                    packed.data() + static_cast<uint64_t>(channel) * chunk_bytes,
                    bits, expanded.data() +
                        static_cast<uint64_t>(channel) * expanded_stride,
                    expanded_stride);
                if (producer_result != CROSSDSD_OK) break;
            }
            profile_add(profile.expand_seconds, profile_start);
            if (producer_result != CROSSDSD_OK) break;
            PipelineBlock block;
            try {
                block.planar.resize(static_cast<size_t>(
                    output_capacity * stream.channel_count));
            } catch (const std::bad_alloc&) {
                producer_result = CROSSDSD_ERROR_OUT_OF_MEMORY;
                break;
            }
            profile_start = profile_now();
            producer_result = crossdsd_decoder_process(
                decoder.value, expanded.data(), bits, expanded_stride,
                block.planar.data(), output_capacity, output_capacity,
                &block.frames, &producer_error);
            profile_add(profile.decode_seconds, profile_start);
            if (producer_result != CROSSDSD_OK) break;
            block.source_bits = bits;
            profile_start = profile_now();
            std::unique_lock<std::mutex> lock(mutex);
            writable.wait(lock, [&] { return stopped || queue.size() < queue_capacity; });
            profile_add(profile.producer_wait_seconds, profile_start);
            if (stopped) break;
            queue.push_back(std::move(block));
            lock.unlock();
            readable.notify_one();
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            producer_finished = true;
        }
        readable.notify_one();
    });

    const uint64_t bytes_per_frame = encoder_info.interpolation_factor / 8u;
    const uint64_t maximum_frames = chunk_bytes * 8u /
        (stream.sample_rate_hz / CROSSDSD_DXD_SAMPLE_RATE) + 1u;
    const uint64_t packed_stride = maximum_frames * bytes_per_frame + 1u;
    std::vector<uint8_t> encoded;
    try {
        encoded.resize(static_cast<size_t>(packed_stride * stream.channel_count));
    } catch (const std::bad_alloc&) {
        result = CROSSDSD_ERROR_OUT_OF_MEMORY;
    }
    uint64_t consumed = 0;
    while (result == CROSSDSD_OK) {
        PipelineBlock block;
        {
            const auto profile_start = profile_now();
            std::unique_lock<std::mutex> lock(mutex);
            readable.wait(lock, [&] { return stopped || !queue.empty() || producer_finished; });
            profile_add(profile.consumer_wait_seconds, profile_start);
            if (queue.empty()) break;
            block = std::move(queue.front());
            queue.pop_front();
        }
        writable.notify_one();
        const uint64_t stride = maximum_frames;
        auto profile_start = profile_now();
        for (uint32_t channel = 0; channel < stream.channel_count; ++channel) {
            double* samples = block.planar.data() +
                static_cast<uint64_t>(channel) * stride;
            for (uint64_t frame = 0; frame < block.frames; ++frame) {
                pass.peaks[channel] = std::max(
                    pass.peaks[channel], std::abs(samples[frame]));
                samples[frame] *= gain;
            }
        }
        profile_add(profile.gain_seconds, profile_start);
        uint64_t encoded_bytes = 0;
        profile_start = profile_now();
        result = crossdsd_encoder_process_f64(
            encoder, block.planar.data(), block.frames, stride,
            encoded.data(), packed_stride, packed_stride, &encoded_bytes,
            error);
        profile_add(profile.encode_seconds, profile_start);
        if (result != CROSSDSD_OK) break;
        if (encoded_bytes) {
            profile_start = profile_now();
            for (uint32_t channel = 1; channel < stream.channel_count; ++channel)
                std::memmove(
                    encoded.data() + static_cast<uint64_t>(channel) * encoded_bytes,
                    encoded.data() + static_cast<uint64_t>(channel) * packed_stride,
                    static_cast<size_t>(encoded_bytes));
            profile_add(profile.compact_seconds, profile_start);
            profile_start = profile_now();
            result = crossdsd_dsf_writer_write_packed(
                writer, encoded.data(), encoded_bytes, 8, error);
            profile_add(profile.write_seconds, profile_start);
            if (result != CROSSDSD_OK) break;
        }
        pass.frames += block.frames;
        ++profile.blocks;
        consumed += block.source_bits;
        if (progress_callback && !progress_callback(
                progress_user_data,
                progress_begin + progress_scale * std::min(
                    1.0, static_cast<double>(consumed) /
                        stream.sample_count_per_channel))) {
            set_error(error, CROSSDSD_ERROR_CANCELLED, "conversion cancelled");
            result = CROSSDSD_ERROR_CANCELLED;
            break;
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        stopped = true;
    }
    writable.notify_all();
    readable.notify_all();
    producer.join();
    if (result == CROSSDSD_OK && producer_result != CROSSDSD_OK) {
        set_error(error, producer_result, producer_error.message);
        result = producer_result;
    }
    write_pipeline_profile(
        profile_path, stream, encoder_info, profile);
    return result;
}

}  // namespace

extern "C" {

crossdsd_result crossdsd_transcode_file(
    const char* input_path, const char* output_path,
    const crossdsd_transcode_file_config* config,
    crossdsd_transcode_file_report* report, crossdsd_error* error) {
    if (!input_path || !output_path || !config || !report ||
        config->struct_size < sizeof(crossdsd_transcode_file_config) ||
        report->struct_size < sizeof(crossdsd_transcode_file_report) ||
        rate_hz(config->dsd_rate) == 0 ||
        (config->encoder_profile != CROSSDSD_ENCODER_STANDARD &&
         config->encoder_profile != CROSSDSD_ENCODER_DSD64_ENHANCED) ||
        (config->max_peak_enabled &&
         (!std::isfinite(config->max_peak_dbfs) ||
          config->max_peak_dbfs > 0.0)) ||
        (config->fixed_gain_enabled &&
         (!std::isfinite(config->fixed_gain_db) ||
          config->fixed_gain_db > 0.0)) ||
        (config->max_peak_enabled && config->fixed_gain_enabled)) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                  "invalid DSD transcode configuration");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    if (config->encoder_profile == CROSSDSD_ENCODER_DSD64_ENHANCED &&
        config->dsd_rate != CROSSDSD_DSD64) {
        set_error(error, CROSSDSD_ERROR_UNSUPPORTED_RATE,
                  "Enhanced quality is available for DSD64 only");
        return CROSSDSD_ERROR_UNSUPPORTED_RATE;
    }
    const uint64_t chunk_bytes =
        config->packed_chunk_bytes_per_channel == 0
            ? 65'536u
            : config->packed_chunk_bytes_per_channel;
    if (chunk_bytes == 0 ||
        chunk_bytes > std::numeric_limits<size_t>::max() / 8u) {
        set_error(error, CROSSDSD_ERROR_OVERFLOW,
                  "transcode block is too large");
        return CROSSDSD_ERROR_OVERFLOW;
    }

    ReaderOwner source_reader;
    auto result =
        crossdsd_reader_open_file(input_path, &source_reader.value, error);
    if (result != CROSSDSD_OK) return result;
    crossdsd_stream_info stream{};
    stream.struct_size = sizeof(stream);
    result = crossdsd_reader_get_info(source_reader.value, &stream);
    if (result != CROSSDSD_OK) return result;
    if (stream.sample_count_per_channel == 0) {
        set_error(error, CROSSDSD_ERROR_INVALID_SAMPLE_COUNT,
                  "transcode input contains no DSD samples");
        return CROSSDSD_ERROR_INVALID_SAMPLE_COUNT;
    }
    if (config->progress_callback &&
        !config->progress_callback(config->progress_user_data, 0.0)) {
        set_error(error, CROSSDSD_ERROR_CANCELLED,
                  "conversion cancelled");
        return CROSSDSD_ERROR_CANCELLED;
    }

    try {
        const auto total_start = std::chrono::steady_clock::now();
        PassResult analysis;
        double analysis_seconds = 0.0;
        double gain = 1.0;
        double gain_db = 0.0;
        double peak = 0.0;
        crossdsd::internal::QuickPeakAnalysis quick_analysis;
        if (config->fixed_gain_enabled) {
            gain_db = config->fixed_gain_db;
            gain = std::pow(10.0, gain_db / 20.0);
        }
        if (config->max_peak_enabled) {
            const auto analysis_start = std::chrono::steady_clock::now();
            if (config->full_peak_search) {
                result = crossdsd::internal::run_full_peak_analysis(
                    input_path, stream, chunk_bytes,
                    config->progress_callback, config->progress_user_data,
                    quick_analysis, error);
                analysis.frames = quick_analysis.reconstructed_frames;
                std::copy(
                    quick_analysis.peaks,
                    quick_analysis.peaks + stream.channel_count,
                    analysis.peaks);
            } else {
                result = crossdsd::internal::run_quick_peak_analysis(
                    input_path, stream, chunk_bytes,
                    config->progress_callback, config->progress_user_data,
                    quick_analysis, error);
                analysis.frames = quick_analysis.reconstructed_frames;
                std::copy(
                    quick_analysis.peaks,
                    quick_analysis.peaks + stream.channel_count,
                    analysis.peaks);
            }
            analysis_seconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - analysis_start).count();
            if (result != CROSSDSD_OK) return result;
            peak = *std::max_element(
                analysis.peaks, analysis.peaks + stream.channel_count);
            if (peak > 0.0) {
                const double peak_db = 20.0 * std::log10(peak);
                gain_db = std::min(0.0, config->max_peak_dbfs - peak_db);
                gain = std::pow(10.0, gain_db / 20.0);
            }
        }

        crossdsd_encoder_config encoder_config{};
        encoder_config.struct_size = sizeof(encoder_config);
        encoder_config.input_sample_rate_hz = CROSSDSD_DXD_SAMPLE_RATE;
        encoder_config.output_sample_rate_hz = rate_hz(config->dsd_rate);
        encoder_config.channel_count = stream.channel_count;
        encoder_config.modulator_profile =
            modulator_profile(config->dsd_rate);
        encoder_config.encoder_profile = config->encoder_profile;
        EncoderOwner encoder;
        result = crossdsd_encoder_create(
            &encoder_config, &encoder.value, error);
        if (result != CROSSDSD_OK) return result;

        crossdsd_dsf_writer_config writer_config{};
        writer_config.struct_size = sizeof(writer_config);
        writer_config.sample_rate_hz = rate_hz(config->dsd_rate);
        writer_config.channel_count = stream.channel_count;
        writer_config.channel_type =
            config->channel_type == 0 ? stream.channel_type
                                      : config->channel_type;
        WriterOwner writer;
        result = crossdsd_dsf_writer_open_file(
            output_path, &writer_config, &writer.value, error);
        if (result != CROSSDSD_OK) return result;
        writer.path = output_path;

        const auto transcode_start = std::chrono::steady_clock::now();
        PassResult conversion;
        const bool cpu_pipeline =
            std::getenv("CROSSDSD_DISABLE_CPU_PIPELINE") == nullptr;
        result = cpu_pipeline
            ? run_pass_pipelined(
                input_path, stream, chunk_bytes, gain, encoder.value,
                writer.value, config->max_peak_enabled ? 0.5 : 0.0,
                config->max_peak_enabled ? 0.49 : 0.99,
                config->progress_callback, config->progress_user_data,
                conversion, error)
            : run_pass(
                input_path, stream, chunk_bytes, gain, encoder.value,
                writer.value, config->max_peak_enabled ? 0.5 : 0.0,
                config->max_peak_enabled ? 0.49 : 0.99,
                config->progress_callback, config->progress_user_data,
                conversion, error);
        if (result != CROSSDSD_OK) return result;
        if (!config->max_peak_enabled) {
            std::copy(
                conversion.peaks,
                conversion.peaks + stream.channel_count,
                analysis.peaks);
            analysis.frames = conversion.frames;
            peak = *std::max_element(
                analysis.peaks, analysis.peaks + stream.channel_count);
        }

        crossdsd_encoder_info encoder_info{};
        encoder_info.struct_size = sizeof(encoder_info);
        crossdsd_encoder_get_info(encoder.value, &encoder_info);
        const uint64_t bytes_per_frame =
            encoder_info.interpolation_factor / 8u;
        const uint64_t remaining =
            encoder_info.input_samples_consumed_per_channel *
                bytes_per_frame -
            encoder_info.packed_bytes_produced_per_channel;
        if (remaining >
            std::numeric_limits<size_t>::max() / stream.channel_count) {
            set_error(error, CROSSDSD_ERROR_OVERFLOW,
                      "transcode final buffer is too large");
            return CROSSDSD_ERROR_OVERFLOW;
        }
        std::vector<uint8_t> final_packed(
            static_cast<size_t>(remaining * stream.channel_count));
        uint64_t final_bytes = 0;
        result = crossdsd_encoder_finish(
            encoder.value, final_packed.data(), remaining, remaining,
            &final_bytes, error);
        if (result != CROSSDSD_OK) return result;
        if (final_bytes != 0) {
            result = crossdsd_dsf_writer_write_packed(
                writer.value, final_packed.data(), final_bytes, 8, error);
            if (result != CROSSDSD_OK) return result;
        }

        uint64_t metadata_written = 0;
        crossdsd_metadata_status metadata_status = CROSSDSD_METADATA_NONE;
        std::vector<uint8_t> metadata;
        if (stream.metadata_status == CROSSDSD_METADATA_ID3 &&
            stream.metadata_size != 0) {
            if (stream.metadata_size > std::numeric_limits<size_t>::max()) {
                set_error(error, CROSSDSD_ERROR_OVERFLOW,
                          "metadata is too large");
                return CROSSDSD_ERROR_OVERFLOW;
            }
            metadata.resize(static_cast<size_t>(stream.metadata_size));
            uint64_t metadata_read = 0;
            result = crossdsd_reader_read_metadata(
                source_reader.value, metadata.data(), metadata.size(),
                &metadata_read, error);
            if (result != CROSSDSD_OK) return result;
            metadata.resize(static_cast<size_t>(metadata_read));
        }
        const char* rate_name = config->dsd_rate == CROSSDSD_DSD64
            ? "DSD64" : config->dsd_rate == CROSSDSD_DSD128
                ? "DSD128" : config->dsd_rate == CROSSDSD_DSD256
                    ? "DSD256" : "DSD512";
        char settings[192]{};
        if (config->max_peak_enabled) {
            std::snprintf(
                settings, sizeof(settings),
                "CrossDSD %s - %s%s - MaxPeak %.1f dBFS",
                CROSSDSD_VERSION, rate_name,
                config->encoder_profile ==
                        CROSSDSD_ENCODER_DSD64_ENHANCED
                    ? " Enhanced" : "",
                config->max_peak_dbfs);
        } else if (config->fixed_gain_enabled) {
            std::snprintf(
                settings, sizeof(settings),
                "CrossDSD %s - %s%s - FixedGain %.3f dB",
                CROSSDSD_VERSION, rate_name,
                config->encoder_profile ==
                        CROSSDSD_ENCODER_DSD64_ENHANCED
                    ? " Enhanced" : "",
                config->fixed_gain_db);
        } else {
            std::snprintf(settings, sizeof(settings), "CrossDSD %s - %s%s",
                          CROSSDSD_VERSION, rate_name,
                          config->encoder_profile ==
                                  CROSSDSD_ENCODER_DSD64_ENHANCED
                              ? " Enhanced" : "");
        }
        metadata = crossdsd_id3_with_encoder_settings(
            metadata, settings, rate_name);
        result = crossdsd_dsf_writer_write_id3(
            writer.value, metadata.data(), metadata.size(), error);
        if (result != CROSSDSD_OK) return result;
        metadata_written = metadata.size();
        metadata_status = CROSSDSD_METADATA_ID3;

        crossdsd_dsf_writer_info writer_info{};
        writer_info.struct_size = sizeof(writer_info);
        crossdsd_dsf_writer_get_info(writer.value, &writer_info);
        result = crossdsd_dsf_writer_close(writer.value, error);
        writer.closed = true;
        writer.value = nullptr;
        if (result != CROSSDSD_OK) return result;
        writer.committed = true;
        const double transcode_seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - transcode_start).count();
        const double total_seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - total_start).count();

        crossdsd_encoder_get_info(encoder.value, &encoder_info);
        crossdsd_transcode_file_report value{};
        value.struct_size = sizeof(value);
        value.api_version = CROSSDSD_CONVERT_API_VERSION;
        value.input_sample_rate_hz = stream.sample_rate_hz;
        value.output_sample_rate_hz = writer_info.sample_rate_hz;
        value.channel_count = stream.channel_count;
        value.channel_type = writer_info.channel_type;
        value.input_dsd_rate = stream.dsd_rate;
        value.output_dsd_rate = writer_info.dsd_rate;
        value.encoder_profile = config->encoder_profile;
        value.input_dsd_samples_per_channel =
            stream.sample_count_per_channel;
        value.reconstructed_frames = conversion.frames;
        value.output_dsd_samples_per_channel =
            encoder_info.dsd_samples_produced_per_channel;
        value.output_packed_bytes_per_channel =
            encoder_info.packed_bytes_produced_per_channel;
        const uint64_t stored_blocks =
            (encoder_info.packed_bytes_produced_per_channel +
             writer_info.block_size_per_channel - 1u) /
            writer_info.block_size_per_channel;
        value.output_size_bytes =
            92u + stored_blocks * writer_info.block_size_per_channel *
                      stream.channel_count +
            metadata_written;
        value.metadata_size_bytes = metadata_written;
        value.metadata_status = metadata_status;
        value.max_peak_enabled = config->max_peak_enabled ? 1u : 0u;
        value.requested_max_peak_dbfs =
            config->max_peak_enabled ? config->max_peak_dbfs : 0.0;
        value.reconstructed_peak = peak;
        value.reconstructed_peak_dbfs =
            peak > 0.0 ? 20.0 * std::log10(peak)
                       : -std::numeric_limits<double>::infinity();
        value.applied_gain = gain;
        value.applied_gain_db = gain_db;
        value.duration_seconds = stream.duration_seconds;
        value.analysis_seconds = analysis_seconds;
        value.transcode_seconds = transcode_seconds;
        value.realtime_factor =
            total_seconds > 0.0 ? stream.duration_seconds / total_seconds : 0.0;
        value.enhanced_dsd_samples_per_channel =
            encoder_info.enhanced_dsd_samples_per_channel;
        value.enhanced_segment_count =
            encoder_info.enhanced_segment_count;
        value.full_peak_search = config->full_peak_search ? 1u : 0u;
        value.quick_scan_seconds = quick_analysis.scan_seconds;
        value.analysis_input_fraction =
            config->max_peak_enabled && !config->full_peak_search
                ? quick_analysis.fine_input_fraction
                : (config->max_peak_enabled ? 1.0 : 0.0);
        value.fixed_gain_enabled = config->fixed_gain_enabled ? 1u : 0u;
        value.requested_fixed_gain_db =
            config->fixed_gain_enabled ? config->fixed_gain_db : 0.0;
        std::copy(
            analysis.peaks, analysis.peaks + stream.channel_count,
            value.channel_reconstructed_peaks);
        for (uint32_t channel = 0; channel < stream.channel_count; ++channel) {
            crossdsd_modulator_channel_metrics metrics{};
            metrics.struct_size = sizeof(metrics);
            crossdsd_encoder_get_channel_metrics(
                encoder.value, channel, &metrics);
            value.channel_maximum_states[channel] =
                metrics.maximum_absolute_state;
            value.channel_maximum_quantizer_inputs[channel] =
                metrics.maximum_absolute_quantizer_input;
            value.channel_maximum_identical_bit_runs[channel] =
                metrics.maximum_identical_bit_run;
            value.channel_one_densities[channel] = metrics.one_density;
        }
        *report = value;
        if (config->progress_callback) {
            config->progress_callback(config->progress_user_data, 1.0);
        }
    } catch (const std::bad_alloc&) {
        set_error(error, CROSSDSD_ERROR_OUT_OF_MEMORY, "out of memory");
        return CROSSDSD_ERROR_OUT_OF_MEMORY;
    }
    set_error(error, CROSSDSD_OK, "");
    return CROSSDSD_OK;
}

}  // extern "C"
