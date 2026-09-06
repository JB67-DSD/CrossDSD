# Copyright 2026-present, Jean-Luc EVRARD.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# Author Jean-Luc EVRARD

cmake_minimum_required(VERSION 3.20)

project(CrossDSDFormats VERSION 1.0.0 LANGUAGES C CXX)

if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

option(CROSSDSD_BUILD_SHARED "Build the shared libcrossdsd-formats library" ON)
option(CROSSDSD_BUILD_TESTS "Build and register tests" ON)
option(CROSSDSD_USE_ACCELERATE
    "Use Apple's Accelerate/vDSP for float64 FIR dot products" OFF)
option(CROSSDSD_ENABLE_CHANNEL_THREADS
    "Process independent channels concurrently" OFF)
option(CROSSDSD_BUILD_METAL_TOOLS
    "Build the experimental Metal benchmark on Apple platforms" OFF)
option(CROSSDSD_BUILD_PHASE7_TOOLS
    "Build the experimental phase 7 DSD encoder benchmark" OFF)
option(CROSSDSD_BUILD_PHASE8_TOOLS
    "Build the experimental phase 8 look-ahead encoder benchmark" OFF)
if(APPLE)
    set(CROSSDSD_METAL_DEFAULT ON)
else()
    set(CROSSDSD_METAL_DEFAULT OFF)
endif()
option(CROSSDSD_ENABLE_METAL
    "Enable the production Metal file-decoding backend" ${CROSSDSD_METAL_DEFAULT})

if(CROSSDSD_USE_ACCELERATE AND NOT APPLE)
    message(FATAL_ERROR "CROSSDSD_USE_ACCELERATE is only available on Apple platforms")
endif()
if(CROSSDSD_BUILD_METAL_TOOLS AND NOT APPLE)
    message(FATAL_ERROR "CROSSDSD_BUILD_METAL_TOOLS is only available on Apple platforms")
endif()
if(CROSSDSD_ENABLE_METAL AND NOT APPLE)
    message(FATAL_ERROR "CROSSDSD_ENABLE_METAL is only available on Apple platforms")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CROSSDSD_SOURCES
    src/aiff_reader.cpp
    src/aiff_to_dxd.cpp
    src/analyze_dsd.cpp
    src/dsf_reader.cpp
    src/dsf_writer.cpp
    src/enhanced_modulator.cpp
    src/encode_file.cpp
    src/metadata_trace.cpp
    src/reference_encoder.cpp
    src/reference_decoder.cpp
    src/reference_modulator.cpp
    src/reference_upsampler.cpp
    src/quick_peak_analysis.cpp
    src/stream_decode.cpp
    src/stream_encode.cpp
    src/stream_peak.cpp
    src/stream_transcode.cpp
    src/transcode_dsd.cpp
    src/wave_reader.cpp
    src/wave_writer.cpp
    src/transcode.cpp)
if(CROSSDSD_ENABLE_METAL)
    enable_language(OBJCXX)
    list(APPEND CROSSDSD_SOURCES src/metal_transcode.mm)
endif()

add_library(crossdsd-formats-static STATIC ${CROSSDSD_SOURCES})
add_library(CrossDSD::formats-static ALIAS crossdsd-formats-static)
target_include_directories(crossdsd-formats-static
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_compile_features(crossdsd-formats-static PRIVATE cxx_std_20)
target_compile_definitions(crossdsd-formats-static PUBLIC CROSSDSD_STATIC=1)
target_compile_definitions(crossdsd-formats-static PRIVATE
    "CROSSDSD_VERSION=\"${PROJECT_VERSION}\"")
if(CROSSDSD_ENABLE_METAL)
    target_compile_definitions(
        crossdsd-formats-static PRIVATE CROSSDSD_ENABLE_METAL=1)
    target_compile_options(crossdsd-formats-static PRIVATE -fobjc-arc)
    target_link_libraries(crossdsd-formats-static
        PUBLIC "-framework Foundation" "-framework Metal")
endif()
if(CROSSDSD_USE_ACCELERATE)
    target_compile_definitions(crossdsd-formats-static PRIVATE CROSSDSD_USE_ACCELERATE=1)
    target_link_libraries(crossdsd-formats-static PUBLIC "-framework Accelerate")
endif()
if(CROSSDSD_ENABLE_CHANNEL_THREADS)
    find_package(Threads REQUIRED)
    target_compile_definitions(
        crossdsd-formats-static PRIVATE CROSSDSD_ENABLE_CHANNEL_THREADS=1)
    target_link_libraries(crossdsd-formats-static PUBLIC Threads::Threads)
endif()
set_target_properties(crossdsd-formats-static PROPERTIES
    EXPORT_NAME formats-static
    OUTPUT_NAME crossdsd-formats
    POSITION_INDEPENDENT_CODE ON)
if(WIN32)
    set_target_properties(crossdsd-formats-static PROPERTIES
        OUTPUT_NAME crossdsd-formats-static)
endif()

if(CROSSDSD_BUILD_SHARED)
    add_library(crossdsd-formats SHARED ${CROSSDSD_SOURCES})
    add_library(CrossDSD::formats ALIAS crossdsd-formats)
    target_include_directories(crossdsd-formats
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>)
    target_compile_features(crossdsd-formats PRIVATE cxx_std_20)
    target_compile_definitions(crossdsd-formats PRIVATE CROSSDSD_FORMATS_BUILD
        "CROSSDSD_VERSION=\"${PROJECT_VERSION}\"")
    if(CROSSDSD_ENABLE_METAL)
        target_compile_definitions(
            crossdsd-formats PRIVATE CROSSDSD_ENABLE_METAL=1)
        target_compile_options(crossdsd-formats PRIVATE -fobjc-arc)
        target_link_libraries(crossdsd-formats
            PRIVATE "-framework Foundation" "-framework Metal")
    endif()
    if(CROSSDSD_USE_ACCELERATE)
        target_compile_definitions(crossdsd-formats PRIVATE CROSSDSD_USE_ACCELERATE=1)
        target_link_libraries(crossdsd-formats PRIVATE "-framework Accelerate")
    endif()
    if(CROSSDSD_ENABLE_CHANNEL_THREADS)
        target_compile_definitions(
            crossdsd-formats PRIVATE CROSSDSD_ENABLE_CHANNEL_THREADS=1)
        target_link_libraries(crossdsd-formats PRIVATE Threads::Threads)
    endif()
    set_target_properties(crossdsd-formats PROPERTIES
        EXPORT_NAME formats
        VERSION ${PROJECT_VERSION}
        SOVERSION 0
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
endif()

add_executable(crossdsd-inspect tools/crossdsd-inspect.cpp)
target_link_libraries(crossdsd-inspect PRIVATE CrossDSD::formats-static)

add_executable(crossdsd-decode-check tools/crossdsd-decode-check.cpp)
target_link_libraries(crossdsd-decode-check PRIVATE CrossDSD::formats-static)

add_executable(
    crossdsd-quickpeak-benchmark
    tools/crossdsd-quickpeak-benchmark.cpp)
target_link_libraries(
    crossdsd-quickpeak-benchmark PRIVATE CrossDSD::formats-static)

add_executable(crossdsd tools/crossdsd.cpp)
target_link_libraries(crossdsd PRIVATE CrossDSD::formats-static)
target_include_directories(crossdsd PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(
    crossdsd PRIVATE "CROSSDSD_VERSION=\"${PROJECT_VERSION}\"")


if(CROSSDSD_BUILD_METAL_TOOLS)
    add_executable(crossdsd-metal-benchmark tools/crossdsd-metal-benchmark.mm)
    target_link_libraries(crossdsd-metal-benchmark
        PRIVATE CrossDSD::formats-static "-framework Foundation" "-framework Metal")
    target_compile_options(crossdsd-metal-benchmark PRIVATE -fobjc-arc)
    target_compile_features(crossdsd-metal-benchmark PRIVATE cxx_std_20)
endif()

if(CROSSDSD_BUILD_PHASE7_TOOLS)
    add_executable(
        crossdsd-phase7-modulator-benchmark
        tools/crossdsd-phase7-modulator-benchmark.cpp)
    target_link_libraries(
        crossdsd-phase7-modulator-benchmark
        PRIVATE CrossDSD::formats-static)
endif()

if(CROSSDSD_BUILD_PHASE8_TOOLS)
    find_package(Threads REQUIRED)
    add_executable(
        crossdsd-phase8-lookahead-benchmark
        tools/crossdsd-phase8-lookahead-benchmark.cpp)
    target_link_libraries(
        crossdsd-phase8-lookahead-benchmark
        PRIVATE CrossDSD::formats-static Threads::Threads)
    add_executable(
        crossdsd-phase8-dsd128-lookahead-benchmark
        tools/crossdsd-phase8-lookahead-benchmark.cpp)
    target_compile_definitions(
        crossdsd-phase8-dsd128-lookahead-benchmark
        PRIVATE CROSSDSD_PHASE8_DSD128=1)
    target_link_libraries(
        crossdsd-phase8-dsd128-lookahead-benchmark
        PRIVATE CrossDSD::formats-static Threads::Threads)
    add_executable(
        crossdsd-phase8-dsd256-lookahead-benchmark
        tools/crossdsd-phase8-lookahead-benchmark.cpp)
    target_compile_definitions(
        crossdsd-phase8-dsd256-lookahead-benchmark
        PRIVATE CROSSDSD_PHASE8_DSD256=1)
    target_link_libraries(
        crossdsd-phase8-dsd256-lookahead-benchmark
        PRIVATE CrossDSD::formats-static Threads::Threads)
endif()

if(CROSSDSD_BUILD_TESTS)
    enable_testing()
    add_executable(crossdsd-formats-tests tests/test_dsf_reader.cpp)
    target_link_libraries(crossdsd-formats-tests PRIVATE CrossDSD::formats-static)
    add_test(NAME crossdsd-formats-unit COMMAND crossdsd-formats-tests)

    add_executable(crossdsd-dsf-writer-tests tests/test_dsf_writer.cpp)
    target_link_libraries(
        crossdsd-dsf-writer-tests PRIVATE CrossDSD::formats-static)
    add_test(NAME crossdsd-dsf-writer-unit COMMAND crossdsd-dsf-writer-tests)

    add_executable(crossdsd-dsp-tests tests/test_reference_decoder.cpp)
    target_link_libraries(crossdsd-dsp-tests PRIVATE CrossDSD::formats-static)
    add_test(NAME crossdsd-dsp-unit COMMAND crossdsd-dsp-tests)

    add_executable(
        crossdsd-modulator-tests tests/test_reference_modulator.cpp)
    target_link_libraries(
        crossdsd-modulator-tests PRIVATE CrossDSD::formats-static)
    add_test(NAME crossdsd-modulator-unit COMMAND crossdsd-modulator-tests)

    add_executable(
        crossdsd-upsampler-tests tests/test_reference_upsampler.cpp)
    target_link_libraries(
        crossdsd-upsampler-tests PRIVATE CrossDSD::formats-static)
    add_test(NAME crossdsd-upsampler-unit COMMAND crossdsd-upsampler-tests)

    add_executable(
        crossdsd-encoder-tests tests/test_reference_encoder.cpp)
    target_link_libraries(
        crossdsd-encoder-tests PRIVATE CrossDSD::formats-static)
    add_test(NAME crossdsd-encoder-unit COMMAND crossdsd-encoder-tests)

    add_executable(
        crossdsd-stream-decode-tests tests/test_stream_decode.cpp)
    target_link_libraries(
        crossdsd-stream-decode-tests PRIVATE CrossDSD::formats-static)
    add_test(
        NAME crossdsd-stream-decode-unit COMMAND crossdsd-stream-decode-tests)

    add_executable(
        crossdsd-stream-encode-tests tests/test_stream_encode.cpp)
    target_link_libraries(
        crossdsd-stream-encode-tests PRIVATE CrossDSD::formats-static)
    add_test(
        NAME crossdsd-stream-encode-unit COMMAND crossdsd-stream-encode-tests)

    add_executable(
        crossdsd-stream-transcode-tests tests/test_stream_transcode.cpp)
    target_link_libraries(
        crossdsd-stream-transcode-tests PRIVATE CrossDSD::formats-static)
    add_test(
        NAME crossdsd-stream-transcode-unit
        COMMAND crossdsd-stream-transcode-tests)

    add_executable(
        crossdsd-stream-peak-tests tests/test_stream_peak.cpp)
    target_link_libraries(
        crossdsd-stream-peak-tests PRIVATE CrossDSD::formats-static)
    add_test(
        NAME crossdsd-stream-peak-unit COMMAND crossdsd-stream-peak-tests)

    add_executable(
        crossdsd-encode-file-tests tests/test_encode_file.cpp)
    target_link_libraries(
        crossdsd-encode-file-tests PRIVATE CrossDSD::formats-static)
    add_test(NAME crossdsd-encode-file-unit COMMAND crossdsd-encode-file-tests)

    add_executable(
        crossdsd-metadata-trace-tests tests/test_metadata_trace.cpp)
    target_include_directories(
        crossdsd-metadata-trace-tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(
        crossdsd-metadata-trace-tests PRIVATE CrossDSD::formats-static)
    add_test(
        NAME crossdsd-metadata-trace-unit
        COMMAND crossdsd-metadata-trace-tests)

    add_executable(
        crossdsd-transcode-dsd-tests tests/test_transcode_dsd.cpp)
    target_link_libraries(
        crossdsd-transcode-dsd-tests PRIVATE CrossDSD::formats-static)
    add_test(
        NAME crossdsd-transcode-dsd-unit COMMAND crossdsd-transcode-dsd-tests)

    add_executable(
        crossdsd-analyze-dsd-tests tests/test_analyze_dsd.cpp)
    target_link_libraries(
        crossdsd-analyze-dsd-tests PRIVATE CrossDSD::formats-static)
    add_test(
        NAME crossdsd-analyze-dsd-unit COMMAND crossdsd-analyze-dsd-tests)

    add_executable(crossdsd-wave-tests tests/test_wave_writer.cpp)
    target_link_libraries(crossdsd-wave-tests PRIVATE CrossDSD::formats-static)
    add_test(NAME crossdsd-wave-unit COMMAND crossdsd-wave-tests)

    add_executable(crossdsd-aiff-tests tests/test_aiff_reader.cpp)
    target_link_libraries(crossdsd-aiff-tests PRIVATE CrossDSD::formats-static)
    add_test(NAME crossdsd-aiff-unit COMMAND crossdsd-aiff-tests)

    add_executable(crossdsd-aiff-to-dxd-tests tests/test_aiff_to_dxd.cpp)
    target_link_libraries(
        crossdsd-aiff-to-dxd-tests PRIVATE CrossDSD::formats-static)
    add_test(
        NAME crossdsd-aiff-to-dxd-unit COMMAND crossdsd-aiff-to-dxd-tests)


    set(CROSSDSD_REFERENCE_SIGNALS_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/reference-signals/CrossDSD_Reference_Signals_v0.2"
        CACHE PATH "Path to the complete reference signal corpus")
    if(EXISTS "${CROSSDSD_REFERENCE_SIGNALS_DIR}/AudioGate64/B004_tone_1000Hz_-20dBFS.dsf"
       AND EXISTS "${CROSSDSD_REFERENCE_SIGNALS_DIR}/DXD_352k8_float32/B004_tone_1000Hz_-20dBFS.wav")
        add_executable(crossdsd-transcode-tests tests/test_transcode.cpp)
        target_link_libraries(
            crossdsd-transcode-tests PRIVATE CrossDSD::formats-static)
        target_compile_definitions(crossdsd-transcode-tests PRIVATE
            "CROSSDSD_TEST_SUITE=\"${CROSSDSD_REFERENCE_SIGNALS_DIR}\"")
        add_test(
            NAME crossdsd-transcode-unit
            COMMAND crossdsd-transcode-tests)

        find_package(Python3 COMPONENTS Interpreter QUIET)
        if(Python3_Interpreter_FOUND)
            add_test(
                NAME crossdsd-cli-integration
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli.py"
                    "$<TARGET_FILE:crossdsd>"
                    "${CROSSDSD_REFERENCE_SIGNALS_DIR}")
        endif()
    endif()
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(TARGETS crossdsd-formats-static crossdsd crossdsd-inspect
        crossdsd-decode-check
    EXPORT CrossDSDFormatsTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(CROSSDSD_BUILD_SHARED)
    install(TARGETS crossdsd-formats
        EXPORT CrossDSDFormatsTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT CrossDSDFormatsTargets
    FILE CrossDSDTargets.cmake
    NAMESPACE CrossDSD::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CrossDSD)
configure_package_config_file(
    cmake/CrossDSDConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/CrossDSDConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CrossDSD)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/CrossDSDConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/CrossDSDConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/CrossDSDConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CrossDSD)
install(FILES
    docs/CROSSDSD_COMMAND_LINE_REFERENCE.md
    docs/CROSSDSD_WINDOWS_BUILD.md
    docs/GETTING_STARTED.md
    docs/API_GUIDE.md
    CHANGELOG.md
    LICENSE
    NOTICE.md
    DISCLAIMER.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/CrossDSD)

set(CPACK_PACKAGE_NAME "CrossDSD")
set(CPACK_PACKAGE_VENDOR "CrossDSD")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
if(WIN32)
    set(CPACK_GENERATOR "ZIP")
    set(CPACK_PACKAGE_FILE_NAME
        "CrossDSD-${PROJECT_VERSION}-RC1-${CMAKE_SYSTEM_PROCESSOR}")
else()
    set(CPACK_GENERATOR "TGZ")
endif()
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
include(CPack)
