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

#ifndef CROSSDSD_PLATFORM_FILE_H
#define CROSSDSD_PLATFORM_FILE_H

#include <cstdint>
#include <cstdio>
#include <limits>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>
#include <vector>
#endif

namespace crossdsd::platform {

#if defined(_WIN32)
inline std::wstring utf8_to_wide(const char* value) {
    if (!value) return {};
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, nullptr, 0);
    if (size <= 0) return {};
    std::vector<wchar_t> buffer(static_cast<size_t>(size));
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, buffer.data(), size) <= 0) {
        return {};
    }
    return std::wstring(buffer.data());
}
#endif

inline FILE* open_file(const char* utf8_path, const char* mode) {
#if defined(_WIN32)
    const auto path = utf8_to_wide(utf8_path);
    const auto wide_mode = utf8_to_wide(mode);
    if (path.empty() || wide_mode.empty()) return nullptr;
    return _wfopen(path.c_str(), wide_mode.c_str());
#else
    return std::fopen(utf8_path, mode);
#endif
}

inline bool seek_file(FILE* file, uint64_t offset) {
    if (offset > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return false;
    }
#if defined(_WIN32)
    return _fseeki64(file, static_cast<int64_t>(offset), SEEK_SET) == 0;
#else
    return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

inline bool seek_file_end(FILE* file) {
#if defined(_WIN32)
    return _fseeki64(file, 0, SEEK_END) == 0;
#else
    return fseeko(file, 0, SEEK_END) == 0;
#endif
}

inline int64_t tell_file(FILE* file) {
#if defined(_WIN32)
    return _ftelli64(file);
#else
    return static_cast<int64_t>(ftello(file));
#endif
}

inline bool remove_file(const char* utf8_path) {
#if defined(_WIN32)
    const auto path = utf8_to_wide(utf8_path);
    return !path.empty() && _wremove(path.c_str()) == 0;
#else
    return std::remove(utf8_path) == 0;
#endif
}

}  // namespace crossdsd::platform

#endif
