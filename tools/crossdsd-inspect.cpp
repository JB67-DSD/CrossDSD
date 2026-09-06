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

#include <cstdio>
#include <cstring>

namespace {
const char* metadata_name(crossdsd_metadata_status value) {
    switch (value) {
        case CROSSDSD_METADATA_NONE: return "none";
        case CROSSDSD_METADATA_ID3: return "id3";
        case CROSSDSD_METADATA_INVALID: return "invalid";
    }
    return "unknown";
}
}

int main(int argc, char** argv) {
    bool json = false;
    const char* path = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--json") == 0) json = true;
        else if (!path) path = argv[i];
        else {
            std::fprintf(stderr, "usage: crossdsd-inspect [--json] FILE.dsf\n");
            return 2;
        }
    }
    if (!path) {
        std::fprintf(stderr, "usage: crossdsd-inspect [--json] FILE.dsf\n");
        return 2;
    }
    crossdsd_reader* reader = nullptr;
    crossdsd_error error{};
    error.struct_size = sizeof(error);
    const auto result = crossdsd_reader_open_file(path, &reader, &error);
    if (result != CROSSDSD_OK) {
        std::fprintf(stderr, "%s:%llu: %s (%s)\n", path,
                     static_cast<unsigned long long>(error.byte_offset),
                     error.message, crossdsd_result_string(result));
        return 1;
    }
    crossdsd_stream_info info{};
    info.struct_size = sizeof(info);
    crossdsd_reader_get_info(reader, &info);
    if (json) {
        std::printf(
            "{\"format\":\"DSF\",\"format_version\":%u,\"channels\":%u,"
            "\"channel_type\":%u,\"sample_rate_hz\":%u,\"dsd_rate\":\"DSD%u\","
            "\"sample_count_per_channel\":%llu,\"duration_seconds\":%.12f,"
            "\"block_size_per_channel\":%u,\"bit_order\":\"LSB-first\","
            "\"audio_offset\":%llu,\"audio_size\":%llu,"
            "\"stored_bits_per_channel\":%llu,\"padding_bits_per_channel\":%llu,"
            "\"metadata_status\":\"%s\",\"metadata_offset\":%llu,\"metadata_size\":%llu}\n",
            info.format_version, info.channel_count, info.channel_type,
            info.sample_rate_hz, static_cast<unsigned>(info.dsd_rate),
            static_cast<unsigned long long>(info.sample_count_per_channel),
            info.duration_seconds, info.block_size_per_channel,
            static_cast<unsigned long long>(info.audio_offset),
            static_cast<unsigned long long>(info.audio_size),
            static_cast<unsigned long long>(info.stored_bits_per_channel),
            static_cast<unsigned long long>(info.padding_bits_per_channel),
            metadata_name(info.metadata_status),
            static_cast<unsigned long long>(info.metadata_offset),
            static_cast<unsigned long long>(info.metadata_size));
    } else {
        std::printf("Format: DSF v%u\nChannels: %u (type %u)\nRate: %u Hz (DSD%u)\n",
                    info.format_version, info.channel_count, info.channel_type,
                    info.sample_rate_hz, static_cast<unsigned>(info.dsd_rate));
        std::printf("Samples/channel: %llu\nDuration: %.9f s\nBit order: LSB-first\n",
                    static_cast<unsigned long long>(info.sample_count_per_channel),
                    info.duration_seconds);
        std::printf("Block/channel: %u bytes\nAudio: offset %llu, size %llu bytes\n",
                    info.block_size_per_channel,
                    static_cast<unsigned long long>(info.audio_offset),
                    static_cast<unsigned long long>(info.audio_size));
        std::printf("Padding/channel: %llu bits\nMetadata: %s, offset %llu, size %llu\n",
                    static_cast<unsigned long long>(info.padding_bits_per_channel),
                    metadata_name(info.metadata_status),
                    static_cast<unsigned long long>(info.metadata_offset),
                    static_cast<unsigned long long>(info.metadata_size));
    }
    crossdsd_reader_close(reader);
    return 0;
}
