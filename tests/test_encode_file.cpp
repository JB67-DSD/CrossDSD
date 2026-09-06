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

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <numbers>
#include <string>
#include <vector>

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

std::filesystem::path temporary(const char* suffix) {
    const auto value = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("crossdsd-encode-file-" + std::to_string(value) + suffix);
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

struct ProgressCapture {
    std::vector<double> values;
    double cancel_at = 2.0;
};

int capture_progress(void* user_data, double fraction) {
    auto* capture = static_cast<ProgressCapture*>(user_data);
    capture->values.push_back(fraction);
    return fraction < capture->cancel_at;
}

void write_wave(
    const std::filesystem::path& path, crossdsd_wave_container container,
    uint32_t rate, uint32_t channels, const std::vector<float>& interleaved,
    const std::vector<uint8_t>& metadata = {}) {
    crossdsd_wave_writer_config config{};
    config.struct_size = sizeof(config);
    config.sample_rate_hz = rate;
    config.channel_count = channels;
    config.expected_frame_count = interleaved.size() / channels;
    config.container = container;
    crossdsd_wave_writer* writer = nullptr;
    const auto encoded = utf8(path);
    CHECK(crossdsd_wave_writer_open_file(
              encoded.c_str(), &config, &writer, nullptr) == CROSSDSD_OK);
    CHECK(crossdsd_wave_writer_write(
              writer, interleaved.data(), interleaved.size() / channels,
              nullptr) == CROSSDSD_OK);
    if (!metadata.empty()) {
        CHECK(crossdsd_wave_writer_write_id3(
                  writer, metadata.data(), metadata.size(), nullptr) ==
              CROSSDSD_OK);
    }
    CHECK(crossdsd_wave_writer_close(writer, nullptr) == CROSSDSD_OK);
}

std::vector<std::vector<uint8_t>> direct_encode(
    const std::vector<float>& interleaved, uint32_t channels) {
    const uint64_t frames = interleaved.size() / channels;
    std::vector<float> planar(interleaved.size());
    for (uint64_t frame = 0; frame < frames; ++frame) {
        for (uint32_t channel = 0; channel < channels; ++channel) {
            planar[static_cast<uint64_t>(channel) * frames + frame] =
                interleaved[frame * channels + channel];
        }
    }
    crossdsd_encoder_config config{};
    config.struct_size = sizeof(config);
    config.input_sample_rate_hz = 352'800;
    config.output_sample_rate_hz = 2'822'400;
    config.channel_count = channels;
    config.modulator_profile = CROSSDSD_MODULATOR_CRFB_ORDER7_HINF_1_32;
    crossdsd_encoder* encoder = nullptr;
    CHECK(crossdsd_encoder_create(&config, &encoder, nullptr) == CROSSDSD_OK);
    std::vector<uint8_t> first(
        static_cast<size_t>((frames + 1u) * channels));
    uint64_t first_bytes = 0;
    CHECK(crossdsd_encoder_process(
              encoder, planar.data(), frames, frames, first.data(), frames + 1u,
              frames + 1u, &first_bytes, nullptr) == CROSSDSD_OK);
    crossdsd_encoder_info info{};
    info.struct_size = sizeof(info);
    CHECK(crossdsd_encoder_get_info(encoder, &info) == CROSSDSD_OK);
    const uint64_t remaining =
        info.input_samples_consumed_per_channel -
        info.packed_bytes_produced_per_channel;
    std::vector<uint8_t> final(
        static_cast<size_t>(remaining * channels));
    uint64_t final_bytes = 0;
    CHECK(crossdsd_encoder_finish(
              encoder, final.data(), remaining, remaining, &final_bytes,
              nullptr) == CROSSDSD_OK);
    std::vector<std::vector<uint8_t>> result(channels);
    for (uint32_t channel = 0; channel < channels; ++channel) {
        result[channel].insert(
            result[channel].end(),
            first.data() + static_cast<uint64_t>(channel) * (frames + 1u),
            first.data() + static_cast<uint64_t>(channel) * (frames + 1u) +
                first_bytes);
        result[channel].insert(
            result[channel].end(),
            final.data() + static_cast<uint64_t>(channel) * remaining,
            final.data() + static_cast<uint64_t>(channel) * remaining +
                final_bytes);
    }
    crossdsd_encoder_destroy(encoder);
    return result;
}

void test_container(
    crossdsd_wave_container container, uint32_t channels, bool with_metadata) {
    constexpr uint64_t frames = 5003;
    std::vector<float> input(frames * channels);
    for (uint64_t frame = 0; frame < frames; ++frame) {
        for (uint32_t channel = 0; channel < channels; ++channel) {
            input[frame * channels + channel] = static_cast<float>(
                (0.6 - 0.15 * channel) *
                std::sin(
                    2.0 * std::numbers::pi * (1000.0 + 2000.0 * channel) *
                    static_cast<double>(frame) / 352'800.0));
        }
    }
    const std::vector<uint8_t> metadata =
        with_metadata
            ? std::vector<uint8_t>{
                  'I', 'D', '3', 4, 0, 0, 0, 0, 0, 0}
            : std::vector<uint8_t>{};
    const auto wave = temporary(".wav");
    const auto dsf = temporary(".dsf");
    write_wave(wave, container, 352'800, channels, input, metadata);

    crossdsd_encode_file_config config{};
    config.struct_size = sizeof(config);
    config.dsd_rate = CROSSDSD_DSD64;
    config.frame_chunk_count = 257;
    crossdsd_encode_file_report report{};
    report.struct_size = sizeof(report);
    crossdsd_error error{};
    error.struct_size = sizeof(error);
    const auto wave_utf8 = utf8(wave);
    const auto dsf_utf8 = utf8(dsf);
    CHECK(crossdsd_encode_file(
              wave_utf8.c_str(), dsf_utf8.c_str(), &config, &report,
              &error) == CROSSDSD_OK);
    CHECK(report.input_container == container);
    CHECK(report.input_frames == frames);
    CHECK(report.output_dsd_samples_per_channel == frames * 8u);
    CHECK(report.output_packed_bytes_per_channel == frames);
    CHECK(report.channel_count == channels);
    CHECK(report.dsd_rate == CROSSDSD_DSD64);
    CHECK(report.channel_input_peaks[0] > 0.59);
    CHECK(report.channel_maximum_states[0] > 0.0);
    CHECK(report.channel_maximum_quantizer_inputs[0] > 0.0);
    CHECK(report.channel_one_densities[0] > 0.49);
    CHECK(report.channel_one_densities[0] < 0.51);
    CHECK(report.metadata_status == CROSSDSD_METADATA_ID3);

    crossdsd_reader* reader = nullptr;
    CHECK(crossdsd_reader_open_file(
              dsf_utf8.c_str(), &reader, &error) == CROSSDSD_OK);
    crossdsd_stream_info stream{};
    stream.struct_size = sizeof(stream);
    CHECK(crossdsd_reader_get_info(reader, &stream) == CROSSDSD_OK);
    CHECK(stream.sample_rate_hz == 2'822'400);
    CHECK(stream.sample_count_per_channel == frames * 8u);
    CHECK(stream.channel_count == channels);
    std::vector<uint8_t> packed(frames * channels);
    uint64_t bytes_read = 0;
    uint8_t valid_bits = 0;
    CHECK(crossdsd_reader_read_packed(
              reader, packed.data(), frames, &bytes_read, &valid_bits,
              &error) == CROSSDSD_OK);
    CHECK(bytes_read == frames);
    CHECK(valid_bits == 8);
    const auto expected = direct_encode(input, channels);
    for (uint32_t channel = 0; channel < channels; ++channel) {
        CHECK(std::equal(
            expected[channel].begin(), expected[channel].end(),
            packed.begin() + static_cast<ptrdiff_t>(channel * frames)));
    }
    CHECK(stream.metadata_status == CROSSDSD_METADATA_ID3);
    std::vector<uint8_t> read_metadata(stream.metadata_size);
    uint64_t metadata_read = 0;
    CHECK(crossdsd_reader_read_metadata(
              reader, read_metadata.data(), read_metadata.size(),
              &metadata_read, &error) == CROSSDSD_OK);
    CHECK(metadata_read == stream.metadata_size);
    const std::string frame{"TSSE"};
    CHECK(std::search(read_metadata.begin(), read_metadata.end(),
                      frame.begin(), frame.end()) !=
          read_metadata.end());
    const std::string settings{"CrossDSD "};
    CHECK(std::search(read_metadata.begin(), read_metadata.end(),
                      settings.begin(), settings.end()) != read_metadata.end());
    const std::string history{"Encoding history"};
    CHECK(std::search(read_metadata.begin(), read_metadata.end(),
                      history.begin(), history.end()) != read_metadata.end());
    const std::string file_type_frame{"TFLT"};
    CHECK(std::search(read_metadata.begin(), read_metadata.end(),
                      file_type_frame.begin(), file_type_frame.end()) !=
          read_metadata.end());
    const std::string file_type{"DSD64"};
    CHECK(std::search(read_metadata.begin(), read_metadata.end(),
                      file_type.begin(), file_type.end()) !=
          read_metadata.end());
    crossdsd_reader_close(reader);
    std::filesystem::remove(wave);
    std::filesystem::remove(dsf);
}

void test_wrong_rate_is_rejected() {
    const auto wave = temporary("-wrong-rate.wav");
    const auto dsf = temporary("-wrong-rate.dsf");
    write_wave(wave, CROSSDSD_WAVE_RIFF, 44'100, 1, {0.0f, 0.0f});
    crossdsd_encode_file_config config{};
    config.struct_size = sizeof(config);
    config.dsd_rate = CROSSDSD_DSD64;
    crossdsd_encode_file_report report{};
    report.struct_size = sizeof(report);
    const auto wave_utf8 = utf8(wave);
    const auto dsf_utf8 = utf8(dsf);
    CHECK(crossdsd_encode_file(
              wave_utf8.c_str(), dsf_utf8.c_str(), &config, &report,
              nullptr) == CROSSDSD_ERROR_UNSUPPORTED_RATE);
    CHECK(!std::filesystem::exists(dsf));
    std::filesystem::remove(wave);
}

void test_dsd64_enhanced_file() {
    constexpr uint64_t frames = 12'288;
    std::vector<float> input(frames);
    for (uint64_t frame = 0; frame < frames; ++frame) {
        const double amplitude =
            frame >= frames / 3u && frame < frames * 2u / 3u ? 0.01 : 1.40;
        input[frame] = static_cast<float>(
            amplitude * std::sin(
                            2.0 * std::numbers::pi * 997.0 *
                            static_cast<double>(frame) / 352'800.0));
    }
    const auto wave = temporary("-enhanced.wav");
    const auto dsf = temporary("-enhanced.dsf");
    write_wave(wave, CROSSDSD_WAVE_RIFF, 352'800, 1, input);
    crossdsd_encode_file_config config{};
    config.struct_size = sizeof(config);
    config.dsd_rate = CROSSDSD_DSD64;
    config.encoder_profile = CROSSDSD_ENCODER_DSD64_ENHANCED;
    config.frame_chunk_count = 257;
    crossdsd_encode_file_report report{};
    report.struct_size = sizeof(report);
    const auto wave_utf8 = utf8(wave);
    const auto dsf_utf8 = utf8(dsf);
    CHECK(crossdsd_encode_file(
              wave_utf8.c_str(), dsf_utf8.c_str(), &config, &report,
              nullptr) == CROSSDSD_OK);
    CHECK(report.encoder_profile == CROSSDSD_ENCODER_DSD64_ENHANCED);
    CHECK(report.output_dsd_samples_per_channel == frames * 8u);
    CHECK(report.enhanced_dsd_samples_per_channel > 0);
    CHECK(report.enhanced_dsd_samples_per_channel <
          report.output_dsd_samples_per_channel);
    CHECK(report.enhanced_segment_count >= 1);

    config.dsd_rate = CROSSDSD_DSD128;
    CHECK(crossdsd_encode_file(
              wave_utf8.c_str(), dsf_utf8.c_str(), &config, &report,
              nullptr) == CROSSDSD_ERROR_UNSUPPORTED_RATE);
    std::filesystem::remove(wave);
    std::filesystem::remove(dsf);
}

void test_encode_progress_and_cancellation() {
    constexpr uint64_t frames = 10'003;
    std::vector<float> input(frames);
    for (uint64_t frame = 0; frame < frames; ++frame) {
        input[frame] = static_cast<float>(
            0.25 * std::sin(
                       2.0 * std::numbers::pi * 1000.0 *
                       static_cast<double>(frame) / 352'800.0));
    }
    const auto wave = temporary("-progress.wav");
    const auto complete = temporary("-progress-complete.dsf");
    const auto cancelled = temporary("-progress-cancelled.dsf");
    write_wave(wave, CROSSDSD_WAVE_RIFF, 352'800, 1, input);

    ProgressCapture progress;
    crossdsd_encode_file_config config{};
    config.struct_size = sizeof(config);
    config.dsd_rate = CROSSDSD_DSD64;
    config.frame_chunk_count = 257;
    config.progress_callback = capture_progress;
    config.progress_user_data = &progress;
    crossdsd_encode_file_report report{};
    report.struct_size = sizeof(report);
    const auto wave_utf8 = utf8(wave);
    const auto complete_utf8 = utf8(complete);
    CHECK(crossdsd_encode_file(
              wave_utf8.c_str(), complete_utf8.c_str(), &config, &report,
              nullptr) == CROSSDSD_OK);
    CHECK(progress.values.size() > 3u);
    CHECK(progress.values.front() == 0.0);
    CHECK(progress.values.back() == 1.0);
    CHECK(std::is_sorted(progress.values.begin(), progress.values.end()));
    CHECK(std::count(progress.values.begin(), progress.values.end(), 1.0) == 1);
    CHECK(*std::max_element(
              progress.values.begin(), progress.values.end() - 1) <= 0.99);
    CHECK(std::filesystem::exists(complete));

    ProgressCapture cancellation;
    cancellation.cancel_at = 0.25;
    config.progress_user_data = &cancellation;
    const auto cancelled_utf8 = utf8(cancelled);
    crossdsd_error error{};
    error.struct_size = sizeof(error);
    CHECK(crossdsd_encode_file(
              wave_utf8.c_str(), cancelled_utf8.c_str(), &config, &report,
              &error) == CROSSDSD_ERROR_CANCELLED);
    CHECK(!cancellation.values.empty());
    CHECK(cancellation.values.front() == 0.0);
    CHECK(cancellation.values.back() >= cancellation.cancel_at);
    CHECK(cancellation.values.back() < 1.0);
    CHECK(!std::filesystem::exists(cancelled));

    std::filesystem::remove(wave);
    std::filesystem::remove(complete);
}

void test_high_rate_container(
    crossdsd_dsd_rate dsd_rate, uint32_t input_rate = 352'800u) {
    constexpr uint64_t frames = 5003;
    std::vector<float> input(frames);
    for (uint64_t frame = 0; frame < frames; ++frame) {
        input[frame] = static_cast<float>(
            0.8 * std::sin(2.0 * std::numbers::pi * 1000.0 *
                           static_cast<double>(frame) / input_rate));
    }
    const auto wave = temporary("-high-rate.wav");
    const auto dsf = temporary("-high-rate.dsf");
    write_wave(wave, CROSSDSD_WAVE_RIFF, input_rate, 1, input, {});
    crossdsd_encode_file_config config{};
    config.struct_size = sizeof(config);
    config.dsd_rate = dsd_rate;
    config.frame_chunk_count = 257;
    crossdsd_encode_file_report report{};
    report.struct_size = sizeof(report);
    const auto wave_utf8 = utf8(wave);
    const auto dsf_utf8 = utf8(dsf);
    CHECK(crossdsd_encode_file(
              wave_utf8.c_str(), dsf_utf8.c_str(), &config, &report,
              nullptr) == CROSSDSD_OK);
    const uint32_t output_rate =
        dsd_rate == CROSSDSD_DSD512 ? 22'579'200u
        : dsd_rate == CROSSDSD_DSD256 ? 11'289'600u : 5'644'800u;
    const uint64_t factor = output_rate / input_rate;
    CHECK(report.output_sample_rate_hz == output_rate);
    CHECK(report.dsd_rate == dsd_rate);
    CHECK(report.output_dsd_samples_per_channel == frames * factor);
    CHECK(report.output_packed_bytes_per_channel == frames * factor / 8u);

    crossdsd_reader* reader = nullptr;
    CHECK(crossdsd_reader_open_file(
              dsf_utf8.c_str(), &reader, nullptr) == CROSSDSD_OK);
    crossdsd_stream_info stream{};
    stream.struct_size = sizeof(stream);
    CHECK(crossdsd_reader_get_info(reader, &stream) == CROSSDSD_OK);
    CHECK(stream.sample_rate_hz == output_rate);
    CHECK(stream.sample_count_per_channel == frames * factor);
    crossdsd_reader_close(reader);
    std::filesystem::remove(wave);
    std::filesystem::remove(dsf);
}

void test_multichannel_type_inference(uint32_t channels, uint32_t expected_type) {
    constexpr uint64_t frames = 257;
    std::vector<float> input(frames * channels);
    for (uint64_t frame = 0; frame < frames; ++frame) {
        for (uint32_t channel = 0; channel < channels; ++channel) {
            input[frame * channels + channel] = static_cast<float>(
                0.1 * std::sin(
                          2.0 * std::numbers::pi *
                          (997.0 + 100.0 * channel) *
                          static_cast<double>(frame) / 352'800.0));
        }
    }
    const auto wave = temporary("-multichannel.wav");
    const auto dsf = temporary("-multichannel.dsf");
    write_wave(wave, CROSSDSD_WAVE_RIFF, 352'800, channels, input);

    crossdsd_encode_file_config config{};
    config.struct_size = sizeof(config);
    config.dsd_rate = CROSSDSD_DSD64;
    config.frame_chunk_count = 127;
    crossdsd_encode_file_report report{};
    report.struct_size = sizeof(report);
    crossdsd_error error{};
    error.struct_size = sizeof(error);
    const auto wave_utf8 = utf8(wave);
    const auto dsf_utf8 = utf8(dsf);
    CHECK(crossdsd_encode_file(
              wave_utf8.c_str(), dsf_utf8.c_str(), &config, &report,
              &error) == CROSSDSD_OK);
    CHECK(report.channel_count == channels);
    CHECK(report.channel_type == expected_type);

    crossdsd_reader* reader = nullptr;
    CHECK(crossdsd_reader_open_file(
              dsf_utf8.c_str(), &reader, &error) == CROSSDSD_OK);
    crossdsd_stream_info stream{};
    stream.struct_size = sizeof(stream);
    CHECK(crossdsd_reader_get_info(reader, &stream) == CROSSDSD_OK);
    CHECK(stream.channel_count == channels);
    CHECK(stream.channel_type == expected_type);
    crossdsd_reader_close(reader);
    std::filesystem::remove(wave);
    std::filesystem::remove(dsf);
}

void test_ambiguous_four_channel_requires_explicit_type() {
    const auto wave = temporary("-four-channel.wav");
    const auto dsf = temporary("-four-channel.dsf");
    write_wave(
        wave, CROSSDSD_WAVE_RIFF, 352'800, 4,
        std::vector<float>(4u * 32u, 0.0f));
    crossdsd_encode_file_config config{};
    config.struct_size = sizeof(config);
    config.dsd_rate = CROSSDSD_DSD64;
    crossdsd_encode_file_report report{};
    report.struct_size = sizeof(report);
    crossdsd_error error{};
    error.struct_size = sizeof(error);
    const auto wave_utf8 = utf8(wave);
    const auto dsf_utf8 = utf8(dsf);
    CHECK(crossdsd_encode_file(
              wave_utf8.c_str(), dsf_utf8.c_str(), &config, &report,
              &error) == CROSSDSD_ERROR_INVALID_ARGUMENT);
    CHECK(!std::filesystem::exists(dsf));
    std::filesystem::remove(wave);
}

}  // namespace

int main() {
    test_container(CROSSDSD_WAVE_RIFF, 2, true);
    test_container(CROSSDSD_WAVE_RF64, 1, false);
    test_wrong_rate_is_rejected();
    test_dsd64_enhanced_file();
    test_encode_progress_and_cancellation();
    test_high_rate_container(CROSSDSD_DSD128);
    test_high_rate_container(CROSSDSD_DSD256);
    test_high_rate_container(CROSSDSD_DSD512);
    test_high_rate_container(CROSSDSD_DSD512, 705'600u);
    test_multichannel_type_inference(3, 3);
    test_multichannel_type_inference(5, 6);
    test_multichannel_type_inference(6, 7);
    test_ambiguous_four_channel_requires_explicit_type();
    if (failures) {
        std::cerr << failures << " encode-file test(s) failed\n";
        return 1;
    }
    std::cout << "All CrossDSD file encoder tests passed\n";
    return 0;
}
