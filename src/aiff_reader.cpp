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
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <vector>

namespace {

void set_error(crossdsd_error* error, crossdsd_result code,
               const char* message, uint64_t offset = 0) {
    if (!error || error->struct_size < sizeof(crossdsd_error)) return;
    error->code = code;
    error->byte_offset = offset;
    std::snprintf(error->message, sizeof(error->message), "%s", message);
}

bool read_bytes(FILE* file, void* output, size_t size) {
    return std::fread(output, 1, size, file) == size;
}

bool read_be16(FILE* file, uint16_t& value) {
    uint8_t bytes[2]{};
    if (!read_bytes(file, bytes, 2)) return false;
    value = static_cast<uint16_t>((bytes[0] << 8u) | bytes[1]);
    return true;
}

bool read_be32(FILE* file, uint32_t& value) {
    uint8_t bytes[4]{};
    if (!read_bytes(file, bytes, 4)) return false;
    value = (static_cast<uint32_t>(bytes[0]) << 24u) |
            (static_cast<uint32_t>(bytes[1]) << 16u) |
            (static_cast<uint32_t>(bytes[2]) << 8u) | bytes[3];
    return true;
}

bool fourcc_is(const char value[4], const char expected[5]) {
    return std::memcmp(value, expected, 4) == 0;
}

bool add_overflows(uint64_t left, uint64_t right) {
    return left > std::numeric_limits<uint64_t>::max() - right;
}

bool decode_extended80(const uint8_t bytes[10], uint32_t& result) {
    const bool negative = (bytes[0] & 0x80u) != 0;
    const uint16_t exponent =
        static_cast<uint16_t>(((bytes[0] & 0x7fu) << 8u) | bytes[1]);
    uint64_t mantissa = 0;
    for (unsigned index = 0; index < 8; ++index) {
        mantissa = (mantissa << 8u) | bytes[index + 2u];
    }
    if (negative || exponent == 0x7fffu || mantissa == 0) return false;
    const long double value = std::ldexp(
        static_cast<long double>(mantissa), static_cast<int>(exponent) -
            16383 - 63);
    if (!std::isfinite(value) || value < 1.0L ||
        value > static_cast<long double>(UINT32_MAX)) return false;
    const auto rounded = std::llround(value);
    if (std::fabs(value - rounded) > 0.01L) return false;
    result = static_cast<uint32_t>(rounded);
    return true;
}

}  // namespace

struct crossdsd_aiff_reader {
    FILE* file = nullptr;
    crossdsd_aiff_reader_info info{};
    uint64_t frames_read = 0;
    bool little_endian_pcm = false;
};

extern "C" {

crossdsd_result crossdsd_aiff_reader_open_file(
    const char* path, crossdsd_aiff_reader** out_reader,
    crossdsd_error* error) {
    if (!path || !out_reader) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                  "invalid AIFF reader arguments");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    *out_reader = nullptr;
    FILE* file = crossdsd::platform::open_file(path, "rb");
    if (!file) {
        set_error(error, CROSSDSD_ERROR_IO, "cannot open AIFF input");
        return CROSSDSD_ERROR_IO;
    }
    const auto fail = [&](crossdsd_result result, const char* message,
                          uint64_t offset = 0) {
        std::fclose(file);
        set_error(error, result, message, offset);
        return result;
    };
    if (!crossdsd::platform::seek_file_end(file))
        return fail(CROSSDSD_ERROR_IO, "cannot determine AIFF size");
    const int64_t signed_size = crossdsd::platform::tell_file(file);
    if (signed_size < 12 || !crossdsd::platform::seek_file(file, 0))
        return fail(CROSSDSD_ERROR_TRUNCATED, "truncated AIFF header");
    const uint64_t file_size = static_cast<uint64_t>(signed_size);
    char form[4]{}, type[4]{};
    uint32_t declared_size = 0;
    if (!read_bytes(file, form, 4) || !read_be32(file, declared_size) ||
        !read_bytes(file, type, 4) || !fourcc_is(form, "FORM") ||
        (!fourcc_is(type, "AIFF") && !fourcc_is(type, "AIFC"))) {
        return fail(CROSSDSD_ERROR_INVALID_SIGNATURE, "input is not AIFF/AIFC");
    }
    (void)declared_size;
    const bool aifc = fourcc_is(type, "AIFC");
    bool have_comm = false, have_ssnd = false;
    uint16_t channels = 0, bits = 0;
    uint32_t frames32 = 0, sample_rate = 0;
    uint64_t data_offset = 0, data_size = 0;
    uint64_t metadata_offset = 0, metadata_size = 0;
    bool little_endian_pcm = false;

    for (uint64_t cursor = 12; cursor + 8u <= file_size;) {
        if (!crossdsd::platform::seek_file(file, cursor))
            return fail(CROSSDSD_ERROR_IO, "cannot seek AIFF chunk", cursor);
        char id[4]{};
        uint32_t size32 = 0;
        if (!read_bytes(file, id, 4) || !read_be32(file, size32))
            return fail(CROSSDSD_ERROR_TRUNCATED, "truncated AIFF chunk", cursor);
        const uint64_t size = size32;
        const uint64_t payload = cursor + 8u;
        if (add_overflows(payload, size) || payload + size > file_size)
            return fail(CROSSDSD_ERROR_TRUNCATED,
                        "AIFF chunk exceeds file size", cursor);

        if (fourcc_is(id, "COMM")) {
            uint8_t rate[10]{};
            if (size < (aifc ? 22u : 18u) || !read_be16(file, channels) ||
                !read_be32(file, frames32) || !read_be16(file, bits) ||
                !read_bytes(file, rate, sizeof(rate)) ||
                !decode_extended80(rate, sample_rate)) {
                return fail(CROSSDSD_ERROR_INVALID_CHUNK,
                            "invalid AIFF COMM chunk", cursor);
            }
            if (aifc) {
                char compression[4]{};
                if (!read_bytes(file, compression, 4)) {
                    return fail(CROSSDSD_ERROR_INVALID_CHUNK,
                                "invalid AIFC compression type", cursor);
                }
                if (fourcc_is(compression, "sowt")) {
                    little_endian_pcm = true;
                } else if (!fourcc_is(compression, "NONE")) {
                    return fail(CROSSDSD_ERROR_UNSUPPORTED_FORMAT,
                                "AIFC compression is unsupported", cursor);
                }
            }
            have_comm = true;
        } else if (fourcc_is(id, "SSND")) {
            uint32_t offset = 0, block_size = 0;
            if (have_ssnd || size < 8u || !read_be32(file, offset) ||
                !read_be32(file, block_size) || offset > size - 8u) {
                return fail(CROSSDSD_ERROR_INVALID_CHUNK,
                            "invalid AIFF SSND chunk", cursor);
            }
            (void)block_size;
            data_offset = payload + 8u + offset;
            data_size = size - 8u - offset;
            have_ssnd = true;
        } else if (fourcc_is(id, "ID3 ")) {
            metadata_offset = payload;
            metadata_size = size;
        }
        const uint64_t padded = size + (size & 1u);
        if (add_overflows(payload, padded))
            return fail(CROSSDSD_ERROR_OVERFLOW,
                        "AIFF chunk offset overflow", cursor);
        cursor = payload + padded;
    }
    if (!have_comm || !have_ssnd)
        return fail(CROSSDSD_ERROR_INVALID_CHUNK,
                    "AIFF file is missing COMM or SSND");
    if (channels == 0 || channels > 256u)
        return fail(CROSSDSD_ERROR_UNSUPPORTED_CHANNELS,
                    "AIFF channel count must be between 1 and 256");
    if (bits != 16u && bits != 24u && bits != 32u)
        return fail(CROSSDSD_ERROR_UNSUPPORTED_FORMAT,
                    "AIFF PCM must use 16, 24 or 32 bits");
    const uint64_t bytes_per_frame =
        static_cast<uint64_t>(channels) * ((bits + 7u) / 8u);
    const uint64_t expected = static_cast<uint64_t>(frames32) * bytes_per_frame;
    if (data_size < expected)
        return fail(CROSSDSD_ERROR_TRUNCATED, "truncated AIFF audio data");

    try {
        auto reader = std::make_unique<crossdsd_aiff_reader>();
        reader->file = file;
        reader->little_endian_pcm = little_endian_pcm;
        auto& info = reader->info;
        info.struct_size = sizeof(info);
        info.api_version = CROSSDSD_AUDIO_API_VERSION;
        info.sample_rate_hz = sample_rate;
        info.channel_count = channels;
        info.bits_per_sample = bits;
        info.frame_count = frames32;
        info.data_offset = data_offset;
        info.data_size_bytes = expected;
        info.metadata_offset = metadata_offset;
        info.metadata_size_bytes = metadata_size;
        info.metadata_status = metadata_size == 0 ? CROSSDSD_METADATA_NONE
                                                  : CROSSDSD_METADATA_ID3;
        info.channel_type = channels == 1u ? 1u : (channels == 2u ? 2u : 0u);
        if (!crossdsd::platform::seek_file(file, data_offset))
            return fail(CROSSDSD_ERROR_IO, "cannot seek AIFF audio");
        *out_reader = reader.release();
    } catch (const std::bad_alloc&) {
        return fail(CROSSDSD_ERROR_OUT_OF_MEMORY, "out of memory");
    }
    set_error(error, CROSSDSD_OK, "");
    return CROSSDSD_OK;
}

void crossdsd_aiff_reader_close(crossdsd_aiff_reader* reader) {
    if (!reader) return;
    if (reader->file) std::fclose(reader->file);
    delete reader;
}

crossdsd_result crossdsd_aiff_reader_get_info(
    const crossdsd_aiff_reader* reader, crossdsd_aiff_reader_info* info) {
    if (!reader || !info || info->struct_size < sizeof(*info))
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    *info = reader->info;
    return CROSSDSD_OK;
}

crossdsd_result crossdsd_aiff_reader_read(
    crossdsd_aiff_reader* reader, float* interleaved, uint64_t frame_capacity,
    uint64_t* frames_read, crossdsd_error* error) {
    if (!reader || !frames_read || (frame_capacity && !interleaved)) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                  "invalid AIFF read arguments");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    *frames_read = 0;
    if (reader->frames_read == reader->info.frame_count) {
        set_error(error, CROSSDSD_ERROR_END_OF_STREAM, "end of AIFF stream");
        return CROSSDSD_ERROR_END_OF_STREAM;
    }
    const uint64_t count = std::min(
        frame_capacity, reader->info.frame_count - reader->frames_read);
    const uint64_t values = count * reader->info.channel_count;
    const unsigned bytes_per_sample = (reader->info.bits_per_sample + 7u) / 8u;
    if (values > std::numeric_limits<size_t>::max() / bytes_per_sample) {
        set_error(error, CROSSDSD_ERROR_OVERFLOW, "AIFF read is too large");
        return CROSSDSD_ERROR_OVERFLOW;
    }
    try {
        std::vector<uint8_t> bytes(static_cast<size_t>(values) * bytes_per_sample);
        if (!bytes.empty() && !read_bytes(reader->file, bytes.data(), bytes.size())) {
            set_error(error, CROSSDSD_ERROR_TRUNCATED, "truncated AIFF audio");
            return CROSSDSD_ERROR_TRUNCATED;
        }
        const double scale = std::ldexp(1.0, reader->info.bits_per_sample - 1u);
        for (uint64_t index = 0; index < values; ++index) {
            const uint8_t* sample = bytes.data() + index * bytes_per_sample;
            uint64_t raw = 0u;
            if (reader->little_endian_pcm) {
                for (unsigned byte = bytes_per_sample; byte > 0; --byte)
                    raw = (raw << 8u) | sample[byte - 1u];
            } else {
                for (unsigned byte = 0; byte < bytes_per_sample; ++byte)
                    raw = (raw << 8u) | sample[byte];
            }
            const unsigned sample_bits = bytes_per_sample * 8u;
            if ((raw & (uint64_t{1} << (sample_bits - 1u))) != 0u)
                raw |= ~uint64_t{0} << sample_bits;
            const int64_t value = static_cast<int64_t>(raw);
            interleaved[index] = static_cast<float>(value / scale);
        }
    } catch (const std::bad_alloc&) {
        set_error(error, CROSSDSD_ERROR_OUT_OF_MEMORY, "out of memory");
        return CROSSDSD_ERROR_OUT_OF_MEMORY;
    }
    reader->frames_read += count;
    *frames_read = count;
    set_error(error, CROSSDSD_OK, "");
    return CROSSDSD_OK;
}

crossdsd_result crossdsd_aiff_reader_read_metadata(
    crossdsd_aiff_reader* reader, uint8_t* output, uint64_t output_capacity,
    uint64_t* bytes_read, crossdsd_error* error) {
    if (!reader || !bytes_read ||
        (reader->info.metadata_size_bytes && !output) ||
        output_capacity < reader->info.metadata_size_bytes) {
        set_error(error, CROSSDSD_ERROR_INVALID_ARGUMENT,
                  "invalid AIFF metadata read arguments");
        return CROSSDSD_ERROR_INVALID_ARGUMENT;
    }
    *bytes_read = 0;
    if (reader->info.metadata_size_bytes == 0) {
        set_error(error, CROSSDSD_OK, "");
        return CROSSDSD_OK;
    }
    if (reader->info.metadata_size_bytes > SIZE_MAX) {
        set_error(error, CROSSDSD_ERROR_OVERFLOW,
                  "AIFF metadata is too large");
        return CROSSDSD_ERROR_OVERFLOW;
    }
    if (!crossdsd::platform::seek_file(reader->file,
                                       reader->info.metadata_offset) ||
        !read_bytes(reader->file, output,
                    static_cast<size_t>(reader->info.metadata_size_bytes))) {
        set_error(error, CROSSDSD_ERROR_IO, "cannot read AIFF metadata");
        return CROSSDSD_ERROR_IO;
    }
    *bytes_read = reader->info.metadata_size_bytes;
    const uint64_t bytes_per_frame = reader->info.channel_count *
        ((reader->info.bits_per_sample + 7u) / 8u);
    if (!crossdsd::platform::seek_file(
            reader->file, reader->info.data_offset +
                reader->frames_read * bytes_per_frame)) {
        set_error(error, CROSSDSD_ERROR_IO,
                  "cannot restore AIFF audio position");
        return CROSSDSD_ERROR_IO;
    }
    set_error(error, CROSSDSD_OK, "");
    return CROSSDSD_OK;
}

}  // extern "C"
