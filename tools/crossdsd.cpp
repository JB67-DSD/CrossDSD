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
#include "platform_file.h"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

volatile std::sig_atomic_t cancelled = 0;

void interrupt_handler(int) { cancelled = 1; }

struct ProgressState {
    unsigned next_percent = 10;
};

int progress(void* user_data, double completed_fraction) {
    auto* state = static_cast<ProgressState*>(user_data);
    const unsigned percent =
        static_cast<unsigned>(completed_fraction * 100.0);
    if (percent >= state->next_percent && percent < 100u) {
        std::fprintf(stderr, "\rDecoding: %u%%", std::min(percent, 100u));
        std::fflush(stderr);
        while (state->next_percent <= percent) state->next_percent += 10;
    }
    return cancelled == 0;
}

int encode_progress(void* user_data, double completed_fraction) {
    auto* state = static_cast<ProgressState*>(user_data);
    const unsigned percent =
        static_cast<unsigned>(completed_fraction * 100.0);
    if (percent >= state->next_percent && percent < 100u) {
        std::fprintf(stderr, "\rEncoding: %u%%", std::min(percent, 100u));
        std::fflush(stderr);
        while (state->next_percent <= percent) state->next_percent += 10;
    }
    return cancelled == 0;
}

int upscale_progress(void* user_data, double completed_fraction) {
    auto* state = static_cast<ProgressState*>(user_data);
    const unsigned percent =
        static_cast<unsigned>(completed_fraction * 100.0);
    if (percent >= state->next_percent && percent < 100u) {
        std::fprintf(stderr, "\rUpsampling: %u%%", std::min(percent, 100u));
        std::fflush(stderr);
        while (state->next_percent <= percent) state->next_percent += 10;
    }
    return cancelled == 0;
}

int transcode_progress(void* user_data, double completed_fraction) {
    auto* state = static_cast<ProgressState*>(user_data);
    const unsigned percent =
        static_cast<unsigned>(completed_fraction * 100.0);
    if (percent >= state->next_percent && percent < 100u) {
        std::fprintf(
            stderr, "\rTranscoding: %u%%", std::min(percent, 100u));
        std::fflush(stderr);
        while (state->next_percent <= percent) state->next_percent += 10;
    }
    return cancelled == 0;
}

int analyze_progress(void* user_data, double completed_fraction) {
    auto* state = static_cast<ProgressState*>(user_data);
    const unsigned percent =
        static_cast<unsigned>(completed_fraction * 100.0);
    if (percent >= state->next_percent && percent < 100u) {
        std::fprintf(stderr, "\rAnalyzing: %u%%", std::min(percent, 100u));
        std::fflush(stderr);
        while (state->next_percent <= percent) state->next_percent += 10;
    }
    return cancelled == 0;
}

std::filesystem::path path_from_utf8(const char* value) {
#if defined(_WIN32)
    return std::filesystem::u8path(value);
#else
    return std::filesystem::path(value);
#endif
}

std::string path_to_utf8(const std::filesystem::path& value) {
#if defined(_WIN32)
    const auto encoded = value.u8string();
    return std::string(
        reinterpret_cast<const char*>(encoded.data()), encoded.size());
#else
    return value.string();
#endif
}

const char* profile_name(crossdsd_filter_profile profile) {
    return profile == CROSSDSD_FILTER_RAW ? "raw" : "standard";
}

const char* container_name(crossdsd_wave_container container) {
    return container == CROSSDSD_WAVE_RF64 ? "rf64" : "wav";
}

const char* backend_name(crossdsd_backend backend) {
    return backend == CROSSDSD_BACKEND_METAL ? "metal" : "cpu";
}

const char* encoder_profile_name(crossdsd_encoder_profile profile) {
    return profile == CROSSDSD_ENCODER_DSD64_ENHANCED ? "enhanced"
                                                       : "standard";
}

void usage() {
    std::fprintf(
        stderr,
        "usage:\n"
        "  crossdsd decode INPUT.dsf OUTPUT.wav [options]\n"
        "  crossdsd encode INPUT.wav OUTPUT.dsf --dsd-rate=64|128|256|512 [options]\n"
        "  crossdsd upscale INPUT.aiff OUTPUT.wav [options]\n"
        "  crossdsd transcode INPUT.dsf OUTPUT.dsf --dsd-rate=64|128|256|512 [options]\n"
        "  crossdsd analyze INPUT1.dsf [INPUT2.dsf ...] [options]\n\n"
        "decode options:\n"
        "  --profile=standard|raw   filter profile (default: standard)\n"
        "  --container=auto|wav|rf64 output container (default: auto)\n"
        "  --backend=auto|cpu|metal processing backend (default: auto)\n"
        "  --report=FILE|-           write the JSON report\n"
        "  --overwrite               replace an existing output\n"
        "\nupscale options:\n"
        "  --container=auto|wav|rf64 output container (default: auto)\n"
        "                            AIFF 44.1 kHz to float32 DXD 352.8 kHz\n"
        "  --overwrite               replace an existing output\n"
        "\nencode options:\n"
        "  --dsd-rate=64|128|256|512 required output rate\n"
        "  --enhanced                segmented Enhanced mode for DSD64;\n"
        "                            silently ignored for DSD128/DSD256/DSD512\n"
        "  --channel-type=N         required for more than two channels\n"
        "  --report=FILE|-           write the JSON report\n"
        "  --overwrite               replace an existing output\n"
        "\ntranscode options:\n"
        "  --dsd-rate=64|128|256|512 required output rate\n"
        "  --enhanced                segmented Enhanced mode for DSD64;\n"
        "                            silently ignored for DSD128/DSD256/DSD512\n"
        "  --maxpeak=DBFS           fixed attenuation when reconstructed peak\n"
        "  --quicksearch            adaptive peak search (default with --maxpeak)\n"
        "  --fullsearch             exhaustive historical peak search\n"
        "                            exceeds this level (finite and <= 0)\n"
        "  --channel-type=N         override the source DSF channel type\n"
        "  --report=FILE|-           write the JSON report\n"
        "  --overwrite               replace an existing output\n"
        "\nanalyze options:\n"
        "  --quicksearch            adaptive peak search (default)\n"
        "  --fullsearch             exhaustive reconstructed peak search\n"
        "  --album-target=DBFS      gain recommendation target (default: 0)\n"
        "  --report=FILE|-           write the JSON report\n");
}

bool write_analyze_report(
    FILE* stream, const char* input,
    const crossdsd_analyze_file_report& report,
    double target_peak_dbfs, double recommended_gain_db) {
    const auto json_string = [](FILE* target, const char* value) {
        if (std::fputc('"', target) == EOF) return false;
        for (const unsigned char* cursor =
                 reinterpret_cast<const unsigned char*>(value);
             *cursor; ++cursor) {
            if (*cursor == '"' || *cursor == '\\') {
                if (std::fputc('\\', target) == EOF) return false;
            }
            if (*cursor < 0x20u) {
                if (std::fprintf(target, "\\u%04x", *cursor) < 0) return false;
            } else if (std::fputc(*cursor, target) == EOF) {
                return false;
            }
        }
        return std::fputc('"', target) != EOF;
    };
    if (std::fputs("{\n  \"operation\":\"analyze\",\n  \"input\":", stream) == EOF ||
        !json_string(stream, input) ||
        std::fprintf(
            stream,
            ",\n"
            "  \"input_dsd_rate\":%u,\n"
            "  \"input_rate_hz\":%u,\n"
            "  \"reconstructed_rate_hz\":%u,\n"
            "  \"channels\":%u,\n"
            "  \"channel_type\":%u,\n"
            "  \"input_dsd_samples_per_channel\":%llu,\n"
            "  \"reconstructed_frames\":%llu,\n"
            "  \"duration_seconds\":%.9f,\n"
            "  \"search\":\"%s\",\n"
            "  \"target_peak_dbfs\":%.12g,\n"
            "  \"recommended_gain_db\":%.12g,\n"
            "  \"analysis_seconds\":%.9f,\n"
            "  \"realtime_factor\":%.9f,\n"
            "  \"quick_scan_seconds\":%.9f,\n"
            "  \"analyzed_input_fraction\":%.9f,\n"
            "  \"worker_count\":%u,\n"
            "  \"reconstructed_peak\":%.12g,\n"
            "  \"reconstructed_peak_dbfs\":",
            static_cast<unsigned>(report.input_dsd_rate),
            report.input_sample_rate_hz,
            report.reconstructed_sample_rate_hz,
            report.channel_count, report.channel_type,
            static_cast<unsigned long long>(
                report.input_dsd_samples_per_channel),
            static_cast<unsigned long long>(report.reconstructed_frames),
            report.duration_seconds,
            report.full_peak_search ? "full" : "quick",
            target_peak_dbfs, recommended_gain_db,
            report.analysis_seconds, report.realtime_factor,
            report.quick_scan_seconds, report.analyzed_input_fraction,
            report.worker_count, report.reconstructed_peak) < 0) {
        return false;
    }
    if (std::isfinite(report.reconstructed_peak_dbfs)) {
        if (std::fprintf(stream, "%.12g", report.reconstructed_peak_dbfs) < 0)
            return false;
    } else if (std::fputs("null", stream) == EOF) {
        return false;
    }
    if (std::fprintf(
            stream,
            ",\n  \"peak_channel\":%u,\n"
            "  \"peak_frame\":%llu,\n"
            "  \"peak_seconds\":%.9f,\n"
            "  \"channel_peaks\":[",
            report.peak_channel + 1u,
            static_cast<unsigned long long>(report.peak_frame),
            report.peak_seconds) < 0) {
        return false;
    }
    for (uint32_t channel = 0; channel < report.channel_count; ++channel) {
        if (channel && std::fputc(',', stream) == EOF) return false;
        if (std::fprintf(
                stream,
                "{\"channel\":%u,\"peak\":%.12g,\"peak_dbfs\":",
                channel + 1u,
                report.channel_reconstructed_peaks[channel]) < 0) {
            return false;
        }
        if (std::isfinite(report.channel_reconstructed_peak_dbfs[channel])) {
            if (std::fprintf(
                    stream, "%.12g",
                    report.channel_reconstructed_peak_dbfs[channel]) < 0) {
                return false;
            }
        } else if (std::fputs("null", stream) == EOF) {
            return false;
        }
        if (std::fprintf(
                stream, ",\"frame\":%llu,\"seconds\":%.9f}",
                static_cast<unsigned long long>(
                    report.channel_peak_frames[channel]),
                report.channel_peak_seconds[channel]) < 0) {
            return false;
        }
    }
    return std::fputs("]\n}\n", stream) != EOF;
}

bool write_analyze_batch_report(
    FILE* stream, const std::vector<const char*>& inputs,
    const std::vector<crossdsd_analyze_file_report>& files,
    const std::vector<double>& individual_gains,
    const crossdsd_analyze_batch_report& report) {
    const auto json_string = [](FILE* target, const char* value) {
        if (std::fputc('"', target) == EOF) return false;
        for (const unsigned char* cursor =
                 reinterpret_cast<const unsigned char*>(value);
             *cursor; ++cursor) {
            if (*cursor == '"' || *cursor == '\\') {
                if (std::fputc('\\', target) == EOF) return false;
            }
            if (*cursor < 0x20u) {
                if (std::fprintf(target, "\\u%04x", *cursor) < 0) return false;
            } else if (std::fputc(*cursor, target) == EOF) {
                return false;
            }
        }
        return std::fputc('"', target) != EOF;
    };
    const auto json_number = [](FILE* target, double value) {
        return std::isfinite(value)
            ? std::fprintf(target, "%.12g", value) >= 0
            : std::fputs("null", target) != EOF;
    };
    if (std::fprintf(
            stream,
            "{\n"
            "  \"operation\":\"analyze-batch\",\n"
            "  \"file_count\":%u,\n"
            "  \"search\":\"%s\",\n"
            "  \"target_peak_dbfs\":%.12g,\n"
            "  \"common_gain_db\":%.12g,\n"
            "  \"maximum_file_index\":%u,\n"
            "  \"maximum_reconstructed_peak\":%.12g,\n"
            "  \"maximum_reconstructed_peak_dbfs\":",
            report.input_file_count,
            report.full_peak_search ? "full" : "quick",
            report.target_peak_dbfs, report.common_gain_db,
            report.maximum_file_index,
            report.maximum_reconstructed_peak) < 0 ||
        !json_number(stream, report.maximum_reconstructed_peak_dbfs) ||
        std::fprintf(
            stream,
            ",\n"
            "  \"total_duration_seconds\":%.9f,\n"
            "  \"analysis_seconds\":%.9f,\n"
            "  \"realtime_factor\":%.9f,\n"
            "  \"files\":[\n",
            report.total_duration_seconds, report.analysis_seconds,
            report.realtime_factor) < 0) {
        return false;
    }
    for (size_t index = 0; index < files.size(); ++index) {
        const auto& file = files[index];
        if (index && std::fputs(",\n", stream) == EOF) return false;
        if (std::fputs("    {\"index\":", stream) == EOF ||
            std::fprintf(stream, "%zu,\"input\":", index) < 0 ||
            !json_string(stream, inputs[index]) ||
            std::fprintf(
                stream,
                ",\"dsd_rate\":%u,\"channels\":%u,"
                "\"duration_seconds\":%.9f,"
                "\"reconstructed_peak\":%.12g,"
                "\"reconstructed_peak_dbfs\":",
                static_cast<unsigned>(file.input_dsd_rate),
                file.channel_count, file.duration_seconds,
                file.reconstructed_peak) < 0 ||
            !json_number(stream, file.reconstructed_peak_dbfs) ||
            std::fprintf(
                stream,
                ","
                "\"peak_channel\":%u,\"peak_seconds\":%.9f,"
                "\"individual_gain_db\":%.12g,\"channel_peaks\":[",
                file.peak_channel + 1u, file.peak_seconds,
                individual_gains[index]) < 0) {
            return false;
        }
        for (uint32_t channel = 0; channel < file.channel_count; ++channel) {
            if (channel && std::fputc(',', stream) == EOF) return false;
            if (std::fprintf(
                    stream, "{\"channel\":%u,\"peak_dbfs\":",
                    channel + 1u) < 0 ||
                !json_number(
                    stream,
                    file.channel_reconstructed_peak_dbfs[channel]) ||
                std::fprintf(
                    stream, ",\"seconds\":%.9f}",
                    file.channel_peak_seconds[channel]) < 0) {
                return false;
            }
        }
        if (std::fputs("]}", stream) == EOF) return false;
    }
    return std::fputs("\n  ]\n}\n", stream) != EOF;
}

bool write_report(
    FILE* stream, const char* input, const char* output,
    const crossdsd_decode_file_report& report) {
    const auto json_string = [](FILE* target, const char* value) {
        if (std::fputc('"', target) == EOF) return false;
        for (const unsigned char* cursor =
                 reinterpret_cast<const unsigned char*>(value);
             *cursor; ++cursor) {
            switch (*cursor) {
                case '"':
                    if (std::fputs("\\\"", target) == EOF) return false;
                    break;
                case '\\':
                    if (std::fputs("\\\\", target) == EOF) return false;
                    break;
                case '\b':
                    if (std::fputs("\\b", target) == EOF) return false;
                    break;
                case '\f':
                    if (std::fputs("\\f", target) == EOF) return false;
                    break;
                case '\n':
                    if (std::fputs("\\n", target) == EOF) return false;
                    break;
                case '\r':
                    if (std::fputs("\\r", target) == EOF) return false;
                    break;
                case '\t':
                    if (std::fputs("\\t", target) == EOF) return false;
                    break;
                default:
                    if (*cursor < 0x20u) {
                        if (std::fprintf(target, "\\u%04x", *cursor) < 0) {
                            return false;
                        }
                    } else if (std::fputc(*cursor, target) == EOF) {
                        return false;
                    }
            }
        }
        return std::fputc('"', target) != EOF;
    };
    if (std::fputs("{\n  \"input\":", stream) == EOF ||
        !json_string(stream, input) ||
        std::fputs(",\n  \"output\":", stream) == EOF ||
        !json_string(stream, output) ||
        std::fputs(",\n", stream) == EOF) {
        return false;
    }
    if (std::fprintf(
            stream,
            "  \"input_rate_hz\":%u,\n"
            "  \"output_rate_hz\":%u,\n"
            "  \"sample_format\":\"float32\",\n"
            "  \"channels\":%u,\n"
            "  \"profile\":\"%s\",\n"
            "  \"container\":\"%s\",\n"
            "  \"backend\":\"%s\",\n"
            "  \"input_samples_per_channel\":%llu,\n"
            "  \"output_frames\":%llu,\n"
            "  \"output_size_bytes\":%llu,\n"
            "  \"metadata_size_bytes\":%llu,\n"
            "  \"metadata_status\":\"%s\",\n"
            "  \"duration_seconds\":%.9f,\n"
            "  \"decode_seconds\":%.9f,\n"
            "  \"preparation_seconds\":%.9f,\n"
            "  \"processing_seconds\":%.9f,\n"
            "  \"writing_seconds\":%.9f,\n"
            "  \"realtime_factor\":%.9f,\n"
            "  \"latency_output_samples\":%.9f,\n"
            "  \"channel_peaks\":[",
            report.input_sample_rate_hz, report.output_sample_rate_hz,
            report.channel_count,
            profile_name(report.filter_profile),
            container_name(report.container), backend_name(report.backend),
            static_cast<unsigned long long>(
                report.input_samples_per_channel),
            static_cast<unsigned long long>(report.output_frames),
            static_cast<unsigned long long>(report.output_size_bytes),
            static_cast<unsigned long long>(report.metadata_size_bytes),
            report.metadata_status == CROSSDSD_METADATA_ID3
                ? "id3-copied"
                : (report.metadata_status == CROSSDSD_METADATA_INVALID
                       ? "invalid-ignored"
                       : "none"),
            report.duration_seconds, report.decode_seconds,
            report.preparation_seconds, report.processing_seconds,
            report.writing_seconds,
            report.realtime_factor, report.latency_output_samples) < 0) {
        return false;
    }
    for (uint32_t channel = 0; channel < report.channel_count; ++channel) {
        if (channel && std::fputc(',', stream) == EOF) return false;
        if (std::fprintf(stream, "%.9g", report.channel_peaks[channel]) < 0) {
            return false;
        }
    }
    return std::fprintf(stream, "]\n}\n") >= 0;
}

bool write_encode_report(
    FILE* stream, const char* input, const char* output,
    const crossdsd_encode_file_report& report) {
    const auto json_string = [](FILE* target, const char* value) {
        if (std::fputc('"', target) == EOF) return false;
        for (const unsigned char* cursor =
                 reinterpret_cast<const unsigned char*>(value);
             *cursor; ++cursor) {
            if (*cursor == '"' || *cursor == '\\') {
                if (std::fputc('\\', target) == EOF) return false;
            }
            if (*cursor < 0x20u) {
                if (std::fprintf(target, "\\u%04x", *cursor) < 0) return false;
            } else if (std::fputc(*cursor, target) == EOF) {
                return false;
            }
        }
        return std::fputc('"', target) != EOF;
    };
    if (std::fputs("{\n  \"operation\":\"encode\",\n  \"input\":", stream) ==
            EOF ||
        !json_string(stream, input) ||
        std::fputs(",\n  \"output\":", stream) == EOF ||
        !json_string(stream, output) ||
        std::fprintf(
            stream,
            ",\n"
            "  \"input_rate_hz\":%u,\n"
            "  \"output_rate_hz\":%u,\n"
            "  \"sample_format\":\"float32\",\n"
            "  \"dsd_rate\":\"%s\",\n"
            "  \"encoder_profile\":\"%s\",\n"
            "  \"channels\":%u,\n"
            "  \"channel_type\":%u,\n"
            "  \"input_frames\":%llu,\n"
            "  \"output_dsd_samples_per_channel\":%llu,\n"
            "  \"output_packed_bytes_per_channel\":%llu,\n"
            "  \"output_size_bytes\":%llu,\n"
            "  \"metadata_size_bytes\":%llu,\n"
            "  \"duration_seconds\":%.9f,\n"
            "  \"encode_seconds\":%.9f,\n"
            "  \"processing_seconds\":%.9f,\n"
            "  \"writing_seconds\":%.9f,\n"
            "  \"realtime_factor\":%.9f,\n"
            "  \"latency_output_samples\":%.9f,\n"
            "  \"enhanced_dsd_samples_per_channel\":%llu,\n"
            "  \"enhanced_segment_count\":%llu,\n"
            "  \"channel_input_peaks\":[",
            report.input_sample_rate_hz, report.output_sample_rate_hz,
            report.dsd_rate == CROSSDSD_DSD512 ? "DSD512"
            : report.dsd_rate == CROSSDSD_DSD256 ? "DSD256"
            : report.dsd_rate == CROSSDSD_DSD128 ? "DSD128"
                                                  : "DSD64",
            encoder_profile_name(report.encoder_profile),
            report.channel_count, report.channel_type,
            static_cast<unsigned long long>(report.input_frames),
            static_cast<unsigned long long>(
                report.output_dsd_samples_per_channel),
            static_cast<unsigned long long>(
                report.output_packed_bytes_per_channel),
            static_cast<unsigned long long>(report.output_size_bytes),
            static_cast<unsigned long long>(report.metadata_size_bytes),
            report.duration_seconds, report.encode_seconds,
            report.processing_seconds, report.writing_seconds,
            report.realtime_factor, report.latency_output_samples,
            static_cast<unsigned long long>(
                report.enhanced_dsd_samples_per_channel),
            static_cast<unsigned long long>(
                report.enhanced_segment_count)) < 0) {
        return false;
    }
    const auto write_double_array =
        [&](const char* next_name, const double* values) {
            for (uint32_t channel = 0; channel < report.channel_count; ++channel) {
                if (channel && std::fputc(',', stream) == EOF) return false;
                if (std::fprintf(stream, "%.12g", values[channel]) < 0) {
                    return false;
                }
            }
            return std::fprintf(stream, "],\n  \"%s\":[", next_name) >= 0;
        };
    if (!write_double_array(
            "channel_maximum_states", report.channel_input_peaks) ||
        !write_double_array(
            "channel_maximum_quantizer_inputs",
            report.channel_maximum_states) ||
        !write_double_array(
            "channel_one_densities",
            report.channel_maximum_quantizer_inputs)) {
        return false;
    }
    for (uint32_t channel = 0; channel < report.channel_count; ++channel) {
        if (channel && std::fputc(',', stream) == EOF) return false;
        if (std::fprintf(
                stream, "%.12g", report.channel_one_densities[channel]) < 0) {
            return false;
        }
    }
    if (std::fputs(
            "],\n  \"channel_maximum_identical_bit_runs\":[", stream) == EOF) {
        return false;
    }
    for (uint32_t channel = 0; channel < report.channel_count; ++channel) {
        if (channel && std::fputc(',', stream) == EOF) return false;
        if (std::fprintf(
                stream, "%llu",
                static_cast<unsigned long long>(
                    report.channel_maximum_identical_bit_runs[channel])) < 0) {
            return false;
        }
    }
    return std::fputs("]\n}\n", stream) != EOF;
}

bool write_transcode_report(
    FILE* stream, const char* input, const char* output,
    const crossdsd_transcode_file_report& report) {
    if (std::fprintf(
        stream,
        "{\n"
        "  \"input\":\"%s\",\n"
        "  \"output\":\"%s\",\n"
        "  \"input_dsd_rate\":%u,\n"
        "  \"output_dsd_rate\":%u,\n"
        "  \"encoder_profile\":\"%s\",\n"
        "  \"channels\":%u,\n"
        "  \"reconstructed_frames\":%llu,\n"
        "  \"reconstructed_peak\":%.12g,\n"
        "  \"reconstructed_peak_dbfs\":",
        input, output, static_cast<unsigned>(report.input_dsd_rate),
        static_cast<unsigned>(report.output_dsd_rate),
        encoder_profile_name(report.encoder_profile), report.channel_count,
        static_cast<unsigned long long>(report.reconstructed_frames),
        report.reconstructed_peak) < 0) {
        return false;
    }
    if (std::isfinite(report.reconstructed_peak_dbfs)) {
        if (std::fprintf(
                stream, "%.12g", report.reconstructed_peak_dbfs) < 0) {
            return false;
        }
    } else if (std::fputs("null", stream) == EOF) {
        return false;
    }
    if (std::fprintf(
        stream,
        ",\n"
        "  \"max_peak_enabled\":%s,\n"
        "  \"peak_search\":\"%s\",\n"
        "  \"quick_scan_seconds\":%.9f,\n"
        "  \"analysis_input_fraction\":%.9f,\n"
        "  \"requested_max_peak_dbfs\":%.12g,\n"
        "  \"applied_gain\":%.12g,\n"
        "  \"applied_gain_db\":%.12g,\n"
        "  \"duration_seconds\":%.9f,\n"
        "  \"analysis_seconds\":%.9f,\n"
        "  \"transcode_seconds\":%.9f,\n"
        "  \"realtime_factor\":%.9f,\n"
        "  \"output_dsd_samples_per_channel\":%llu,\n"
        "  \"output_packed_bytes_per_channel\":%llu,\n"
        "  \"output_size_bytes\":%llu,\n"
        "  \"metadata_size_bytes\":%llu,\n"
        "  \"enhanced_dsd_samples_per_channel\":%llu,\n"
        "  \"enhanced_segment_count\":%llu,\n"
        "  \"channel_reconstructed_peaks\":[",
        report.max_peak_enabled ? "true" : "false",
        report.full_peak_search ? "full" : "quick",
        report.quick_scan_seconds, report.analysis_input_fraction,
        report.requested_max_peak_dbfs, report.applied_gain,
        report.applied_gain_db, report.duration_seconds,
        report.analysis_seconds, report.transcode_seconds,
        report.realtime_factor,
        static_cast<unsigned long long>(
            report.output_dsd_samples_per_channel),
        static_cast<unsigned long long>(
            report.output_packed_bytes_per_channel),
        static_cast<unsigned long long>(report.output_size_bytes),
        static_cast<unsigned long long>(report.metadata_size_bytes),
        static_cast<unsigned long long>(
            report.enhanced_dsd_samples_per_channel),
        static_cast<unsigned long long>(
            report.enhanced_segment_count)) < 0) {
        return false;
    }
    for (uint32_t channel = 0; channel < report.channel_count; ++channel) {
        if (channel && std::fputc(',', stream) == EOF) return false;
        if (std::fprintf(
                stream, "%.12g",
                report.channel_reconstructed_peaks[channel]) < 0) {
            return false;
        }
    }
    if (std::fputs(
            "],\n  \"channel_maximum_states\":[", stream) == EOF) {
        return false;
    }
    for (uint32_t channel = 0; channel < report.channel_count; ++channel) {
        if (channel && std::fputc(',', stream) == EOF) return false;
        if (std::fprintf(
                stream, "%.12g",
                report.channel_maximum_states[channel]) < 0) {
            return false;
        }
    }
    if (std::fputs(
            "],\n  \"channel_maximum_quantizer_inputs\":[", stream) == EOF) {
        return false;
    }
    for (uint32_t channel = 0; channel < report.channel_count; ++channel) {
        if (channel && std::fputc(',', stream) == EOF) return false;
        if (std::fprintf(
                stream, "%.12g",
                report.channel_maximum_quantizer_inputs[channel]) < 0) {
            return false;
        }
    }
    if (std::fputs(
            "],\n  \"channel_maximum_identical_bit_runs\":[", stream) ==
        EOF) {
        return false;
    }
    for (uint32_t channel = 0; channel < report.channel_count; ++channel) {
        if (channel && std::fputc(',', stream) == EOF) return false;
        if (std::fprintf(
                stream, "%llu",
                static_cast<unsigned long long>(
                    report.channel_maximum_identical_bit_runs[channel])) < 0) {
            return false;
        }
    }
    if (std::fputs(
            "],\n  \"channel_one_densities\":[", stream) == EOF) {
        return false;
    }
    for (uint32_t channel = 0; channel < report.channel_count; ++channel) {
        if (channel && std::fputc(',', stream) == EOF) return false;
        if (std::fprintf(
                stream, "%.12g",
                report.channel_one_densities[channel]) < 0) {
            return false;
        }
    }
    return std::fputs("]\n}\n", stream) != EOF;
}

int run_analyze(int argc, char** argv) {
    std::vector<const char*> inputs;
    const char* report_path = nullptr;
    crossdsd_analyze_batch_config config{};
    config.struct_size = sizeof(config);
    config.target_peak_dbfs = 0.0;
    ProgressState progress_state;
    config.progress_callback = analyze_progress;
    config.progress_user_data = &progress_state;
    for (int index = 2; index < argc; ++index) {
        const char* argument = argv[index];
        if (std::strcmp(argument, "--quicksearch") == 0) {
            config.full_peak_search = 0;
        } else if (std::strcmp(argument, "--fullsearch") == 0) {
            config.full_peak_search = 1;
        } else if (std::strncmp(argument, "--album-target=", 15) == 0) {
            char* end = nullptr;
            errno = 0;
            const double value = std::strtod(argument + 15, &end);
            if (errno != 0 || !end || *end != '\0' ||
                !std::isfinite(value) || value > 0.0) {
                usage();
                return 2;
            }
            config.target_peak_dbfs = value;
        } else if (std::strncmp(argument, "--report=", 9) == 0) {
            report_path = argument + 9;
        } else {
            inputs.push_back(argument);
        }
    }
    if (inputs.empty()) {
        usage();
        return 2;
    }
    std::vector<crossdsd_analyze_file_report> files(inputs.size());
    std::vector<double> individual_gains(inputs.size());
    crossdsd_analyze_batch_report report{};
    report.struct_size = sizeof(report);
    crossdsd_error error{};
    error.struct_size = sizeof(error);
    std::signal(SIGINT, interrupt_handler);
    const auto result = crossdsd_analyze_batch(
        inputs.data(), static_cast<uint32_t>(inputs.size()), &config,
        files.data(), individual_gains.data(), &report, &error);
    if (result != CROSSDSD_OK) {
        std::fputc('\n', stderr);
        std::fprintf(
            stderr, "crossdsd: %s (%s)\n", error.message,
            crossdsd_result_string(result));
        return 1;
    }
    std::fputs("\rAnalyzing: 100%\n", stderr);
    for (size_t index = 0; index < files.size(); ++index) {
        const auto& file = files[index];
        std::fprintf(
            stderr,
            "%zu. %s: DSD%u, peak %.3f dBFS on channel %u at %.3f s; "
            "individual gain %.3f dB.\n",
            index + 1u, inputs[index],
            static_cast<unsigned>(file.input_dsd_rate),
            file.reconstructed_peak_dbfs, file.peak_channel + 1u,
            file.peak_seconds, individual_gains[index]);
    }
    std::fprintf(
        stderr,
        "Analyzed %u file(s), %.3f s of audio, using %s search in %.3f s "
        "(%.1fx real time). Album gain for %.3f dBFS: %.3f dB.\n",
        report.input_file_count, report.total_duration_seconds,
        report.full_peak_search ? "full" : "quick",
        report.analysis_seconds, report.realtime_factor,
        report.target_peak_dbfs, report.common_gain_db);
    if (report_path) {
        FILE* report_file =
            std::strcmp(report_path, "-") == 0
                ? stdout
                : crossdsd::platform::open_file(report_path, "wb");
        if (!report_file) {
            std::fprintf(stderr, "crossdsd: cannot create report\n");
            return 1;
        }
        const bool ok = inputs.size() == 1
            ? write_analyze_report(
                  report_file, inputs[0], files[0],
                  report.target_peak_dbfs, individual_gains[0])
            : write_analyze_batch_report(
                  report_file, inputs, files, individual_gains, report);
        if (report_file != stdout && std::fclose(report_file) != 0) return 1;
        if (!ok) return 1;
    }
    return 0;
}

int run_transcode(int argc, char** argv) {
    const char* input = nullptr;
    const char* output = nullptr;
    const char* report_path = nullptr;
    bool overwrite = false;
    bool rate_supplied = false;
    crossdsd_transcode_file_config config{};
    config.struct_size = sizeof(config);
    config.encoder_profile = CROSSDSD_ENCODER_STANDARD;
    ProgressState progress_state;
    config.progress_callback = transcode_progress;
    config.progress_user_data = &progress_state;
    for (int index = 2; index < argc; ++index) {
        const char* argument = argv[index];
        if (std::strcmp(argument, "--dsd-rate=64") == 0) {
            config.dsd_rate = CROSSDSD_DSD64;
            rate_supplied = true;
        } else if (std::strcmp(argument, "--dsd-rate=128") == 0) {
            config.dsd_rate = CROSSDSD_DSD128;
            rate_supplied = true;
        } else if (std::strcmp(argument, "--dsd-rate=256") == 0) {
            config.dsd_rate = CROSSDSD_DSD256;
            rate_supplied = true;
        } else if (std::strcmp(argument, "--dsd-rate=512") == 0) {
            config.dsd_rate = CROSSDSD_DSD512;
            rate_supplied = true;
        } else if (std::strcmp(argument, "--enhanced") == 0) {
            config.encoder_profile = CROSSDSD_ENCODER_DSD64_ENHANCED;
        } else if (std::strncmp(argument, "--maxpeak=", 10) == 0) {
            char* end = nullptr;
            errno = 0;
            const double value = std::strtod(argument + 10, &end);
            if (errno != 0 || !end || *end != '\0' ||
                !std::isfinite(value) || value > 0.0) {
                usage();
                return 2;
            }
            config.max_peak_enabled = 1;
            config.max_peak_dbfs = value;
        } else if (std::strcmp(argument, "--quicksearch") == 0) {
            config.full_peak_search = 0;
        } else if (std::strcmp(argument, "--fullsearch") == 0) {
            config.full_peak_search = 1;
        } else if (std::strncmp(argument, "--channel-type=", 15) == 0) {
            char* end = nullptr;
            errno = 0;
            const unsigned long value =
                std::strtoul(argument + 15, &end, 10);
            if (errno != 0 || !end || *end != '\0' || value > UINT32_MAX) {
                usage();
                return 2;
            }
            config.channel_type = static_cast<uint32_t>(value);
        } else if (std::strncmp(argument, "--report=", 9) == 0) {
            report_path = argument + 9;
        } else if (std::strcmp(argument, "--overwrite") == 0) {
            overwrite = true;
        } else if (!input) {
            input = argument;
        } else if (!output) {
            output = argument;
        } else {
            usage();
            return 2;
        }
    }
    if (!input || !output || !rate_supplied) {
        usage();
        return 2;
    }
    if (config.dsd_rate != CROSSDSD_DSD64) {
        config.encoder_profile = CROSSDSD_ENCODER_STANDARD;
    }
    std::error_code filesystem_error;
    const std::filesystem::path output_path = path_from_utf8(output);
    if (!overwrite && std::filesystem::exists(output_path, filesystem_error)) {
        std::fprintf(stderr, "crossdsd: output already exists: %s\n", output);
        return 1;
    }
    std::filesystem::path temporary_path = output_path;
    temporary_path += path_from_utf8(
        (".crossdsd-part-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()))
            .c_str());
    const std::string temporary_utf8 = path_to_utf8(temporary_path);
    crossdsd_transcode_file_report report{};
    report.struct_size = sizeof(report);
    crossdsd_error error{};
    error.struct_size = sizeof(error);
    std::signal(SIGINT, interrupt_handler);
    const auto result = crossdsd_transcode_file(
        input, temporary_utf8.c_str(), &config, &report, &error);
    if (result != CROSSDSD_OK) {
        std::fputc('\n', stderr);
        std::filesystem::remove(temporary_path, filesystem_error);
        std::fprintf(
            stderr, "crossdsd: %s (%s)\n", error.message,
            crossdsd_result_string(result));
        return 1;
    }
    std::fputs("\rTranscoding: 100%\n", stderr);
    std::filesystem::rename(temporary_path, output_path, filesystem_error);
    if (filesystem_error && overwrite) {
        filesystem_error.clear();
        std::filesystem::remove(output_path, filesystem_error);
        if (!filesystem_error) {
            std::filesystem::rename(
                temporary_path, output_path, filesystem_error);
        }
    }
    if (filesystem_error) {
        std::filesystem::remove(temporary_path, filesystem_error);
        std::fprintf(stderr, "crossdsd: cannot install finalized output\n");
        return 1;
    }
    std::fprintf(
        stderr,
        "Transcoded %.3f s from DSD%u to DSD%u%s in %.3f s (%.2fx real "
        "time). Reconstructed peak %.3f dBFS; applied gain %.3f dB.\n",
        report.duration_seconds,
        static_cast<unsigned>(report.input_dsd_rate),
        static_cast<unsigned>(report.output_dsd_rate),
        report.encoder_profile == CROSSDSD_ENCODER_DSD64_ENHANCED
            ? " Enhanced" : "",
        report.analysis_seconds + report.transcode_seconds,
        report.realtime_factor, report.reconstructed_peak_dbfs,
        report.applied_gain_db);
    if (report_path) {
        FILE* report_file =
            std::strcmp(report_path, "-") == 0
                ? stdout
                : crossdsd::platform::open_file(report_path, "wb");
        if (!report_file) {
            std::fprintf(stderr, "crossdsd: cannot create report\n");
            return 1;
        }
        const bool ok =
            write_transcode_report(report_file, input, output, report);
        if (report_file != stdout && std::fclose(report_file) != 0) return 1;
        if (!ok) return 1;
    }
    return 0;
}

int run_encode(int argc, char** argv) {
    const char* input = nullptr;
    const char* output = nullptr;
    const char* report_path = nullptr;
    bool overwrite = false;
    bool rate_supplied = false;
    crossdsd_encode_file_config config{};
    config.struct_size = sizeof(config);
    config.encoder_profile = CROSSDSD_ENCODER_STANDARD;
    ProgressState progress_state;
    config.progress_callback = encode_progress;
    config.progress_user_data = &progress_state;
    for (int index = 2; index < argc; ++index) {
        const char* argument = argv[index];
        if (std::strcmp(argument, "--dsd-rate=64") == 0) {
            config.dsd_rate = CROSSDSD_DSD64;
            rate_supplied = true;
        } else if (std::strcmp(argument, "--dsd-rate=128") == 0) {
            config.dsd_rate = CROSSDSD_DSD128;
            rate_supplied = true;
        } else if (std::strcmp(argument, "--dsd-rate=256") == 0) {
            config.dsd_rate = CROSSDSD_DSD256;
            rate_supplied = true;
        } else if (std::strcmp(argument, "--dsd-rate=512") == 0) {
            config.dsd_rate = CROSSDSD_DSD512;
            rate_supplied = true;
        } else if (std::strcmp(argument, "--enhanced") == 0) {
            config.encoder_profile = CROSSDSD_ENCODER_DSD64_ENHANCED;
        } else if (std::strncmp(argument, "--channel-type=", 15) == 0) {
            char* end = nullptr;
            errno = 0;
            const unsigned long value =
                std::strtoul(argument + 15, &end, 10);
            if (errno != 0 || !end || *end != '\0' || value > UINT32_MAX) {
                usage();
                return 2;
            }
            config.channel_type = static_cast<uint32_t>(value);
        } else if (std::strncmp(argument, "--report=", 9) == 0) {
            report_path = argument + 9;
        } else if (std::strcmp(argument, "--overwrite") == 0) {
            overwrite = true;
        } else if (!input) {
            input = argument;
        } else if (!output) {
            output = argument;
        } else {
            usage();
            return 2;
        }
    }
    if (!input || !output || !rate_supplied) {
        usage();
        return 2;
    }
    if (config.dsd_rate != CROSSDSD_DSD64) {
        config.encoder_profile = CROSSDSD_ENCODER_STANDARD;
    }
    std::error_code filesystem_error;
    const std::filesystem::path output_path = path_from_utf8(output);
    if (!overwrite &&
        std::filesystem::exists(output_path, filesystem_error)) {
        std::fprintf(stderr, "crossdsd: output already exists: %s\n", output);
        return 1;
    }
    std::filesystem::path temporary_path = output_path;
    temporary_path += path_from_utf8(
        (".crossdsd-part-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()))
            .c_str());
    const std::string temporary_utf8 = path_to_utf8(temporary_path);
    crossdsd_encode_file_report report{};
    report.struct_size = sizeof(report);
    crossdsd_error error{};
    error.struct_size = sizeof(error);
    std::signal(SIGINT, interrupt_handler);
    const auto result = crossdsd_encode_file(
        input, temporary_utf8.c_str(), &config, &report, &error);
    if (result != CROSSDSD_OK) {
        std::fputc('\n', stderr);
        std::filesystem::remove(temporary_path, filesystem_error);
        std::fprintf(
            stderr, "crossdsd: %s (%s)\n", error.message,
            crossdsd_result_string(result));
        return 1;
    }
    std::fputs("\rEncoding: 100%\n", stderr);
    std::filesystem::rename(temporary_path, output_path, filesystem_error);
    if (filesystem_error && overwrite) {
        filesystem_error.clear();
        std::filesystem::remove(output_path, filesystem_error);
        if (!filesystem_error) {
            std::filesystem::rename(
                temporary_path, output_path, filesystem_error);
        }
    }
    if (filesystem_error) {
        std::filesystem::remove(temporary_path, filesystem_error);
        std::fprintf(stderr, "crossdsd: cannot install finalized output\n");
        return 1;
    }
    std::fprintf(
        stderr,
        "Encoded %.3f s of float32 %.1f kHz audio to DSD%u%s in %.3f s "
        "(%.1fx real time).\n",
        report.duration_seconds,
        static_cast<double>(report.input_sample_rate_hz) / 1000.0,
        static_cast<unsigned>(report.dsd_rate),
        report.encoder_profile == CROSSDSD_ENCODER_DSD64_ENHANCED
            ? " Enhanced"
            : "",
        report.encode_seconds,
        report.realtime_factor);
    if (report_path) {
        FILE* report_file =
            std::strcmp(report_path, "-") == 0
                ? stdout
                : crossdsd::platform::open_file(report_path, "wb");
        if (!report_file) {
            std::fprintf(stderr, "crossdsd: cannot create report: %s\n",
                         std::strerror(errno));
            return 1;
        }
        const bool ok =
            write_encode_report(report_file, input, output, report);
        if (report_file != stdout && std::fclose(report_file) != 0) {
            std::fprintf(stderr, "crossdsd: cannot finalize report\n");
            return 1;
        }
        if (!ok) {
            std::fprintf(stderr, "crossdsd: cannot write report\n");
            return 1;
        }
    }
    return 0;
}

int run_upscale(int argc, char** argv) {
    const char* input = nullptr;
    const char* output = nullptr;
    bool overwrite = false;
    crossdsd_aiff_to_dxd_config config{};
    config.struct_size = sizeof(config);
    config.container = CROSSDSD_WAVE_AUTO;
    ProgressState progress_state;
    config.progress_callback = upscale_progress;
    config.progress_user_data = &progress_state;
    for (int index = 2; index < argc; ++index) {
        const char* argument = argv[index];
        if (std::strcmp(argument, "--container=auto") == 0)
            config.container = CROSSDSD_WAVE_AUTO;
        else if (std::strcmp(argument, "--container=wav") == 0)
            config.container = CROSSDSD_WAVE_RIFF;
        else if (std::strcmp(argument, "--container=rf64") == 0)
            config.container = CROSSDSD_WAVE_RF64;
        else if (std::strcmp(argument, "--overwrite") == 0)
            overwrite = true;
        else if (!input)
            input = argument;
        else if (!output)
            output = argument;
        else {
            usage();
            return 2;
        }
    }
    if (!input || !output) {
        usage();
        return 2;
    }
    std::error_code filesystem_error;
    const std::filesystem::path final_path = path_from_utf8(output);
    if (!overwrite && std::filesystem::exists(final_path, filesystem_error)) {
        std::fprintf(stderr, "crossdsd: output already exists: %s\n", output);
        return 1;
    }
    std::filesystem::path temporary_path = final_path;
    temporary_path += path_from_utf8(
        (".crossdsd-part-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count())).c_str());
    const std::string temporary_utf8 = path_to_utf8(temporary_path);
    crossdsd_aiff_to_dxd_report report{};
    report.struct_size = sizeof(report);
    crossdsd_error error{};
    error.struct_size = sizeof(error);
    cancelled = 0;
    std::signal(SIGINT, interrupt_handler);
    const auto result = crossdsd_convert_aiff_to_dxd_file(
        input, temporary_utf8.c_str(), &config, &report, &error);
    if (result != CROSSDSD_OK) {
        std::fputc('\n', stderr);
        std::filesystem::remove(temporary_path, filesystem_error);
        std::fprintf(stderr, "crossdsd: %s (%s)\n", error.message,
                     crossdsd_result_string(result));
        return 1;
    }
    std::fputs("\rUpsampling: 100%\n", stderr);
    std::filesystem::rename(temporary_path, final_path, filesystem_error);
    if (filesystem_error && overwrite) {
        filesystem_error.clear();
        std::filesystem::remove(final_path, filesystem_error);
        if (!filesystem_error)
            std::filesystem::rename(temporary_path, final_path, filesystem_error);
    }
    if (filesystem_error) {
        std::filesystem::remove(temporary_path, filesystem_error);
        std::fprintf(stderr, "crossdsd: cannot install finalized output\n");
        return 1;
    }
    double input_peak = 0.0, output_peak = 0.0;
    for (uint32_t channel = 0; channel < report.channel_count; ++channel) {
        input_peak = std::max(input_peak, report.channel_input_peaks[channel]);
        output_peak = std::max(output_peak, report.channel_output_peaks[channel]);
    }
    const auto dbfs = [](double peak) {
        return peak > 0.0 ? 20.0 * std::log10(peak)
                          : -std::numeric_limits<double>::infinity();
    };
    std::fprintf(stderr,
        "Upsampled %.3f s of AIFF %u-bit 44.1 kHz audio to float32 DXD "
        "352.8 kHz in %.3f s (%.1fx real time).\n"
        "Peaks: input %+.3f dBFS, output %+.3f dBFS; metadata %s.\n",
        report.duration_seconds, report.input_bits_per_sample,
        report.conversion_seconds, report.realtime_factor, dbfs(input_peak),
        dbfs(output_peak), report.metadata_size_bytes ? "preserved" : "absent");
    return 0;
}

}  // namespace

int crossdsd_main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("CrossDSD %s\n", CROSSDSD_VERSION);
        return 0;
    }
    if (argc >= 2 && std::strcmp(argv[1], "encode") == 0) {
        return run_encode(argc, argv);
    }
    if (argc >= 2 && std::strcmp(argv[1], "upscale") == 0) {
        return run_upscale(argc, argv);
    }
    if (argc >= 2 && std::strcmp(argv[1], "transcode") == 0) {
        return run_transcode(argc, argv);
    }
    if (argc >= 2 && std::strcmp(argv[1], "analyze") == 0) {
        return run_analyze(argc, argv);
    }
    if (argc < 2 || std::strcmp(argv[1], "decode") != 0) {
        usage();
        return 2;
    }
    const char* input = nullptr;
    const char* output = nullptr;
    const char* report_path = nullptr;
    bool overwrite = false;
    crossdsd_decode_file_config config{};
    config.struct_size = sizeof(config);
    config.filter_profile = CROSSDSD_FILTER_STANDARD;
    config.container = CROSSDSD_WAVE_AUTO;
    config.backend = CROSSDSD_BACKEND_AUTO;
    ProgressState progress_state;
    config.progress_callback = progress;
    config.progress_user_data = &progress_state;
    for (int index = 2; index < argc; ++index) {
        const char* argument = argv[index];
        if (std::strcmp(argument, "--profile=standard") == 0) {
            config.filter_profile = CROSSDSD_FILTER_STANDARD;
        } else if (std::strcmp(argument, "--profile=raw") == 0) {
            config.filter_profile = CROSSDSD_FILTER_RAW;
        } else if (std::strcmp(argument, "--container=auto") == 0) {
            config.container = CROSSDSD_WAVE_AUTO;
        } else if (std::strcmp(argument, "--container=wav") == 0) {
            config.container = CROSSDSD_WAVE_RIFF;
        } else if (std::strcmp(argument, "--container=rf64") == 0) {
            config.container = CROSSDSD_WAVE_RF64;
        } else if (std::strcmp(argument, "--backend=auto") == 0) {
            config.backend = CROSSDSD_BACKEND_AUTO;
        } else if (std::strcmp(argument, "--backend=cpu") == 0) {
            config.backend = CROSSDSD_BACKEND_CPU;
        } else if (std::strcmp(argument, "--backend=metal") == 0) {
            config.backend = CROSSDSD_BACKEND_METAL;
        } else if (std::strncmp(argument, "--report=", 9) == 0) {
            report_path = argument + 9;
        } else if (std::strcmp(argument, "--overwrite") == 0) {
            overwrite = true;
        } else if (!input) {
            input = argument;
        } else if (!output) {
            output = argument;
        } else {
            usage();
            return 2;
        }
    }
    if (!input || !output) {
        usage();
        return 2;
    }
    std::error_code filesystem_error;
    const std::filesystem::path output_path = path_from_utf8(output);
    if (!overwrite &&
        std::filesystem::exists(output_path, filesystem_error)) {
        std::fprintf(stderr, "crossdsd: output already exists: %s\n", output);
        return 1;
    }

    std::filesystem::path temporary_path = output_path;
    temporary_path +=
        path_from_utf8(
            (".crossdsd-part-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()))
                .c_str());
    const std::string temporary_utf8 = path_to_utf8(temporary_path);

    crossdsd_decode_file_report report{};
    report.struct_size = sizeof(report);
    crossdsd_error error{};
    error.struct_size = sizeof(error);
    std::signal(SIGINT, interrupt_handler);
    const auto result =
        crossdsd_decode_file(
            input, temporary_utf8.c_str(), &config, &report, &error);
    if (result != CROSSDSD_OK) {
        std::fputc('\n', stderr);
        std::filesystem::remove(temporary_path, filesystem_error);
        std::fprintf(
            stderr, "crossdsd: %s (%s)\n", error.message,
            crossdsd_result_string(result));
        return 1;
    }
    std::fputs("\rDecoding: 100%\n", stderr);
    std::filesystem::rename(temporary_path, output_path, filesystem_error);
    if (filesystem_error && overwrite) {
        filesystem_error.clear();
        std::filesystem::remove(output_path, filesystem_error);
        if (!filesystem_error) {
            std::filesystem::rename(
                temporary_path, output_path, filesystem_error);
        }
    }
    if (filesystem_error) {
        std::filesystem::remove(temporary_path, filesystem_error);
        std::fprintf(stderr, "crossdsd: cannot install finalized output\n");
        return 1;
    }
    std::fprintf(
        stderr,
        "Decoded %.3f s to float32 352.8 kHz %s using %s in %.3f s "
        "(%.1fx real time).\n",
        report.duration_seconds, container_name(report.container),
        backend_name(report.backend), report.decode_seconds,
        report.realtime_factor);
    if (report_path) {
        FILE* report_file =
            std::strcmp(report_path, "-") == 0
                ? stdout
                : crossdsd::platform::open_file(report_path, "wb");
        if (!report_file) {
            std::fprintf(stderr, "crossdsd: cannot create report: %s\n",
                         std::strerror(errno));
            return 1;
        }
        const bool ok = write_report(report_file, input, output, report);
        if (report_file != stdout && std::fclose(report_file) != 0) {
            std::fprintf(stderr, "crossdsd: cannot finalize report\n");
            return 1;
        }
        if (!ok) {
            std::fprintf(stderr, "crossdsd: cannot write report\n");
            return 1;
        }
    }
    return 0;
}

#if defined(_WIN32)
int wmain(int argc, wchar_t** wide_argv) {
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<size_t>(argc));
    for (int index = 0; index < argc; ++index) {
        const int size = WideCharToMultiByte(
            CP_UTF8, 0, wide_argv[index], -1, nullptr, 0, nullptr, nullptr);
        if (size <= 0) return 2;
        std::vector<char> buffer(static_cast<size_t>(size));
        if (WideCharToMultiByte(
                CP_UTF8, 0, wide_argv[index], -1, buffer.data(), size,
                nullptr, nullptr) <= 0) {
            return 2;
        }
        arguments.emplace_back(buffer.data());
    }
    std::vector<char*> argv;
    argv.reserve(arguments.size());
    for (auto& argument : arguments) argv.push_back(argument.data());
    return crossdsd_main(argc, argv.data());
}
#else
int main(int argc, char** argv) { return crossdsd_main(argc, argv); }
#endif
