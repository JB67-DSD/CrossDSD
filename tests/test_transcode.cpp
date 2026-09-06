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

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

int failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                      \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: "       \
                      << #condition << '\n';                                     \
            ++failures;                                                         \
        }                                                                       \
    } while (false)

int cancel(void* user_data, double fraction) {
    auto* calls = static_cast<unsigned*>(user_data);
    ++*calls;
    return fraction < 0.01 ? 1 : 0;
}

std::string utf8(const std::filesystem::path& path) {
#if defined(_WIN32)
    const auto encoded = path.u8string();
    return std::string(
        reinterpret_cast<const char*>(encoded.data()), encoded.size());
#else
    return path.string();
#endif
}

void test_cancellation() {
    const auto source =
        std::filesystem::path(CROSSDSD_TEST_SUITE) /
        "AudioGate64/A002_single_impulse_-6dBFS.dsf";
    const auto output =
        std::filesystem::temp_directory_path() /
        ("crossdsd-cancel-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) +
         ".wav");
    unsigned calls = 0;
    crossdsd_decode_file_config config{};
    config.struct_size = sizeof(config);
    config.filter_profile = CROSSDSD_FILTER_STANDARD;
    config.container = CROSSDSD_WAVE_AUTO;
    config.backend = CROSSDSD_BACKEND_CPU;
    config.packed_chunk_bytes_per_channel = 4096;
    config.progress_callback = cancel;
    config.progress_user_data = &calls;
    crossdsd_decode_file_report report{};
    report.struct_size = sizeof(report);
    crossdsd_error error{};
    error.struct_size = sizeof(error);
    const auto source_utf8 = utf8(source);
    const auto output_utf8 = utf8(output);
    CHECK(crossdsd_decode_file(
              source_utf8.c_str(), output_utf8.c_str(), &config, &report,
              &error) ==
          CROSSDSD_ERROR_CANCELLED);
    CHECK(error.code == CROSSDSD_ERROR_CANCELLED);
    CHECK(calls > 0);
    CHECK(!std::filesystem::exists(output));
    std::filesystem::remove(output);
}

}  // namespace

int main() {
    test_cancellation();
    if (failures) {
        std::cerr << failures << " transcode test(s) failed\n";
        return 1;
    }
    std::cout << "All CrossDSD transcode tests passed\n";
    return 0;
}
