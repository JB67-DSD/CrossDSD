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
#include "platform_file.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <vector>

namespace {

constexpr uint64_t kDsdChunkSize = 28;
constexpr uint64_t kFmtChunkSize = 52;
constexpr uint64_t kDataHeaderSize = 12;
constexpr uint64_t kAudioOffset = kDsdChunkSize + kFmtChunkSize + kDataHeaderSize;
constexpr uint32_t kDefaultBlockSize = 4096;
constexpr uint32_t kMaxChannels = 256;

void set_error(crossdsd_error* error, crossdsd_result code, uint64_t offset,
               const char* message) {
    if (!error) return;
    error->code = code;
    error->byte_offset = offset;
    std::snprintf(error->message, sizeof(error->message), "%s", message);
}

bool add_overflow(uint64_t a, uint64_t b, uint64_t& result) {
    if (a > std::numeric_limits<uint64_t>::max() - b) return true;
    result = a + b;
    return false;
}

bool mul_overflow(uint64_t a, uint64_t b, uint64_t& result) {
    if (a != 0 && b > std::numeric_limits<uint64_t>::max() / a) return true;
    result = a * b;
    return false;
}

bool write_bytes(FILE* file, const void* data, size_t size) {
    return size == 0 || std::fwrite(data, 1, size, file) == size;
}

bool write_u32(FILE* file, uint32_t value) {
    const uint8_t bytes[4]{
        static_cast<uint8_t>(value),
        static_cast<uint8_t>(value >> 8u),
        static_cast<uint8_t>(value >> 16u),
        static_cast<uint8_t>(value >> 24u),
    };
    return write_bytes(file, bytes, sizeof(bytes));
}

bool write_u64(FILE* file, uint64_t value) {
    const uint8_t bytes[8]{
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

crossdsd_dsd_rate classify_rate(uint32_t hz) {
    if (hz == 2'822'400u) return CROSSDSD_DSD64;
    if (hz == 5'644'800u) return CROSSDSD_DSD128;
    if (hz == 11'289'600u) return CROSSDSD_DSD256;
    if (hz == 22'579'200u) return CROSSDSD_DSD512;
    return CROSSDSD_DSD_RATE_UNKNOWN;
}

}  // namespace

struct crossdsd_dsf_writer {
    std::unique_ptr<FILE, int (*)(FILE*)> file{nullptr, std::fclose};
    crossdsd_dsf_writer_info info{};
    std::vector<uint8_t> block;
    std::vector<uint8_t> metadata;
    uint32_t block_fill = 0;
    bool audio_sealed = false;
    bool metadata_set = false;
    bool failed = false;
};

namespace {

bool write_placeholder_header(crossdsd_dsf_writer& writer) {
    FILE* file = writer.file.get();
    const auto& info = writer.info;
    return write_bytes(file, "DSD ", 4) &&
           write_u64(file, kDsdChunkSize) &&
           write_u64(file, 0) &&
           write_u64(file, 0) &&
           write_bytes(file, "fmt ", 4) &&
           write_u64(file, kFmtChunkSize) &&
           write_u32(file, 1) &&
           write_u32(file, 0) &&
           write_u32(file, info.channel_type) &&
           write_u32(file, info.channel_count) &&
           write_u32(file, info.sample_rate_hz) &&
           write_u32(file, 1) &&
           write_u64(file, 0) &&
           write_u32(file, info.block_size_per_channel) &&
           write_u32(file, 0) &&
           write_bytes(file, "data", 4) &&
           write_u64(file, 0);
}

bool flush_block(crossdsd_dsf_writer& writer, bool final_block) {
    if (writer.block_fill == 0) return true;
    const uint32_t block_size = writer.info.block_size_per_channel;
    if (final_block && writer.block_fill < block_size) {
        for (uint32_t channel = 0; channel < writer.info.channel_count; ++channel) {
            std::memset(
                writer.block.data() + static_cast<uint64_t>(channel) * block_size +
                    writer.block_fill,
                0, block_size - writer.block_fill);
        }
    }
    if (!write_bytes(
            writer.file.get(), writer.block.data(),
            static_cast<size_t>(block_size) * writer.info.channel_count)) {
        return false;
    }
    writer.info.stored_bytes_per_channel += block_size;
    writer.info.audio_size +=
        static_cast<uint64_t>(block_size) * writer.info.channel_count;
    writer.block_fill = 0;
    std::fill(writer.block.begin(), writer.block.end(), 0);
    return true;
}

bool patch_header(crossdsd_dsf_writer& writer, uint64_t file_size,
                  uint64_t metadata_offset) {
    FILE* file = writer.file.get();
    return crossdsd::platform::seek_file(file, 12) &&
           write_u64(file, file_size) &&
           write_u64(file, metadata_offset) &&
           crossdsd::platform::seek_file(file, 64) &&
           write_u64(file, writer.info.sample_count_per_channel) &&
           crossdsd::platform::seek_file(file, 84) &&
           write_u64(file, kDataHeaderSize + writer.info.audio_size);
}

}  // namespace

extern "C" {

crossdsd_result crossdsd_dsf_writer_open_file(
    const char* path, const crossdsd_dsf_writer_config* config,
    crossdsd_dsf_writer** out_writer, crossdsd_error* error) {
    if (!path || !config ||
        config->struct_size < sizeof(crossdsd_dsf_writer_config) ||
        !out_writer) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT, 0,
                  "invalid DSF writer arguments");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    *out_writer = nullptr;
    const auto dsd_rate = classify_rate(config->sample_rate_hz);
    if (dsd_rate == CROSSDSD_DSD_RATE_UNKNOWN) {
        set_error(error, CROSSDSD_ERROR_UNSUPPORTED_RATE, 0,
                  "only DSD64, DSD128, DSD256 and DSD512 are supported");
        return CROSSDSD_ERROR_UNSUPPORTED_RATE;
    }
    if (config->channel_count == 0 || config->channel_count > kMaxChannels) {
        set_error(error, CROSSDSD_ERROR_UNSUPPORTED_CHANNELS, 0,
                  "channel count must be between 1 and 256");
        return CROSSDSD_ERROR_UNSUPPORTED_CHANNELS;
    }
    uint32_t channel_type = config->channel_type;
    if (channel_type == 0) {
        if (config->channel_count > 2) {
            set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT, 0,
                      "channel type is required for multichannel DSF");
            return CROSSDSD_ERROR_INVALID_ARGUMENT;
        }
        channel_type = config->channel_count;
    }
    const uint32_t block_size =
        config->block_size_per_channel == 0
            ? kDefaultBlockSize
            : config->block_size_per_channel;
    uint64_t block_bytes = 0;
    if (mul_overflow(block_size, config->channel_count, block_bytes) ||
        block_bytes > std::numeric_limits<size_t>::max()) {
        set_error(error, CROSSDSD_ERROR_OVERFLOW, 0,
                  "DSF block size calculation overflow");
        return CROSSDSD_ERROR_OVERFLOW;
    }

    FILE* raw = crossdsd::platform::open_file(path, "w+b");
    if (!raw) {
        set_error(error, CROSSDSD_ERROR_IO, 0, "cannot create DSF file");
        return CROSSDSD_ERROR_IO;
    }
    std::unique_ptr<crossdsd_dsf_writer> writer;
    try {
        writer = std::make_unique<crossdsd_dsf_writer>();
        writer->block.resize(static_cast<size_t>(block_bytes), 0);
    } catch (const std::bad_alloc&) {
        std::fclose(raw);
        set_error(error, CROSSDSD_ERROR_OUT_OF_MEMORY, 0, "out of memory");
        return CROSSDSD_ERROR_OUT_OF_MEMORY;
    }
    writer->file.reset(raw);
    auto& info = writer->info;
    info.struct_size = sizeof(info);
    info.api_version = CROSSDSD_FORMATS_API_VERSION;
    info.sample_rate_hz = config->sample_rate_hz;
    info.dsd_rate = dsd_rate;
    info.channel_count = config->channel_count;
    info.channel_type = channel_type;
    info.block_size_per_channel = block_size;
    if (!write_placeholder_header(*writer)) {
        set_error(error, CROSSDSD_ERROR_IO, 0, "cannot write DSF header");
        return CROSSDSD_ERROR_IO;
    }
    *out_writer = writer.release();
    set_error(error, CROSSDSD_OK, 0, "");
    return CROSSDSD_OK;
}

crossdsd_result crossdsd_dsf_writer_write_packed(
    crossdsd_dsf_writer* writer, const uint8_t* packed_planar,
    uint64_t bytes_per_channel, uint8_t valid_bits_in_last_byte,
    crossdsd_error* error) {
    if (!writer || !writer->file || writer->failed || writer->audio_sealed ||
        !packed_planar || bytes_per_channel == 0 ||
        valid_bits_in_last_byte == 0 || valid_bits_in_last_byte > 8) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT, 0,
                  "invalid DSF packed write");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    uint64_t input_size = 0;
    if (mul_overflow(bytes_per_channel, writer->info.channel_count, input_size) ||
        input_size > std::numeric_limits<size_t>::max()) {
        set_error(error, CROSSDSD_ERROR_OVERFLOW, 0, "DSF input size overflow");
        return CROSSDSD_ERROR_OVERFLOW;
    }
    (void)input_size;
    if (valid_bits_in_last_byte < 8) {
        const uint8_t invalid_mask =
            static_cast<uint8_t>(~((1u << valid_bits_in_last_byte) - 1u));
        for (uint32_t channel = 0; channel < writer->info.channel_count; ++channel) {
            if ((packed_planar[
                     static_cast<uint64_t>(channel) * bytes_per_channel +
                     bytes_per_channel - 1u] &
                 invalid_mask) != 0) {
                set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT, 0,
                          "unused bits in final DSD byte must be zero");
                return CROSSDSD_ERROR_INVALID_ARGUMENT;
            }
        }
    }
    uint64_t added_bits = 0;
    if (mul_overflow(bytes_per_channel - 1u, 8u, added_bits) ||
        add_overflow(added_bits, valid_bits_in_last_byte, added_bits) ||
        add_overflow(
            writer->info.sample_count_per_channel, added_bits,
            writer->info.sample_count_per_channel)) {
        set_error(error, CROSSDSD_ERROR_OVERFLOW, 0, "DSF sample count overflow");
        return CROSSDSD_ERROR_OVERFLOW;
    }

    const uint32_t block_size = writer->info.block_size_per_channel;
    uint64_t consumed = 0;
    while (consumed < bytes_per_channel) {
        const uint32_t available = block_size - writer->block_fill;
        const uint64_t part = std::min<uint64_t>(
            available, bytes_per_channel - consumed);
        for (uint32_t channel = 0; channel < writer->info.channel_count; ++channel) {
            std::memcpy(
                writer->block.data() +
                    static_cast<uint64_t>(channel) * block_size +
                    writer->block_fill,
                packed_planar +
                    static_cast<uint64_t>(channel) * bytes_per_channel +
                    consumed,
                static_cast<size_t>(part));
        }
        writer->block_fill += static_cast<uint32_t>(part);
        consumed += part;
        if (writer->block_fill == block_size && !flush_block(*writer, false)) {
            writer->failed = true;
            set_error(error, CROSSDSD_ERROR_IO, 0, "cannot write DSF audio block");
            return CROSSDSD_ERROR_IO;
        }
    }
    writer->audio_sealed = valid_bits_in_last_byte < 8;
    set_error(error, CROSSDSD_OK, 0, "");
    return CROSSDSD_OK;
}

crossdsd_result crossdsd_dsf_writer_write_id3(
    crossdsd_dsf_writer* writer, const uint8_t* id3_data, uint64_t id3_size,
    crossdsd_error* error) {
    if (!writer || !writer->file || writer->failed || writer->metadata_set ||
        !id3_data || id3_size < 10 || id3_size > std::numeric_limits<size_t>::max() ||
        std::memcmp(id3_data, "ID3", 3) != 0) {
        set_error(error, CROSSDSD_ERROR_INVALID_METADATA, 0,
                  "metadata must be one complete ID3v2 tag");
        return CROSSDSD_ERROR_INVALID_METADATA;
    }
    try {
        writer->metadata.assign(id3_data, id3_data + id3_size);
    } catch (const std::bad_alloc&) {
        set_error(error, CROSSDSD_ERROR_OUT_OF_MEMORY, 0, "out of memory");
        return CROSSDSD_ERROR_OUT_OF_MEMORY;
    }
    writer->metadata_set = true;
    writer->info.metadata_size = id3_size;
    set_error(error, CROSSDSD_OK, 0, "");
    return CROSSDSD_OK;
}

crossdsd_result crossdsd_dsf_writer_get_info(
    const crossdsd_dsf_writer* writer, crossdsd_dsf_writer_info* info) {
    if (!writer || !info ||
        info->struct_size < sizeof(crossdsd_dsf_writer_info)) {
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    *info = writer->info;
    return CROSSDSD_OK;
}

crossdsd_result crossdsd_dsf_writer_close(
    crossdsd_dsf_writer* writer, crossdsd_error* error) {
    if (!writer || !writer->file) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT, 0,
                  "invalid DSF writer");
        delete writer;
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    crossdsd_result result = CROSSDSD_OK;
    if (writer->failed) {
        result = CROSSDSD_ERROR_IO;
        set_error(error, result, 0, "DSF writer is in a failed state");
    } else if (writer->info.sample_count_per_channel == 0) {
        result = CROSSDSD_ERROR_INVALID_SAMPLE_COUNT;
        set_error(error, result, 0, "DSF output contains no samples");
    } else if (!flush_block(*writer, true)) {
        result = CROSSDSD_ERROR_IO;
        set_error(error, result, 0, "cannot flush final DSF audio block");
    } else {
        const uint64_t metadata_offset =
            writer->metadata.empty() ? 0 : kAudioOffset + writer->info.audio_size;
        uint64_t file_size = kAudioOffset;
        if (add_overflow(file_size, writer->info.audio_size, file_size) ||
            add_overflow(file_size, writer->info.metadata_size, file_size)) {
            result = CROSSDSD_ERROR_OVERFLOW;
            set_error(error, result, 0, "DSF file size overflow");
        } else if (!write_bytes(
                       writer->file.get(), writer->metadata.data(),
                       writer->metadata.size()) ||
                   !patch_header(*writer, file_size, metadata_offset) ||
                   std::fflush(writer->file.get()) != 0) {
            result = CROSSDSD_ERROR_IO;
            set_error(error, result, 0, "cannot finalize DSF file");
        } else {
            set_error(error, CROSSDSD_OK, 0, "");
        }
    }
    writer->file.reset();
    delete writer;
    return result;
}

}  // extern "C"
