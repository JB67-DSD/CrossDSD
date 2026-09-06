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

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>

namespace {

void set_error(
    crossdsd_error* error, crossdsd_result code, const char* message,
    uint64_t offset = 0) {
    if (!error || error->struct_size < sizeof(crossdsd_error)) return;
    error->code = code;
    error->byte_offset = offset;
    std::snprintf(error->message, sizeof(error->message), "%s", message);
}

bool read_bytes(FILE* file, void* output, size_t size) {
    return std::fread(output, 1, size, file) == size;
}

bool read_u16(FILE* file, uint16_t& value) {
    uint8_t bytes[2]{};
    if (!read_bytes(file, bytes, sizeof(bytes))) return false;
    value = static_cast<uint16_t>(
        static_cast<uint16_t>(bytes[0]) |
        static_cast<uint16_t>(static_cast<uint16_t>(bytes[1]) << 8u));
    return true;
}

bool read_u32(FILE* file, uint32_t& value) {
    uint8_t bytes[4]{};
    if (!read_bytes(file, bytes, sizeof(bytes))) return false;
    value = static_cast<uint32_t>(bytes[0]) |
            (static_cast<uint32_t>(bytes[1]) << 8u) |
            (static_cast<uint32_t>(bytes[2]) << 16u) |
            (static_cast<uint32_t>(bytes[3]) << 24u);
    return true;
}

bool read_u64(FILE* file, uint64_t& value) {
    uint8_t bytes[8]{};
    if (!read_bytes(file, bytes, sizeof(bytes))) return false;
    value = 0;
    for (unsigned index = 0; index < 8; ++index) {
        value |= static_cast<uint64_t>(bytes[index]) << (index * 8u);
    }
    return true;
}

bool fourcc_is(const char value[4], const char expected[5]) {
    return std::memcmp(value, expected, 4) == 0;
}

bool add_overflows(uint64_t left, uint64_t right) {
    return left > std::numeric_limits<uint64_t>::max() - right;
}

}  // namespace

struct crossdsd_wave_reader {
    FILE* file = nullptr;
    crossdsd_wave_reader_info info{};
    uint64_t frames_read = 0;
};

extern "C" {

crossdsd_result crossdsd_wave_reader_open_file(
    const char* path, crossdsd_wave_reader** out_reader,
    crossdsd_error* error) {
    if (!path || !out_reader) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                  "invalid WAVE reader arguments");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    *out_reader = nullptr;
    FILE* file = crossdsd::platform::open_file(path, "rb");
    if (!file) {
        set_error(error, CROSSDSD_ERROR_IO, "cannot open WAVE input");
        return CROSSDSD_ERROR_IO;
    }
    const auto fail = [&](crossdsd_result result, const char* message,
                          uint64_t offset = 0) {
        std::fclose(file);
        set_error(error, result, message, offset);
        return result;
    };
    if (!crossdsd::platform::seek_file_end(file)) {
        return fail(CROSSDSD_ERROR_IO, "cannot determine WAVE size");
    }
    const int64_t signed_size = crossdsd::platform::tell_file(file);
    if (signed_size < 12 || !crossdsd::platform::seek_file(file, 0)) {
        return fail(CROSSDSD_ERROR_TRUNCATED, "truncated WAVE header");
    }
    const uint64_t file_size = static_cast<uint64_t>(signed_size);
    char container_id[4]{};
    uint32_t riff_size = 0;
    char wave_id[4]{};
    if (!read_bytes(file, container_id, 4) || !read_u32(file, riff_size) ||
        !read_bytes(file, wave_id, 4)) {
        return fail(CROSSDSD_ERROR_TRUNCATED, "truncated WAVE header");
    }
    (void)riff_size;
    const bool rf64 = fourcc_is(container_id, "RF64");
    if ((!fourcc_is(container_id, "RIFF") && !rf64) ||
        !fourcc_is(wave_id, "WAVE")) {
        return fail(CROSSDSD_ERROR_INVALID_SIGNATURE,
                    "input is not RIFF/WAVE or RF64");
    }

    bool have_ds64 = false;
    bool have_format = false;
    bool have_data = false;
    uint64_t rf64_data_size = 0;
    uint64_t rf64_frame_count = 0;
    uint16_t format_tag = 0;
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint16_t block_align = 0;
    uint16_t bits_per_sample = 0;
    bool extensible_float = false;
    uint32_t channel_mask = 0;
    uint64_t data_offset = 0;
    uint64_t data_size = 0;
    uint64_t metadata_offset = 0;
    uint64_t metadata_size = 0;

    uint64_t cursor = 12;
    while (cursor + 8u <= file_size) {
        if (!crossdsd::platform::seek_file(file, cursor)) {
            return fail(CROSSDSD_ERROR_IO, "cannot seek WAVE chunk", cursor);
        }
        char chunk_id[4]{};
        uint32_t chunk_size_32 = 0;
        if (!read_bytes(file, chunk_id, 4) || !read_u32(file, chunk_size_32)) {
            return fail(CROSSDSD_ERROR_TRUNCATED, "truncated WAVE chunk", cursor);
        }
        uint64_t chunk_size = chunk_size_32;
        if (rf64 && fourcc_is(chunk_id, "data") &&
            chunk_size_32 == UINT32_MAX) {
            if (!have_ds64) {
                return fail(CROSSDSD_ERROR_INVALID_CHUNK,
                            "RF64 data precedes ds64", cursor);
            }
            chunk_size = rf64_data_size;
        }
        const uint64_t payload = cursor + 8u;
        if (add_overflows(payload, chunk_size) ||
            payload + chunk_size > file_size) {
            return fail(CROSSDSD_ERROR_TRUNCATED,
                        "WAVE chunk exceeds file size", cursor);
        }

        if (fourcc_is(chunk_id, "ds64")) {
            uint64_t ignored_riff_size = 0;
            uint32_t table_count = 0;
            if (!rf64 || chunk_size < 28u ||
                !read_u64(file, ignored_riff_size) ||
                !read_u64(file, rf64_data_size) ||
                !read_u64(file, rf64_frame_count) ||
                !read_u32(file, table_count)) {
                return fail(CROSSDSD_ERROR_INVALID_CHUNK,
                            "invalid RF64 ds64 chunk", cursor);
            }
            (void)ignored_riff_size;
            (void)table_count;
            have_ds64 = true;
        } else if (fourcc_is(chunk_id, "fmt ")) {
            uint32_t byte_rate = 0;
            if (chunk_size < 16u || !read_u16(file, format_tag) ||
                !read_u16(file, channels) || !read_u32(file, sample_rate) ||
                !read_u32(file, byte_rate) || !read_u16(file, block_align) ||
                !read_u16(file, bits_per_sample)) {
                return fail(CROSSDSD_ERROR_INVALID_CHUNK,
                            "invalid WAVE fmt chunk", cursor);
            }
            (void)byte_rate;
            if (format_tag == 0xfffeu && chunk_size >= 40u) {
                uint16_t extension_size = 0;
                uint16_t valid_bits = 0;
                uint8_t subtype[16]{};
                if (!read_u16(file, extension_size) ||
                    !read_u16(file, valid_bits) ||
                    !read_u32(file, channel_mask) ||
                    !read_bytes(file, subtype, sizeof(subtype))) {
                    return fail(CROSSDSD_ERROR_INVALID_CHUNK,
                                "invalid WAVE extensible format", cursor);
                }
                static constexpr uint8_t kFloatSubtype[16] = {
                    3, 0, 0, 0, 0, 0, 0x10, 0,
                    0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71};
                extensible_float =
                    extension_size >= 22u && valid_bits == 32u &&
                    std::memcmp(subtype, kFloatSubtype, sizeof(subtype)) == 0;
            }
            have_format = true;
        } else if (fourcc_is(chunk_id, "data")) {
            if (have_data) {
                return fail(CROSSDSD_ERROR_INVALID_CHUNK,
                            "multiple WAVE data chunks are unsupported", cursor);
            }
            data_offset = payload;
            data_size = chunk_size;
            have_data = true;
        } else if (fourcc_is(chunk_id, "id3 ") ||
                   fourcc_is(chunk_id, "ID3 ")) {
            metadata_offset = payload;
            metadata_size = chunk_size;
        }
        const uint64_t padded = chunk_size + (chunk_size & 1u);
        if (add_overflows(payload, padded)) {
            return fail(CROSSDSD_ERROR_OVERFLOW,
                        "WAVE chunk offset overflow", cursor);
        }
        cursor = payload + padded;
    }

    if (rf64 && !have_ds64) {
        return fail(CROSSDSD_ERROR_INVALID_CHUNK, "RF64 file has no ds64 chunk");
    }
    if (!have_format || !have_data) {
        return fail(CROSSDSD_ERROR_INVALID_CHUNK,
                    "WAVE file is missing fmt or data");
    }
    if (format_tag != 3u && !extensible_float) {
        return fail(CROSSDSD_ERROR_UNSUPPORTED_FORMAT,
                    "WAVE input must be IEEE float32");
    }
    if (channels == 0 || channels > 256u) {
        return fail(CROSSDSD_ERROR_UNSUPPORTED_CHANNELS,
                    "WAVE channel count must be between 1 and 256");
    }
    if (bits_per_sample != 32u ||
        block_align != static_cast<uint32_t>(channels) * sizeof(float)) {
        return fail(CROSSDSD_ERROR_UNSUPPORTED_FORMAT,
                    "WAVE input must use 32-bit IEEE float frames");
    }
    if (data_size % block_align != 0u) {
        return fail(CROSSDSD_ERROR_INVALID_SAMPLE_COUNT,
                    "WAVE data is not a whole number of frames");
    }
    const uint64_t frames = data_size / block_align;
    if (rf64_frame_count != 0 && rf64_frame_count != frames) {
        return fail(CROSSDSD_ERROR_INVALID_SAMPLE_COUNT,
                    "RF64 frame count disagrees with data size");
    }

    try {
        auto reader = std::make_unique<crossdsd_wave_reader>();
        reader->file = file;
        auto& info = reader->info;
        info.struct_size = sizeof(info);
        info.api_version = CROSSDSD_AUDIO_API_VERSION;
        info.sample_rate_hz = sample_rate;
        info.channel_count = channels;
        info.bits_per_sample = bits_per_sample;
        info.container = rf64 ? CROSSDSD_WAVE_RF64 : CROSSDSD_WAVE_RIFF;
        info.frame_count = frames;
        info.data_offset = data_offset;
        info.data_size_bytes = data_size;
        info.metadata_offset = metadata_offset;
        info.metadata_size_bytes = metadata_size;
        info.metadata_status =
            metadata_size == 0 ? CROSSDSD_METADATA_NONE
                               : CROSSDSD_METADATA_ID3;
        info.channel_mask = channel_mask;
        switch (channel_mask) {
        case 0x0004u: info.channel_type = 1; break;
        case 0x0003u: info.channel_type = 2; break;
        case 0x0007u: info.channel_type = 3; break;
        case 0x0033u: info.channel_type = 4; break;
        case 0x000fu: info.channel_type = 5; break;
        case 0x0037u: info.channel_type = 6; break;
        case 0x003fu: info.channel_type = 7; break;
        default: info.channel_type = 0; break;
        }
        if (!crossdsd::platform::seek_file(file, data_offset)) {
            return fail(CROSSDSD_ERROR_IO, "cannot seek WAVE audio");
        }
        *out_reader = reader.release();
    } catch (const std::bad_alloc&) {
        return fail(CROSSDSD_ERROR_OUT_OF_MEMORY, "out of memory");
    }
    set_error(error, CROSSDSD_OK, "");
    return CROSSDSD_OK;
}

void crossdsd_wave_reader_close(crossdsd_wave_reader* reader) {
    if (!reader) return;
    if (reader->file) std::fclose(reader->file);
    delete reader;
}

crossdsd_result crossdsd_wave_reader_get_info(
    const crossdsd_wave_reader* reader, crossdsd_wave_reader_info* info) {
    if (!reader || !info ||
        info->struct_size < sizeof(crossdsd_wave_reader_info)) {
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    *info = reader->info;
    return CROSSDSD_OK;
}

crossdsd_result crossdsd_wave_reader_read(
    crossdsd_wave_reader* reader, float* interleaved, uint64_t frame_capacity,
    uint64_t* frames_read, crossdsd_error* error) {
    if (!reader || !frames_read ||
        (frame_capacity != 0 && !interleaved)) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                  "invalid WAVE read arguments");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    *frames_read = 0;
    if (reader->frames_read == reader->info.frame_count) {
        set_error(error, CROSSDSD_ERROR_END_OF_STREAM, "end of WAVE stream");
        return CROSSDSD_ERROR_END_OF_STREAM;
    }
    const uint64_t count = std::min(
        frame_capacity, reader->info.frame_count - reader->frames_read);
    if (count == 0) {
        set_error(error, CROSSDSD_OK, "");
        return CROSSDSD_OK;
    }
    if (count >
        std::numeric_limits<uint64_t>::max() / reader->info.channel_count) {
        set_error(error, CROSSDSD_ERROR_OVERFLOW, "WAVE read is too large");
        return CROSSDSD_ERROR_OVERFLOW;
    }
    const uint64_t values = count * reader->info.channel_count;
    if (values > std::numeric_limits<size_t>::max() / sizeof(float)) {
        set_error(error, CROSSDSD_ERROR_OVERFLOW, "WAVE read is too large");
        return CROSSDSD_ERROR_OVERFLOW;
    }
    if (std::fread(
            interleaved, sizeof(float), static_cast<size_t>(values),
            reader->file) != values) {
        set_error(error, CROSSDSD_ERROR_TRUNCATED, "truncated WAVE audio");
        return CROSSDSD_ERROR_TRUNCATED;
    }
    reader->frames_read += count;
    *frames_read = count;
    set_error(error, CROSSDSD_OK, "");
    return CROSSDSD_OK;
}

crossdsd_result crossdsd_wave_reader_read_metadata(
    crossdsd_wave_reader* reader, uint8_t* output, uint64_t output_capacity,
    uint64_t* bytes_read, crossdsd_error* error) {
    if (!reader || !bytes_read ||
        (reader->info.metadata_size_bytes != 0 && !output) ||
        output_capacity < reader->info.metadata_size_bytes) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                  "invalid WAVE metadata read arguments");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    *bytes_read = 0;
    if (reader->info.metadata_size_bytes == 0) {
        set_error(error, CROSSDSD_OK, "");
        return CROSSDSD_OK;
    }
    if (reader->info.metadata_size_bytes > std::numeric_limits<size_t>::max() ||
        !crossdsd::platform::seek_file(
            reader->file, reader->info.metadata_offset) ||
        !read_bytes(
            reader->file, output,
            static_cast<size_t>(reader->info.metadata_size_bytes))) {
        set_error(error, CROSSDSD_ERROR_IO, "cannot read WAVE metadata");
        return CROSSDSD_ERROR_IO;
    }
    *bytes_read = reader->info.metadata_size_bytes;
    const uint64_t block_align =
        static_cast<uint64_t>(reader->info.channel_count) * sizeof(float);
    const uint64_t audio_position =
        reader->info.data_offset + reader->frames_read * block_align;
    if (!crossdsd::platform::seek_file(reader->file, audio_position)) {
        set_error(error, CROSSDSD_ERROR_IO,
                  "cannot restore WAVE audio position");
        return CROSSDSD_ERROR_IO;
    }
    set_error(error, CROSSDSD_OK, "");
    return CROSSDSD_OK;
}

}  // extern "C"
