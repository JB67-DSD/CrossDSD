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
#include <stdlib.h>
#include <string.h>

static int show_progress(void* user_data, double completed_fraction) {
    int* cancelled = (int*)user_data;
    fprintf(stderr, "\rCrossDSD export: %.0f%%", completed_fraction * 100.0);
    return !*cancelled;
}

int main(int argc, char** argv) {
    if (argc != 4 && argc != 5) {
        fprintf(
            stderr,
            "usage: %s INPUT_DXD.wav OUTPUT.dsf 64|128|256|512 [--enhanced]\n",
            argv[0]);
        return 2;
    }
    const int requested_rate = atoi(argv[3]);
    if (requested_rate != 64 && requested_rate != 128 &&
        requested_rate != 256 && requested_rate != 512) {
        fprintf(stderr, "DSD rate must be 64, 128, 256 or 512.\n");
        return 2;
    }
    const int enhanced =
        argc == 5 && strcmp(argv[4], "--enhanced") == 0;
    if (argc == 5 && !enhanced) {
        fprintf(stderr, "The only optional argument is --enhanced.\n");
        return 2;
    }
    if (enhanced && requested_rate != 64) {
        fprintf(stderr, "--enhanced is available for DSD64 only.\n");
        return 2;
    }

    int cancelled = 0;
    crossdsd_encode_file_config config = {0};
    config.struct_size = sizeof(config);
    config.dsd_rate = (crossdsd_dsd_rate)requested_rate;
    config.encoder_profile =
        enhanced ? CROSSDSD_ENCODER_DSD64_ENHANCED
                 : CROSSDSD_ENCODER_STANDARD;
    config.progress_callback = show_progress;
    config.progress_user_data = &cancelled;

    crossdsd_encode_file_report report = {0};
    report.struct_size = sizeof(report);
    crossdsd_error error = {0};
    error.struct_size = sizeof(error);

    const crossdsd_result result = crossdsd_encode_file(
        argv[1], argv[2], &config, &report, &error);
    fprintf(stderr, "\n");
    if (result != CROSSDSD_OK) {
        fprintf(
            stderr, "CrossDSD failed: %s (%s)\n", error.message,
            crossdsd_result_string(result));
        return 1;
    }

    printf(
        "Created DSD%d%s at %u Hz: %llu samples/channel, %.2fx real time.\n",
        requested_rate, enhanced ? " Enhanced" : "",
        report.output_sample_rate_hz,
        (unsigned long long)report.output_dsd_samples_per_channel,
        report.realtime_factor);
    for (uint32_t channel = 0; channel < report.channel_count; ++channel) {
        printf(
            "Channel %u: input peak %.9g, max state %.9g, "
            "max quantizer input %.9g\n",
            channel + 1, report.channel_input_peaks[channel],
            report.channel_maximum_states[channel],
            report.channel_maximum_quantizer_inputs[channel]);
    }
    return 0;
}
