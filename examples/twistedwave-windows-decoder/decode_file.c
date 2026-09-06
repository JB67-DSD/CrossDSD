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

#include <crossdsd/convert.h>

#include <stdio.h>

static int show_progress(void* user_data, double completed_fraction) {
    int* cancelled = (int*)user_data;
    fprintf(stderr, "\rCrossDSD decode: %.0f%%", completed_fraction * 100.0);
    return !*cancelled;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s INPUT.dsf OUTPUT.wav\n", argv[0]);
        return 2;
    }

    int cancelled = 0;
    crossdsd_decode_file_config config = {0};
    config.struct_size = sizeof(config);
    config.filter_profile = CROSSDSD_FILTER_STANDARD;
    config.container = CROSSDSD_WAVE_AUTO;
    config.backend = CROSSDSD_BACKEND_CPU;
    config.progress_callback = show_progress;
    config.progress_user_data = &cancelled;

    crossdsd_decode_file_report report = {0};
    report.struct_size = sizeof(report);
    crossdsd_error error = {0};
    error.struct_size = sizeof(error);

    const crossdsd_result result = crossdsd_decode_file(
        argv[1], argv[2], &config, &report, &error);
    fprintf(stderr, "\n");
    if (result != CROSSDSD_OK) {
        fprintf(
            stderr, "CrossDSD failed: %s (%s)\n", error.message,
            crossdsd_result_string(result));
        return 1;
    }

    printf(
        "Created %llu float32 frames at %u Hz (%.2fx real time).\n",
        (unsigned long long)report.output_frames,
        report.output_sample_rate_hz,
        report.realtime_factor);
    for (uint32_t channel = 0; channel < report.channel_count; ++channel) {
        printf(
            "Channel %u peak: %.9g\n",
            channel + 1, report.channel_peaks[channel]);
    }
    return 0;
}
