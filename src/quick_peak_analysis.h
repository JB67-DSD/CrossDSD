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

#ifndef CROSSDSD_QUICK_PEAK_ANALYSIS_H
#define CROSSDSD_QUICK_PEAK_ANALYSIS_H

#include "crossdsd/convert.h"

namespace crossdsd::internal {

struct QuickPeakAnalysis {
    uint64_t reconstructed_frames = 0;
    double peaks[CROSSDSD_MAX_CHANNELS]{};
    uint64_t peak_frames[CROSSDSD_MAX_CHANNELS]{};
    double scan_seconds = 0.0;
    double fine_seconds = 0.0;
    double fine_input_fraction = 0.0;
    uint32_t worker_count = 1;
};

crossdsd_result run_quick_peak_analysis(
    const char* input_path, const crossdsd_stream_info& stream,
    uint64_t chunk_bytes, crossdsd_progress_callback progress_callback,
    void* progress_user_data, QuickPeakAnalysis& analysis,
    crossdsd_error* error);

crossdsd_result run_full_peak_analysis(
    const char* input_path, const crossdsd_stream_info& stream,
    uint64_t chunk_bytes, crossdsd_progress_callback progress_callback,
    void* progress_user_data, QuickPeakAnalysis& analysis,
    crossdsd_error* error);

}  // namespace crossdsd::internal

#endif
