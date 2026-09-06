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

#include "metadata_trace.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

namespace {

uint32_t syncsafe(const uint8_t* value) {
    return (static_cast<uint32_t>(value[0] & 0x7fu) << 21u) |
           (static_cast<uint32_t>(value[1] & 0x7fu) << 14u) |
           (static_cast<uint32_t>(value[2] & 0x7fu) << 7u) |
           static_cast<uint32_t>(value[3] & 0x7fu);
}

uint32_t big_endian(const uint8_t* value) {
    return (static_cast<uint32_t>(value[0]) << 24u) |
           (static_cast<uint32_t>(value[1]) << 16u) |
           (static_cast<uint32_t>(value[2]) << 8u) |
           static_cast<uint32_t>(value[3]);
}

void append_syncsafe(std::vector<uint8_t>& output, uint32_t value) {
    output.push_back(static_cast<uint8_t>((value >> 21u) & 0x7fu));
    output.push_back(static_cast<uint8_t>((value >> 14u) & 0x7fu));
    output.push_back(static_cast<uint8_t>((value >> 7u) & 0x7fu));
    output.push_back(static_cast<uint8_t>(value & 0x7fu));
}

void append_big_endian(std::vector<uint8_t>& output, uint32_t value) {
    output.push_back(static_cast<uint8_t>(value >> 24u));
    output.push_back(static_cast<uint8_t>(value >> 16u));
    output.push_back(static_cast<uint8_t>(value >> 8u));
    output.push_back(static_cast<uint8_t>(value));
}

}  // namespace

namespace {

void append_utf8(std::string& output, uint32_t value) {
    if (value <= 0x7fu) {
        output.push_back(static_cast<char>(value));
    } else if (value <= 0x7ffu) {
        output.push_back(static_cast<char>(0xc0u | (value >> 6u)));
        output.push_back(static_cast<char>(0x80u | (value & 0x3fu)));
    } else if (value <= 0xffffu) {
        output.push_back(static_cast<char>(0xe0u | (value >> 12u)));
        output.push_back(static_cast<char>(0x80u | ((value >> 6u) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | (value & 0x3fu)));
    } else if (value <= 0x10ffffu) {
        output.push_back(static_cast<char>(0xf0u | (value >> 18u)));
        output.push_back(static_cast<char>(0x80u | ((value >> 12u) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | ((value >> 6u) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | (value & 0x3fu)));
    }
}

std::string decode_id3_text(std::span<const uint8_t> payload) {
    if (payload.empty()) return {};
    const uint8_t encoding = payload[0];
    payload = payload.subspan(1u);
    std::string output;
    if (encoding == 0u) {
        for (const uint8_t value : payload) {
            if (value == 0u) break;
            append_utf8(output, value);
        }
        return output;
    }
    if (encoding == 3u) {
        const auto end = std::find(payload.begin(), payload.end(), 0u);
        return {reinterpret_cast<const char*>(payload.data()),
                static_cast<size_t>(end - payload.begin())};
    }
    if ((encoding != 1u && encoding != 2u) || payload.size() < 2u) return {};
    bool little_endian = false;
    size_t offset = 0u;
    if (encoding == 1u) {
        if (payload[0] == 0xffu && payload[1] == 0xfeu) {
            little_endian = true;
            offset = 2u;
        } else if (payload[0] == 0xfeu && payload[1] == 0xffu) {
            offset = 2u;
        }
    }
    const auto read_unit = [&](size_t position) {
        return little_endian
            ? static_cast<uint16_t>(payload[position] |
                  (static_cast<uint16_t>(payload[position + 1u]) << 8u))
            : static_cast<uint16_t>(
                  (static_cast<uint16_t>(payload[position]) << 8u) |
                  payload[position + 1u]);
    };
    while (offset + 1u < payload.size()) {
        const uint16_t first = read_unit(offset);
        offset += 2u;
        if (first == 0u) break;
        uint32_t scalar = first;
        if (first >= 0xd800u && first <= 0xdbffu &&
            offset + 1u < payload.size()) {
            const uint16_t second = read_unit(offset);
            if (second >= 0xdc00u && second <= 0xdfffu) {
                offset += 2u;
                scalar = 0x10000u +
                    ((static_cast<uint32_t>(first) - 0xd800u) << 10u) +
                    (static_cast<uint32_t>(second) - 0xdc00u);
            }
        }
        append_utf8(output, scalar);
    }
    return output;
}

}  // namespace

std::string crossdsd_id3_text_frame(
    std::span<const uint8_t> source, std::string_view frame_id) {
    if (frame_id.size() != 4u || source.size() < 10u ||
        std::memcmp(source.data(), "ID3", 3u) != 0 ||
        (source[3] != 3u && source[3] != 4u)) {
        return {};
    }
    const uint8_t version = source[3];
    const uint8_t flags = source[5];
    const uint32_t declared = syncsafe(source.data() + 6u);
    if (declared > source.size() - 10u) return {};
    const auto body = source.subspan(10u, declared);
    size_t offset = 0u;
    if ((flags & 0x40u) != 0u && body.size() >= 4u) {
        const uint32_t extended = version == 4u
            ? syncsafe(body.data()) : big_endian(body.data()) + 4u;
        if (extended > body.size()) return {};
        offset = extended;
    }
    while (offset + 10u <= body.size()) {
        const uint8_t* header = body.data() + offset;
        if (std::all_of(header, header + 4u,
                        [](uint8_t value) { return value == 0u; })) break;
        const uint32_t size = version == 4u
            ? syncsafe(header + 4u) : big_endian(header + 4u);
        if (size > body.size() - offset - 10u) break;
        if (std::equal(frame_id.begin(), frame_id.end(), header)) {
            return decode_id3_text(body.subspan(offset + 10u, size));
        }
        offset += 10u + size;
    }
    return {};
}

std::vector<uint8_t> crossdsd_id3_with_encoder_settings(
    std::span<const uint8_t> source, std::string_view settings,
    std::string_view file_type) {
    uint8_t version = 4u;
    uint8_t revision = 0u;
    uint8_t flags = 0u;
    std::span<const uint8_t> body;
    if (source.size() >= 10u &&
        std::memcmp(source.data(), "ID3", 3u) == 0 &&
        (source[3] == 3u || source[3] == 4u)) {
        const uint32_t declared = syncsafe(source.data() + 6u);
        if (declared <= source.size() - 10u) {
            version = source[3];
            revision = source[4];
            flags = static_cast<uint8_t>(source[5] & 0x7fu); // omit footer
            body = source.subspan(10u, declared);
        }
    }

    std::vector<uint8_t> frames;
    std::string history;
    size_t offset = 0u;
    if ((flags & 0x40u) != 0u && body.size() >= 4u) {
        const uint32_t declared = version == 4u
            ? syncsafe(body.data()) : big_endian(body.data()) + 4u;
        if (declared >= 4u && declared <= body.size()) {
            frames.insert(frames.end(), body.begin(), body.begin() + declared);
            offset = declared;
        } else {
            flags = static_cast<uint8_t>(flags & ~0x40u);
        }
    }
    while (offset + 10u <= body.size()) {
        const auto* header = body.data() + offset;
        if (std::all_of(header, header + 4u,
                        [](uint8_t value) { return value == 0u; })) break;
        const uint32_t size = version == 4u
            ? syncsafe(header + 4u) : big_endian(header + 4u);
        if (size > body.size() - offset - 10u) break;
        bool replace = std::memcmp(header, "TSSE", 4u) == 0 ||
                       std::memcmp(header, "TFLT", 4u) == 0;
        if (!replace && std::memcmp(header, "TXXX", 4u) == 0 && size > 2u) {
            const uint8_t* payload = header + 10u;
            if (payload[0] == 0u || payload[0] == 3u) {
                const uint8_t* description = payload + 1u;
                const uint8_t* end = payload + size;
                const uint8_t* separator = std::find(description, end, 0u);
                constexpr std::string_view name{"Encoding history"};
                if (separator != end &&
                    static_cast<size_t>(separator - description) == name.size() &&
                    std::equal(name.begin(), name.end(), description)) {
                    history.assign(
                        reinterpret_cast<const char*>(separator + 1u),
                        reinterpret_cast<const char*>(end));
                    replace = true;
                }
            }
        }
        if (!replace) {
            frames.insert(frames.end(), header, header + 10u + size);
        }
        offset += 10u + size;
    }

    const uint32_t payload_size = static_cast<uint32_t>(settings.size() + 1u);
    frames.insert(frames.end(), {'T', 'S', 'S', 'E'});
    if (version == 4u) append_syncsafe(frames, payload_size);
    else append_big_endian(frames, payload_size);
    frames.push_back(0u);
    frames.push_back(0u);
    frames.push_back(version == 4u ? 3u : 0u);
    frames.insert(frames.end(), settings.begin(), settings.end());

    const uint32_t file_type_payload_size = static_cast<uint32_t>(
        file_type.size() + 1u);
    frames.insert(frames.end(), {'T', 'F', 'L', 'T'});
    if (version == 4u) append_syncsafe(frames, file_type_payload_size);
    else append_big_endian(frames, file_type_payload_size);
    frames.push_back(0u);
    frames.push_back(0u);
    frames.push_back(version == 4u ? 3u : 0u);
    frames.insert(frames.end(), file_type.begin(), file_type.end());

    if (!history.empty()) history.push_back('\n');
    history.append(settings);
    constexpr std::string_view history_name{"Encoding history"};
    const uint32_t history_payload_size = static_cast<uint32_t>(
        1u + history_name.size() + 1u + history.size());
    frames.insert(frames.end(), {'T', 'X', 'X', 'X'});
    if (version == 4u) append_syncsafe(frames, history_payload_size);
    else append_big_endian(frames, history_payload_size);
    frames.push_back(0u);
    frames.push_back(0u);
    frames.push_back(version == 4u ? 3u : 0u);
    frames.insert(frames.end(), history_name.begin(), history_name.end());
    frames.push_back(0u);
    frames.insert(frames.end(), history.begin(), history.end());

    std::vector<uint8_t> result{'I', 'D', '3', version, revision, flags};
    append_syncsafe(result, static_cast<uint32_t>(frames.size()));
    result.insert(result.end(), frames.begin(), frames.end());
    return result;
}
