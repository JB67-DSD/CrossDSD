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

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

#if defined(CROSSDSD_ENABLE_METAL)
extern "C" bool crossdsd_metal_backend_available(void);
extern "C" crossdsd_result crossdsd_decode_file_metal(
    const char* input_path, const char* output_path,
    const crossdsd_decode_file_config* config,
    crossdsd_decode_file_report* report, crossdsd_error* error);
#endif

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

extern "C" {

crossdsd_result crossdsd_decode_file(
    const char* input_path, const char* output_path,
    const crossdsd_decode_file_config* config,
    crossdsd_decode_file_report* report, crossdsd_error* error) {
    if (!input_path || !output_path || !config || !report ||
        config->struct_size < sizeof(crossdsd_decode_file_config) ||
        report->struct_size < sizeof(crossdsd_decode_file_report) ||
        (config->filter_profile != CROSSDSD_FILTER_STANDARD &&
         config->filter_profile != CROSSDSD_FILTER_RAW) ||
        (config->container != CROSSDSD_WAVE_AUTO &&
         config->container != CROSSDSD_WAVE_RIFF &&
         config->container != CROSSDSD_WAVE_RF64) ||
        (config->backend != CROSSDSD_BACKEND_AUTO &&
         config->backend != CROSSDSD_BACKEND_CPU &&
         config->backend != CROSSDSD_BACKEND_METAL) ||
        (config->fixed_gain_enabled &&
         (!std::isfinite(config->fixed_gain_db) || config->fixed_gain_db > 0.0))) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                  "invalid file decode configuration");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
#if !defined(CROSSDSD_ENABLE_METAL)
    if (config->backend == CROSSDSD_BACKEND_METAL) {
        set_error(error, CROSSDSD_ERROR_UNSUPPORTED_FORMAT,
                  "Metal file backend is not available in this build");
        return CROSSDSD_ERROR_UNSUPPORTED_FORMAT;
    }
#endif
    const double fixed_gain = config->fixed_gain_enabled
        ? std::pow(10.0, config->fixed_gain_db / 20.0) : 1.0;
    const uint64_t chunk_bytes =
        config->packed_chunk_bytes_per_channel == 0
            ? 65'536u
            : config->packed_chunk_bytes_per_channel;
    if (chunk_bytes > std::numeric_limits<size_t>::max() / 8u) {
        set_error(error, CROSSDSD_ERROR_OVERFLOW, "decode block is too large");
        return CROSSDSD_ERROR_OVERFLOW;
    }

    ReaderOwner reader;
    auto result =
        crossdsd_reader_open_file(input_path, &reader.value, error);
    if (result != CROSSDSD_OK) return result;
    crossdsd_stream_info stream{};
    stream.struct_size = sizeof(stream);
    result = crossdsd_reader_get_info(reader.value, &stream);
    if (result != CROSSDSD_OK) return result;

#if defined(CROSSDSD_ENABLE_METAL)
    const bool request_metal = !config->fixed_gain_enabled && (
        config->backend == CROSSDSD_BACKEND_METAL ||
        (config->backend == CROSSDSD_BACKEND_AUTO &&
         stream.duration_seconds >= 0.25 &&
         crossdsd_metal_backend_available()));
    if (request_metal) {
        const auto metal_result = crossdsd_decode_file_metal(
            input_path, output_path, config, report, error);
        if (metal_result == CROSSDSD_OK ||
            config->backend == CROSSDSD_BACKEND_METAL ||
            (metal_result != CROSSDSD_ERROR_OUT_OF_MEMORY &&
             metal_result != CROSSDSD_ERROR_OVERFLOW &&
             metal_result != CROSSDSD_ERROR_UNSUPPORTED_FORMAT)) {
            return metal_result;
        }
        set_error(error, CROSSDSD_OK, "");
    }
#endif

    crossdsd_decoder_config decoder_config{};
    decoder_config.struct_size = sizeof(decoder_config);
    decoder_config.input_sample_rate_hz = stream.sample_rate_hz;
    decoder_config.channel_count = stream.channel_count;
    decoder_config.level_mode = CROSSDSD_LEVEL_PCM_CONTINUITY;
    decoder_config.filter_profile = config->filter_profile;
    DecoderOwner decoder;
    result = crossdsd_decoder_create(
        &decoder_config, &decoder.value, error);
    if (result != CROSSDSD_OK) return result;
    crossdsd_decoder_info decoder_info{};
    decoder_info.struct_size = sizeof(decoder_info);
    result = crossdsd_decoder_get_info(decoder.value, &decoder_info);
    if (result != CROSSDSD_OK) return result;

    crossdsd_wave_writer_config writer_config{};
    writer_config.struct_size = sizeof(writer_config);
    writer_config.sample_rate_hz = CROSSDSD_DXD_SAMPLE_RATE;
    writer_config.channel_count = stream.channel_count;
    writer_config.expected_frame_count =
        stream.sample_count_per_channel / decoder_info.decimation_factor;
    writer_config.container = config->container;
    writer_config.channel_type = stream.channel_type;
    WriterOwner writer;
    result = crossdsd_wave_writer_open_file(
        output_path, &writer_config, &writer.value, error);
    if (result != CROSSDSD_OK) return result;
    writer.path = output_path;

    try {
        if (chunk_bytes >
                std::numeric_limits<size_t>::max() / stream.channel_count ||
            chunk_bytes * stream.channel_count >
                std::numeric_limits<size_t>::max() / 8u) {
            set_error(error, CROSSDSD_ERROR_OVERFLOW,
                      "decode buffers are too large");
            return CROSSDSD_ERROR_OVERFLOW;
        }
        const uint64_t packed_values = chunk_bytes * stream.channel_count;
        const uint64_t expanded_stride = chunk_bytes * 8u;
        const uint64_t expanded_values =
            expanded_stride * stream.channel_count;
        const uint64_t output_capacity =
            expanded_stride / decoder_info.decimation_factor + 1u;
        if (output_capacity >
            std::numeric_limits<size_t>::max() / stream.channel_count) {
            set_error(error, CROSSDSD_ERROR_OVERFLOW,
                      "output buffer is too large");
            return CROSSDSD_ERROR_OVERFLOW;
        }
        std::vector<uint8_t> packed(static_cast<size_t>(packed_values));
        std::vector<int8_t> expanded(static_cast<size_t>(expanded_values));
        std::vector<double> planar(
            static_cast<size_t>(output_capacity * stream.channel_count));
        std::vector<float> interleaved(
            static_cast<size_t>(output_capacity * stream.channel_count));
        double peaks[CROSSDSD_MAX_CHANNELS]{};
        double processing_seconds = 0.0;
        double writing_seconds = 0.0;

        const auto start = std::chrono::steady_clock::now();
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
            const auto processing_start =
                std::chrono::steady_clock::now();
            result = crossdsd_decoder_process(
                decoder.value, expanded.data(), bits, expanded_stride,
                planar.data(), output_capacity, output_capacity, &produced,
                error);
            processing_seconds += std::chrono::duration<double>(
                std::chrono::steady_clock::now() - processing_start).count();
            if (result != CROSSDSD_OK) return result;
            for (uint64_t frame = 0; frame < produced; ++frame) {
                for (uint32_t channel = 0; channel < stream.channel_count;
                     ++channel) {
                    const double value = fixed_gain *
                        planar[static_cast<uint64_t>(channel) * output_capacity +
                               frame];
                    peaks[channel] =
                        std::max(peaks[channel], std::abs(value));
                    interleaved[
                        frame * stream.channel_count + channel] =
                        static_cast<float>(value);
                }
            }
            const auto writing_start = std::chrono::steady_clock::now();
            result = crossdsd_wave_writer_write(
                writer.value, interleaved.data(), produced, error);
            writing_seconds += std::chrono::duration<double>(
                std::chrono::steady_clock::now() - writing_start).count();
            if (result != CROSSDSD_OK) return result;
            if (config->progress_callback) {
                crossdsd_decoder_info progress_info{};
                progress_info.struct_size = sizeof(progress_info);
                crossdsd_decoder_get_info(decoder.value, &progress_info);
                const double fraction =
                    stream.sample_count_per_channel == 0
                        ? 1.0
                        : std::min(
                              1.0,
                              static_cast<double>(
                                  progress_info
                                      .input_samples_consumed_per_channel) /
                                  stream.sample_count_per_channel);
                if (!config->progress_callback(
                        config->progress_user_data, fraction)) {
                    set_error(error, CROSSDSD_ERROR_CANCELLED,
                              "conversion cancelled");
                    return CROSSDSD_ERROR_CANCELLED;
                }
            }
        }
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
        const auto metadata_writing_start = std::chrono::steady_clock::now();
        result = crossdsd_wave_writer_write_id3(
            writer.value, metadata.data(), metadata.size(), error);
        writing_seconds += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - metadata_writing_start).count();
        if (result != CROSSDSD_OK) return result;
        const double seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
        crossdsd_decoder_get_info(decoder.value, &decoder_info);
        crossdsd_wave_writer_info writer_info{};
        writer_info.struct_size = sizeof(writer_info);
        crossdsd_wave_writer_get_info(writer.value, &writer_info);
        result = crossdsd_wave_writer_close(writer.value, error);
        writer.closed = true;
        writer.value = nullptr;
        if (result != CROSSDSD_OK) return result;
        writer.committed = true;

        crossdsd_decode_file_report value{};
        value.struct_size = sizeof(value);
        value.api_version = CROSSDSD_CONVERT_API_VERSION;
        value.input_sample_rate_hz = stream.sample_rate_hz;
        value.output_sample_rate_hz = CROSSDSD_DXD_SAMPLE_RATE;
        value.channel_count = stream.channel_count;
        value.stage_count = decoder_info.stage_count;
        value.filter_profile = config->filter_profile;
        value.container = writer_info.container;
        value.backend = CROSSDSD_BACKEND_CPU;
        value.input_samples_per_channel =
            stream.sample_count_per_channel;
        value.output_frames = writer_info.frame_count;
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
        value.processing_seconds = processing_seconds;
        value.writing_seconds = writing_seconds;
        value.preparation_seconds =
            std::max(0.0, seconds - processing_seconds - writing_seconds);
        value.realtime_factor =
            seconds > 0.0 ? stream.duration_seconds / seconds : 0.0;
        value.latency_output_samples =
            decoder_info.latency_output_samples;
        std::copy(
            peaks, peaks + stream.channel_count, value.channel_peaks);
        value.fixed_gain_enabled = config->fixed_gain_enabled;
        value.requested_fixed_gain_db = config->fixed_gain_enabled
            ? config->fixed_gain_db : 0.0;
        *report = value;
    } catch (const std::bad_alloc&) {
        set_error(error, CROSSDSD_ERROR_OUT_OF_MEMORY, "out of memory");
        return CROSSDSD_ERROR_OUT_OF_MEMORY;
    }
    set_error(error, CROSSDSD_OK, "");
    return CROSSDSD_OK;
}

}  // extern "C"
