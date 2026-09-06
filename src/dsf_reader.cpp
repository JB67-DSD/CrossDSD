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
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>

namespace {

constexpr uint64_t kDsdChunkSize = 28;
constexpr uint64_t kFmtChunkSize = 52;
constexpr uint64_t kDataHeaderSize = 12;
constexpr uint32_t kMaxChannels = 256;

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

uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8u) |
           (static_cast<uint32_t>(p[2]) << 16u) |
           (static_cast<uint32_t>(p[3]) << 24u);
}

uint64_t le64(const uint8_t* p) {
    return static_cast<uint64_t>(le32(p)) |
           (static_cast<uint64_t>(le32(p + 4)) << 32u);
}

void set_error(crossdsd_error* error, crossdsd_result code, uint64_t offset,
               const char* message) {
    if (!error) return;
    error->code = code;
    error->byte_offset = offset;
    std::snprintf(error->message, sizeof(error->message), "%s", message);
}

bool read_exact(FILE* file, uint64_t offset, void* output, size_t size) {
    return crossdsd::platform::seek_file(file, offset) &&
           std::fread(output, 1, size, file) == size;
}

crossdsd_dsd_rate classify_rate(uint32_t hz) {
    if (hz == 2'822'400u) return CROSSDSD_DSD64;
    if (hz == 5'644'800u) return CROSSDSD_DSD128;
    if (hz == 11'289'600u) return CROSSDSD_DSD256;
    if (hz == 22'579'200u) return CROSSDSD_DSD512;
    return CROSSDSD_DSD_RATE_UNKNOWN;
}

}  // namespace

struct crossdsd_reader {
    std::unique_ptr<FILE, int (*)(FILE*)> file{nullptr, std::fclose};
    crossdsd_stream_info info{};
    uint64_t logical_byte_position = 0;
    uint64_t valid_bytes_per_channel = 0;
};

extern "C" {

crossdsd_result crossdsd_reader_open_file(
    const char* path, crossdsd_reader** out_reader, crossdsd_error* error) {
    if (!path || !out_reader) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT, 0, "path and out_reader are required");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    *out_reader = nullptr;
    FILE* raw = crossdsd::platform::open_file(path, "rb");
    if (!raw) {
        set_error(error, CROSSDSD_ERROR_IO, 0, "cannot open file");
        return CROSSDSD_ERROR_IO;
    }
    std::unique_ptr<crossdsd_reader> reader;
    try {
        reader = std::make_unique<crossdsd_reader>();
    } catch (const std::bad_alloc&) {
        std::fclose(raw);
        set_error(error, CROSSDSD_ERROR_OUT_OF_MEMORY, 0, "out of memory");
        return CROSSDSD_ERROR_OUT_OF_MEMORY;
    }
    reader->file.reset(raw);

    if (!crossdsd::platform::seek_file(raw, 0) ||
        !crossdsd::platform::seek_file_end(raw)) {
        set_error(error, CROSSDSD_ERROR_IO, 0, "cannot determine file size");
        return CROSSDSD_ERROR_IO;
    }
    const auto end = crossdsd::platform::tell_file(raw);
    if (end < 0) {
        set_error(error, CROSSDSD_ERROR_IO, 0, "cannot determine file size");
        return CROSSDSD_ERROR_IO;
    }
    const uint64_t actual_file_size = static_cast<uint64_t>(end);

    std::array<uint8_t, kDsdChunkSize> dsd{};
    if (!read_exact(raw, 0, dsd.data(), dsd.size())) {
        set_error(error, CROSSDSD_ERROR_TRUNCATED, 0, "truncated DSD chunk");
        return CROSSDSD_ERROR_TRUNCATED;
    }
    if (std::memcmp(dsd.data(), "DSD ", 4) != 0) {
        set_error(error, CROSSDSD_ERROR_INVALID_SIGNATURE, 0, "missing DSD signature");
        return CROSSDSD_ERROR_INVALID_SIGNATURE;
    }
    if (le64(dsd.data() + 4) != kDsdChunkSize) {
        set_error(error, CROSSDSD_ERROR_INVALID_CHUNK, 4, "DSD chunk size must be 28");
        return CROSSDSD_ERROR_INVALID_CHUNK;
    }
    const uint64_t declared_file_size = le64(dsd.data() + 12);
    const uint64_t metadata_offset = le64(dsd.data() + 20);
    if (declared_file_size != actual_file_size) {
        set_error(error, CROSSDSD_ERROR_INVALID_CHUNK, 12, "declared file size differs from actual size");
        return CROSSDSD_ERROR_INVALID_CHUNK;
    }

    std::array<uint8_t, kFmtChunkSize> fmt{};
    if (!read_exact(raw, kDsdChunkSize, fmt.data(), fmt.size())) {
        set_error(error, CROSSDSD_ERROR_TRUNCATED, kDsdChunkSize, "truncated fmt chunk");
        return CROSSDSD_ERROR_TRUNCATED;
    }
    if (std::memcmp(fmt.data(), "fmt ", 4) != 0 || le64(fmt.data() + 4) != kFmtChunkSize) {
        set_error(error, CROSSDSD_ERROR_INVALID_CHUNK, kDsdChunkSize, "invalid or missing fmt chunk");
        return CROSSDSD_ERROR_INVALID_CHUNK;
    }
    const uint32_t format_version = le32(fmt.data() + 12);
    const uint32_t format_id = le32(fmt.data() + 16);
    const uint32_t channel_type = le32(fmt.data() + 20);
    const uint32_t channels = le32(fmt.data() + 24);
    const uint32_t sample_rate = le32(fmt.data() + 28);
    const uint32_t bits_per_sample = le32(fmt.data() + 32);
    const uint64_t sample_count = le64(fmt.data() + 36);
    const uint32_t block_size = le32(fmt.data() + 44);
    if (format_version != 1 || format_id != 0) {
        set_error(error, CROSSDSD_ERROR_UNSUPPORTED_FORMAT, 40, "unsupported DSF format version or id");
        return CROSSDSD_ERROR_UNSUPPORTED_FORMAT;
    }
    if (channels == 0 || channels > kMaxChannels) {
        set_error(error, CROSSDSD_ERROR_UNSUPPORTED_CHANNELS, 52, "channel count must be between 1 and 256");
        return CROSSDSD_ERROR_UNSUPPORTED_CHANNELS;
    }
    const crossdsd_dsd_rate rate = classify_rate(sample_rate);
    if (rate == CROSSDSD_DSD_RATE_UNKNOWN) {
        set_error(error, CROSSDSD_ERROR_UNSUPPORTED_RATE, 56, "only DSD64, DSD128, DSD256 and DSD512 are supported");
        return CROSSDSD_ERROR_UNSUPPORTED_RATE;
    }
    if (bits_per_sample != 1) {
        set_error(error, CROSSDSD_ERROR_UNSUPPORTED_FORMAT, 60, "only LSB-first DSF is supported");
        return CROSSDSD_ERROR_UNSUPPORTED_FORMAT;
    }
    if (block_size == 0) {
        set_error(error, CROSSDSD_ERROR_INVALID_CHUNK, 72, "block size per channel must be non-zero");
        return CROSSDSD_ERROR_INVALID_CHUNK;
    }
    if (sample_count == 0 || sample_count > std::numeric_limits<uint64_t>::max() - 7u) {
        set_error(error, CROSSDSD_ERROR_INVALID_SAMPLE_COUNT, 64, "invalid sample count");
        return CROSSDSD_ERROR_INVALID_SAMPLE_COUNT;
    }
    const uint64_t valid_bytes = (sample_count + 7u) / 8u;
    const uint64_t blocks = valid_bytes / block_size + (valid_bytes % block_size != 0);
    uint64_t stored_bytes_per_channel = 0;
    uint64_t expected_payload = 0;
    if (mul_overflow(blocks, block_size, stored_bytes_per_channel) ||
        mul_overflow(stored_bytes_per_channel, channels, expected_payload)) {
        set_error(error, CROSSDSD_ERROR_OVERFLOW, 64, "audio size calculation overflow");
        return CROSSDSD_ERROR_OVERFLOW;
    }

    const uint64_t data_header_offset = kDsdChunkSize + kFmtChunkSize;
    std::array<uint8_t, kDataHeaderSize> data{};
    if (!read_exact(raw, data_header_offset, data.data(), data.size())) {
        set_error(error, CROSSDSD_ERROR_TRUNCATED, data_header_offset, "truncated data chunk");
        return CROSSDSD_ERROR_TRUNCATED;
    }
    if (std::memcmp(data.data(), "data", 4) != 0) {
        set_error(error, CROSSDSD_ERROR_INVALID_CHUNK, data_header_offset, "missing data chunk");
        return CROSSDSD_ERROR_INVALID_CHUNK;
    }
    const uint64_t data_chunk_size = le64(data.data() + 4);
    const uint64_t actual_payload =
        data_chunk_size >= kDataHeaderSize ? data_chunk_size - kDataHeaderSize : 0;
    uint64_t block_group_size = 0;
    if (mul_overflow(block_size, channels, block_group_size)) {
        set_error(error, CROSSDSD_ERROR_OVERFLOW, 72, "block group size calculation overflow");
        return CROSSDSD_ERROR_OVERFLOW;
    }
    /*
     * Some otherwise interoperable DSF writers append one complete block group
     * when the declared sample count already ends on a block boundary. Treat
     * that group as padding, while continuing to reject truncated, partial or
     * arbitrarily oversized payloads.
     */
    const bool canonical_payload = actual_payload == expected_payload;
    const bool one_extra_padding_group =
        actual_payload > expected_payload &&
        actual_payload - expected_payload == block_group_size;
    if (data_chunk_size < kDataHeaderSize ||
        (!canonical_payload && !one_extra_padding_group)) {
        set_error(error, CROSSDSD_ERROR_INVALID_SAMPLE_COUNT, data_header_offset + 4,
                  "data size is inconsistent with sample count, channels and block size");
        return CROSSDSD_ERROR_INVALID_SAMPLE_COUNT;
    }
    uint64_t data_end = 0;
    if (add_overflow(data_header_offset, data_chunk_size, data_end) || data_end > actual_file_size) {
        set_error(error, CROSSDSD_ERROR_TRUNCATED, data_header_offset, "audio data extends past end of file");
        return CROSSDSD_ERROR_TRUNCATED;
    }
    if (metadata_offset != 0 && (metadata_offset != data_end || metadata_offset >= actual_file_size)) {
        set_error(error, CROSSDSD_ERROR_INVALID_METADATA, 20, "metadata offset is outside the expected file region");
        return CROSSDSD_ERROR_INVALID_METADATA;
    }

    crossdsd_metadata_status metadata_status = CROSSDSD_METADATA_NONE;
    uint64_t metadata_size = 0;
    if (metadata_offset != 0) {
        metadata_size = actual_file_size - metadata_offset;
        std::array<uint8_t, 3> id3{};
        if (!read_exact(raw, metadata_offset, id3.data(), id3.size())) {
            set_error(error, CROSSDSD_ERROR_TRUNCATED, metadata_offset, "truncated metadata");
            return CROSSDSD_ERROR_TRUNCATED;
        }
        metadata_status = std::memcmp(id3.data(), "ID3", 3) == 0
                              ? CROSSDSD_METADATA_ID3
                              : CROSSDSD_METADATA_INVALID;
    } else if (data_end != actual_file_size) {
        set_error(error, CROSSDSD_ERROR_INVALID_CHUNK, data_end, "unreferenced bytes follow audio data");
        return CROSSDSD_ERROR_INVALID_CHUNK;
    }

    auto& info = reader->info;
    info.struct_size = sizeof(info);
    info.api_version = CROSSDSD_FORMATS_API_VERSION;
    info.format_version = format_version;
    info.format_id = format_id;
    info.channel_type = channel_type;
    info.channel_count = channels;
    info.sample_rate_hz = sample_rate;
    info.dsd_rate = rate;
    info.bit_order = CROSSDSD_BIT_ORDER_LSB_FIRST;
    info.sample_count_per_channel = sample_count;
    info.duration_seconds = static_cast<double>(sample_count) / sample_rate;
    info.block_size_per_channel = block_size;
    info.audio_offset = data_header_offset + kDataHeaderSize;
    info.audio_size = actual_payload;
    const uint64_t physical_bytes_per_channel = actual_payload / channels;
    info.stored_bits_per_channel = physical_bytes_per_channel * 8u;
    info.padding_bits_per_channel = info.stored_bits_per_channel - sample_count;
    info.metadata_offset = metadata_offset;
    info.metadata_size = metadata_size;
    info.metadata_status = metadata_status;
    reader->valid_bytes_per_channel = valid_bytes;
    *out_reader = reader.release();
    set_error(error, CROSSDSD_OK, 0, "");
    return CROSSDSD_OK;
}

void crossdsd_reader_close(crossdsd_reader* reader) { delete reader; }

crossdsd_result crossdsd_reader_get_info(
    const crossdsd_reader* reader, crossdsd_stream_info* info) {
    if (!reader || !info || info->struct_size < sizeof(crossdsd_stream_info)) {
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    *info = reader->info;
    return CROSSDSD_OK;
}

crossdsd_result crossdsd_reader_read_packed(
    crossdsd_reader* reader, uint8_t* output_planar, uint64_t capacity_per_channel,
    uint64_t* bytes_read_per_channel, uint8_t* valid_bits_in_last_byte,
    crossdsd_error* error) {
    if (!reader || !output_planar || capacity_per_channel == 0 ||
        !bytes_read_per_channel || !valid_bits_in_last_byte) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT, 0, "invalid read arguments");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    *bytes_read_per_channel = 0;
    *valid_bits_in_last_byte = 0;
    if (reader->logical_byte_position >= reader->valid_bytes_per_channel) {
        return CROSSDSD_ERROR_END_OF_STREAM;
    }
    uint64_t total_output = 0;
    if (mul_overflow(capacity_per_channel, reader->info.channel_count, total_output)) {
        set_error(error, CROSSDSD_ERROR_OVERFLOW, 0, "output size overflow");
        return CROSSDSD_ERROR_OVERFLOW;
    }
    (void)total_output;
    const uint64_t count = std::min(
        capacity_per_channel, reader->valid_bytes_per_channel - reader->logical_byte_position);
    const uint64_t block_size = reader->info.block_size_per_channel;
    const uint64_t channels = reader->info.channel_count;

    for (uint64_t channel = 0; channel < channels; ++channel) {
        uint64_t done = 0;
        while (done < count) {
            const uint64_t logical = reader->logical_byte_position + done;
            const uint64_t block_index = logical / block_size;
            const uint64_t within = logical % block_size;
            const uint64_t part = std::min(count - done, block_size - within);
            uint64_t group_offset = 0, channel_offset = 0, physical = 0;
            if (mul_overflow(block_index, block_size * channels, group_offset) ||
                mul_overflow(channel, block_size, channel_offset) ||
                add_overflow(reader->info.audio_offset, group_offset, physical) ||
                add_overflow(physical, channel_offset, physical) ||
                add_overflow(physical, within, physical)) {
                set_error(error, CROSSDSD_ERROR_OVERFLOW, 0, "read offset overflow");
                return CROSSDSD_ERROR_OVERFLOW;
            }
            if (!read_exact(reader->file.get(), physical,
                            output_planar + channel * capacity_per_channel + done,
                            static_cast<size_t>(part))) {
                set_error(error, CROSSDSD_ERROR_IO, physical, "cannot read audio data");
                return CROSSDSD_ERROR_IO;
            }
            done += part;
        }
    }
    reader->logical_byte_position += count;
    uint8_t valid_bits = 8;
    if (reader->logical_byte_position == reader->valid_bytes_per_channel &&
        reader->info.sample_count_per_channel % 8u != 0) {
        valid_bits = static_cast<uint8_t>(reader->info.sample_count_per_channel % 8u);
        const uint8_t mask = static_cast<uint8_t>((1u << valid_bits) - 1u);
        for (uint64_t channel = 0; channel < channels; ++channel) {
            output_planar[channel * capacity_per_channel + count - 1] &= mask;
        }
    }
    *bytes_read_per_channel = count;
    *valid_bits_in_last_byte = valid_bits;
    set_error(error, CROSSDSD_OK, 0, "");
    return CROSSDSD_OK;
}

crossdsd_result crossdsd_reader_seek_sample(
    crossdsd_reader* reader, uint64_t sample_offset, crossdsd_error* error) {
    if (!reader || sample_offset > reader->info.sample_count_per_channel ||
        sample_offset % 8u != 0) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT, 0,
                  "seek offset must be byte-aligned and within the valid sample range");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    reader->logical_byte_position = sample_offset / 8u;
    return CROSSDSD_OK;
}

crossdsd_result crossdsd_expand_dsd_lsb(
    const uint8_t* packed, uint64_t valid_bit_count, int8_t* expanded,
    uint64_t expanded_capacity) {
    if ((!packed && valid_bit_count != 0) || (!expanded && valid_bit_count != 0) ||
        expanded_capacity < valid_bit_count) {
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    for (uint64_t bit = 0; bit < valid_bit_count; ++bit) {
        expanded[bit] = ((packed[bit / 8u] >> (bit % 8u)) & 1u) ? 1 : -1;
    }
    return CROSSDSD_OK;
}

crossdsd_result crossdsd_reader_read_metadata(
    crossdsd_reader* reader, uint8_t* output, uint64_t output_capacity,
    uint64_t* bytes_read, crossdsd_error* error) {
    if (!reader || !bytes_read || (reader->info.metadata_size != 0 && !output)) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT, 0, "invalid metadata arguments");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    *bytes_read = 0;
    if (reader->info.metadata_size == 0) return CROSSDSD_OK;
    if (output_capacity < reader->info.metadata_size ||
        reader->info.metadata_size > std::numeric_limits<size_t>::max()) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT, reader->info.metadata_offset,
                  "metadata output buffer is too small");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    if (!read_exact(reader->file.get(), reader->info.metadata_offset, output,
                    static_cast<size_t>(reader->info.metadata_size))) {
        set_error(error, CROSSDSD_ERROR_IO, reader->info.metadata_offset, "cannot read metadata");
        return CROSSDSD_ERROR_IO;
    }
    *bytes_read = reader->info.metadata_size;
    return CROSSDSD_OK;
}

const char* crossdsd_result_string(crossdsd_result result) {
    switch (result) {
        case CROSSDSD_OK: return "ok";
        case CROSSDSD_ERROR_INVALID_ARGUMENT: return "invalid argument";
        case CROSSDSD_ERROR_IO: return "I/O error";
        case CROSSDSD_ERROR_TRUNCATED: return "truncated file";
        case CROSSDSD_ERROR_INVALID_SIGNATURE: return "invalid signature";
        case CROSSDSD_ERROR_INVALID_CHUNK: return "invalid chunk";
        case CROSSDSD_ERROR_UNSUPPORTED_FORMAT: return "unsupported format";
        case CROSSDSD_ERROR_UNSUPPORTED_RATE: return "unsupported DSD rate";
        case CROSSDSD_ERROR_UNSUPPORTED_CHANNELS: return "unsupported channel count";
        case CROSSDSD_ERROR_INVALID_SAMPLE_COUNT: return "invalid sample count";
        case CROSSDSD_ERROR_INVALID_METADATA: return "invalid metadata";
        case CROSSDSD_ERROR_OVERFLOW: return "integer overflow";
        case CROSSDSD_ERROR_END_OF_STREAM: return "end of stream";
        case CROSSDSD_ERROR_OUT_OF_MEMORY: return "out of memory";
        case CROSSDSD_ERROR_CANCELLED: return "cancelled";
    }
    return "unknown error";
}

}  // extern "C"
