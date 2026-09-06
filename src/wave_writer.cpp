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

#include "crossdsd/audio.h"
#include "platform_file.h"

#include <bit>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <new>

namespace {

constexpr uint64_t kRiffHeaderBytes = 56u;
constexpr uint64_t kRf64HeaderBytes = 92u;
constexpr uint64_t kExtensibleExtraBytes = 24u;

uint32_t channel_mask_for_dsf_type(uint32_t type) {
    constexpr uint32_t fl = 0x0001u;
    constexpr uint32_t fr = 0x0002u;
    constexpr uint32_t fc = 0x0004u;
    constexpr uint32_t lfe = 0x0008u;
    constexpr uint32_t bl = 0x0010u;
    constexpr uint32_t br = 0x0020u;
    switch (type) {
    case 1: return fc;
    case 2: return fl | fr;
    case 3: return fl | fr | fc;
    case 4: return fl | fr | bl | br;
    case 5: return fl | fr | fc | lfe;
    case 6: return fl | fr | fc | bl | br;
    case 7: return fl | fr | fc | lfe | bl | br;
    default: return 0;
    }
}

uint32_t channel_count_for_dsf_type(uint32_t type) {
    static constexpr uint32_t counts[] = {0, 1, 2, 3, 4, 4, 5, 6};
    return type < sizeof(counts) / sizeof(counts[0]) ? counts[type] : 0;
}

void set_error(crossdsd_error* error, crossdsd_result code, const char* message) {
    if (!error || error->struct_size < sizeof(crossdsd_error)) return;
    error->code = code;
    error->byte_offset = 0;
    std::snprintf(error->message, sizeof(error->message), "%s", message);
}

bool multiply_overflows(uint64_t left, uint64_t right) {
    return right != 0 && left > std::numeric_limits<uint64_t>::max() / right;
}

bool write_bytes(FILE* file, const void* data, size_t size) {
    return std::fwrite(data, 1, size, file) == size;
}

bool write_fourcc(FILE* file, const char value[5]) {
    return write_bytes(file, value, 4);
}

bool write_u16(FILE* file, uint16_t value) {
    const uint8_t bytes[] = {
        static_cast<uint8_t>(value),
        static_cast<uint8_t>(value >> 8u),
    };
    return write_bytes(file, bytes, sizeof(bytes));
}

bool write_u32(FILE* file, uint32_t value) {
    const uint8_t bytes[] = {
        static_cast<uint8_t>(value),
        static_cast<uint8_t>(value >> 8u),
        static_cast<uint8_t>(value >> 16u),
        static_cast<uint8_t>(value >> 24u),
    };
    return write_bytes(file, bytes, sizeof(bytes));
}

bool write_u64(FILE* file, uint64_t value) {
    const uint8_t bytes[] = {
        static_cast<uint8_t>(value),
        static_cast<uint8_t>(value >> 8u),
        static_cast<uint8_t>(value >> 16u),
        static_cast<uint8_t>(value >> 24u),
        static_cast<uint8_t>(value >> 32u),
        static_cast<uint8_t>(value >> 40u),
        static_cast<uint8_t>(value >> 48u),
        static_cast<uint8_t>(value >> 56u),
    };
    return write_bytes(file, bytes, sizeof(bytes));
}

bool write_common_chunks(
    FILE* file, uint32_t sample_rate, uint16_t channels,
    uint32_t channel_mask) {
    const uint32_t block_align = static_cast<uint32_t>(channels) * sizeof(float);
    const uint64_t byte_rate_64 = static_cast<uint64_t>(sample_rate) * block_align;
    if (byte_rate_64 > std::numeric_limits<uint32_t>::max()) return false;
    const bool extensible = channel_mask != 0;
    bool ok = write_fourcc(file, "fmt ") &&
           write_u32(file, extensible ? 40u : 16u) &&
           write_u16(file, extensible ? 0xfffeu : 3u) &&
           write_u16(file, channels) &&
           write_u32(file, sample_rate) &&
           write_u32(file, static_cast<uint32_t>(byte_rate_64)) &&
           write_u16(file, static_cast<uint16_t>(block_align)) &&
           write_u16(file, 32u);
    if (ok && extensible) {
        static constexpr uint8_t kFloatSubtype[16] = {
            3, 0, 0, 0, 0, 0, 0x10, 0,
            0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71};
        ok = write_u16(file, 22u) && write_u16(file, 32u) &&
             write_u32(file, channel_mask) &&
             write_bytes(file, kFloatSubtype, sizeof(kFloatSubtype));
    }
    return ok && write_fourcc(file, "fact") &&
           write_u32(file, 4u) && write_u32(file, 0u);
}

bool write_riff_header(
    FILE* file, uint32_t sample_rate, uint16_t channels,
    uint32_t channel_mask) {
    return write_fourcc(file, "RIFF") && write_u32(file, 0u) &&
           write_fourcc(file, "WAVE") &&
           write_common_chunks(file, sample_rate, channels, channel_mask) &&
           write_fourcc(file, "data") && write_u32(file, 0u);
}

bool write_rf64_header(
    FILE* file, uint32_t sample_rate, uint16_t channels,
    uint32_t channel_mask) {
    return write_fourcc(file, "RF64") && write_u32(file, UINT32_MAX) &&
           write_fourcc(file, "WAVE") && write_fourcc(file, "ds64") &&
           write_u32(file, 28u) && write_u64(file, 0u) && write_u64(file, 0u) &&
           write_u64(file, 0u) && write_u32(file, 0u) &&
           write_common_chunks(file, sample_rate, channels, channel_mask) &&
           write_fourcc(file, "data") && write_u32(file, UINT32_MAX);
}

crossdsd_wave_container select_container(
    const crossdsd_wave_writer_config& config) {
    if (config.container != CROSSDSD_WAVE_AUTO) return config.container;
    if (multiply_overflows(
            config.expected_frame_count,
            static_cast<uint64_t>(config.channel_count) * sizeof(float))) {
        return CROSSDSD_WAVE_RF64;
    }
    const uint64_t data_bytes =
        config.expected_frame_count * config.channel_count * sizeof(float);
    const uint64_t header_bytes = kRiffHeaderBytes +
        (channel_mask_for_dsf_type(config.channel_type) != 0
             ? kExtensibleExtraBytes : 0u);
    return data_bytes + header_bytes - 8u <= UINT32_MAX &&
                   config.expected_frame_count <= UINT32_MAX
               ? CROSSDSD_WAVE_RIFF
               : CROSSDSD_WAVE_RF64;
}

}  // namespace

struct crossdsd_wave_writer {
    FILE* file = nullptr;
    crossdsd_wave_writer_info info{};
    uint64_t header_bytes = 0;
    uint64_t fact_sample_offset = 0;
    uint64_t data_size_offset = 0;
};

extern "C" {

crossdsd_result crossdsd_wave_writer_open_file(
    const char* path, const crossdsd_wave_writer_config* config,
    crossdsd_wave_writer** out_writer, crossdsd_error* error) {
    if (!path || !config ||
        config->struct_size < sizeof(crossdsd_wave_writer_config) || !out_writer ||
        config->sample_rate_hz == 0 || config->channel_count == 0 ||
        config->channel_count > 256 ||
        (config->channel_type != 0 &&
         channel_count_for_dsf_type(config->channel_type) !=
             config->channel_count) ||
        (config->container != CROSSDSD_WAVE_AUTO &&
         config->container != CROSSDSD_WAVE_RIFF &&
         config->container != CROSSDSD_WAVE_RF64)) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                  "invalid WAVE writer configuration");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    if (config->channel_count >
        std::numeric_limits<uint16_t>::max() / sizeof(float)) {
        set_error(error, CROSSDSD_ERROR_UNSUPPORTED_CHANNELS,
                  "channel count exceeds WAVE block alignment");
        return CROSSDSD_ERROR_UNSUPPORTED_CHANNELS;
    }
    *out_writer = nullptr;
    FILE* file = crossdsd::platform::open_file(path, "wb");
    if (!file) {
        set_error(error, CROSSDSD_ERROR_IO, "cannot create output file");
        return CROSSDSD_ERROR_IO;
    }
    try {
        auto writer = std::make_unique<crossdsd_wave_writer>();
        writer->file = file;
        writer->info.struct_size = sizeof(writer->info);
        writer->info.api_version = CROSSDSD_AUDIO_API_VERSION;
        writer->info.sample_rate_hz = config->sample_rate_hz;
        writer->info.channel_count = config->channel_count;
        writer->info.bits_per_sample = 32u;
        writer->info.channel_type = config->channel_type;
        writer->info.channel_mask =
            channel_mask_for_dsf_type(config->channel_type);
        writer->info.container = select_container(*config);
        const bool extensible = writer->info.channel_mask != 0;
        writer->header_bytes =
            (writer->info.container == CROSSDSD_WAVE_RIFF
                 ? kRiffHeaderBytes : kRf64HeaderBytes) +
            (extensible ? kExtensibleExtraBytes : 0u);
        writer->fact_sample_offset =
            (writer->info.container == CROSSDSD_WAVE_RIFF ? 44u : 80u) +
            (extensible ? kExtensibleExtraBytes : 0u);
        writer->data_size_offset = writer->fact_sample_offset + 8u;
        const bool header_ok =
            writer->info.container == CROSSDSD_WAVE_RIFF
                ? write_riff_header(
                      file, config->sample_rate_hz,
                      static_cast<uint16_t>(config->channel_count),
                      writer->info.channel_mask)
                : write_rf64_header(
                      file, config->sample_rate_hz,
                      static_cast<uint16_t>(config->channel_count),
                      writer->info.channel_mask);
        if (!header_ok) {
            std::fclose(file);
            set_error(error, CROSSDSD_ERROR_IO, "cannot write WAVE header");
            return CROSSDSD_ERROR_IO;
        }
        *out_writer = writer.release();
    } catch (const std::bad_alloc&) {
        std::fclose(file);
        set_error(error, CROSSDSD_ERROR_OUT_OF_MEMORY, "out of memory");
        return CROSSDSD_ERROR_OUT_OF_MEMORY;
    }
    set_error(error, CROSSDSD_OK, "");
    return CROSSDSD_OK;
}

crossdsd_result crossdsd_wave_writer_write(
    crossdsd_wave_writer* writer, const float* interleaved,
    uint64_t frame_count, crossdsd_error* error) {
    if (!writer || !writer->file || writer->info.metadata_size_bytes != 0 ||
        (frame_count != 0 && !interleaved)) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                  "invalid WAVE write arguments");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    const uint64_t channels = writer->info.channel_count;
    if (multiply_overflows(frame_count, channels) ||
        multiply_overflows(frame_count * channels, sizeof(float)) ||
        writer->info.data_size_bytes >
            std::numeric_limits<uint64_t>::max() -
                frame_count * channels * sizeof(float)) {
        set_error(error, CROSSDSD_ERROR_OVERFLOW, "WAVE data size overflow");
        return CROSSDSD_ERROR_OVERFLOW;
    }
    const uint64_t byte_count = frame_count * channels * sizeof(float);
    if (writer->info.container == CROSSDSD_WAVE_RIFF &&
        writer->info.data_size_bytes + byte_count + writer->header_bytes - 8u >
            UINT32_MAX) {
        set_error(error, CROSSDSD_ERROR_OVERFLOW,
                  "RIFF output exceeds 4 GiB; use RF64");
        return CROSSDSD_ERROR_OVERFLOW;
    }
    if (byte_count > std::numeric_limits<size_t>::max()) {
        set_error(error, CROSSDSD_ERROR_OVERFLOW, "write is too large");
        return CROSSDSD_ERROR_OVERFLOW;
    }
    bool ok = true;
    if constexpr (std::endian::native == std::endian::little) {
        ok = write_bytes(writer->file, interleaved, static_cast<size_t>(byte_count));
    } else {
        for (uint64_t index = 0; ok && index < frame_count * channels; ++index) {
            ok = write_u32(writer->file, std::bit_cast<uint32_t>(interleaved[index]));
        }
    }
    if (!ok) {
        set_error(error, CROSSDSD_ERROR_IO, "cannot write WAVE samples");
        return CROSSDSD_ERROR_IO;
    }
    writer->info.frame_count += frame_count;
    writer->info.data_size_bytes += byte_count;
    set_error(error, CROSSDSD_OK, "");
    return CROSSDSD_OK;
}

crossdsd_result crossdsd_wave_writer_write_id3(
    crossdsd_wave_writer* writer, const uint8_t* id3_data, uint64_t id3_size,
    crossdsd_error* error) {
    if (!writer || !writer->file || (id3_size != 0 && !id3_data) ||
        writer->info.metadata_size_bytes != 0 || id3_size > UINT32_MAX) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                  "invalid ID3 metadata arguments");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    const uint64_t chunk_bytes = 8u + id3_size + (id3_size & 1u);
    if (writer->info.container == CROSSDSD_WAVE_RIFF &&
        writer->header_bytes - 8u + writer->info.data_size_bytes + chunk_bytes >
            UINT32_MAX) {
        set_error(error, CROSSDSD_ERROR_OVERFLOW,
                  "RIFF metadata exceeds 4 GiB; use RF64");
        return CROSSDSD_ERROR_OVERFLOW;
    }
    bool ok = write_fourcc(writer->file, "id3 ") &&
              write_u32(writer->file, static_cast<uint32_t>(id3_size)) &&
              write_bytes(writer->file, id3_data, static_cast<size_t>(id3_size));
    if (ok && (id3_size & 1u)) {
        const uint8_t padding = 0;
        ok = write_bytes(writer->file, &padding, 1);
    }
    if (!ok) {
        set_error(error, CROSSDSD_ERROR_IO, "cannot write ID3 metadata");
        return CROSSDSD_ERROR_IO;
    }
    writer->info.metadata_size_bytes = chunk_bytes;
    set_error(error, CROSSDSD_OK, "");
    return CROSSDSD_OK;
}

crossdsd_result crossdsd_wave_writer_get_info(
    const crossdsd_wave_writer* writer, crossdsd_wave_writer_info* info) {
    if (!writer || !info ||
        info->struct_size < sizeof(crossdsd_wave_writer_info)) {
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    *info = writer->info;
    return CROSSDSD_OK;
}

crossdsd_result crossdsd_wave_writer_close(
    crossdsd_wave_writer* writer, crossdsd_error* error) {
    if (!writer || !writer->file) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                  "invalid WAVE writer");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    FILE* file = writer->file;
    const auto info = writer->info;
    const uint64_t header_bytes = writer->header_bytes;
    const uint64_t fact_sample_offset = writer->fact_sample_offset;
    const uint64_t data_size_offset = writer->data_size_offset;
    writer->file = nullptr;
    bool ok = true;
    if (info.container == CROSSDSD_WAVE_RIFF) {
        ok = crossdsd::platform::seek_file(file, 4u) &&
             write_u32(file, static_cast<uint32_t>(
                                 header_bytes - 8u + info.data_size_bytes +
                                 info.metadata_size_bytes)) &&
             crossdsd::platform::seek_file(file, fact_sample_offset) &&
             write_u32(file, static_cast<uint32_t>(info.frame_count)) &&
             crossdsd::platform::seek_file(file, data_size_offset) &&
             write_u32(file, static_cast<uint32_t>(info.data_size_bytes));
    } else {
        ok = crossdsd::platform::seek_file(file, 20u) &&
             write_u64(
                 file, header_bytes - 8u + info.data_size_bytes +
                           info.metadata_size_bytes) &&
             write_u64(file, info.data_size_bytes) &&
             write_u64(file, info.frame_count) &&
             crossdsd::platform::seek_file(file, fact_sample_offset) &&
             write_u32(
                 file, info.frame_count > UINT32_MAX
                           ? UINT32_MAX
                           : static_cast<uint32_t>(info.frame_count));
    }
    if (ok) ok = std::fflush(file) == 0;
    if (std::fclose(file) != 0) ok = false;
    delete writer;
    if (!ok) {
        set_error(error, CROSSDSD_ERROR_IO, "cannot finalize WAVE file");
        return CROSSDSD_ERROR_IO;
    }
    set_error(error, CROSSDSD_OK, "");
    return CROSSDSD_OK;
}

}  // extern "C"
