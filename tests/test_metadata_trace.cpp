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

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << __FILE__ << ':' << __LINE__ \
    << ": CHECK failed: " << #x << '\n'; ++failures; } } while (false)

void syncsafe(std::vector<uint8_t>& output, uint32_t value) {
    output.push_back(static_cast<uint8_t>((value >> 21u) & 0x7fu));
    output.push_back(static_cast<uint8_t>((value >> 14u) & 0x7fu));
    output.push_back(static_cast<uint8_t>((value >> 7u) & 0x7fu));
    output.push_back(static_cast<uint8_t>(value & 0x7fu));
}

void frame(
    std::vector<uint8_t>& body, const char* name,
    const std::vector<uint8_t>& payload) {
    body.insert(body.end(), name, name + 4u);
    syncsafe(body, static_cast<uint32_t>(payload.size()));
    body.push_back(0u);
    body.push_back(0u);
    body.insert(body.end(), payload.begin(), payload.end());
}

void test_text_frames() {
    std::vector<uint8_t> body;
    const std::string album = "Beethoven: Symphonie n° 9";
    std::vector<uint8_t> album_payload{3u};
    album_payload.insert(album_payload.end(), album.begin(), album.end());
    frame(body, "TALB", album_payload);

    const std::u16string composer = u"Ludwig van Beethoven";
    std::vector<uint8_t> composer_payload{1u, 0xffu, 0xfeu};
    for (const char16_t value : composer) {
        composer_payload.push_back(static_cast<uint8_t>(value));
        composer_payload.push_back(static_cast<uint8_t>(value >> 8u));
    }
    frame(body, "TCOM", composer_payload);

    std::vector<uint8_t> tag{'I', 'D', '3', 4u, 0u, 0u};
    syncsafe(tag, static_cast<uint32_t>(body.size()));
    tag.insert(tag.end(), body.begin(), body.end());

    CHECK(crossdsd_id3_text_frame(tag, "TALB") == album);
    CHECK(crossdsd_id3_text_frame(tag, "TCOM") == "Ludwig van Beethoven");
    CHECK(crossdsd_id3_text_frame(tag, "TPE1").empty());
}

}  // namespace

int main() {
    test_text_frames();
    if (failures) return 1;
    std::cout << "All CrossDSD metadata trace tests passed\n";
    return 0;
}
