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

#ifndef CROSSDSD_ENHANCED_MODULATOR_H
#define CROSSDSD_ENHANCED_MODULATOR_H

#include "crossdsd/dsp.h"

#include <cstdint>

namespace crossdsd::enhanced {

struct Modulator;

Modulator* create(uint32_t channel_count);
void destroy(Modulator* modulator);

crossdsd_result process(
    Modulator* modulator,
    const double* input_planar,
    uint64_t input_count_per_channel,
    uint64_t input_stride,
    int8_t* output_planar,
    uint64_t output_capacity_per_channel,
    uint64_t output_stride,
    uint64_t* output_count_per_channel);

crossdsd_result finish(
    Modulator* modulator,
    int8_t* output_planar,
    uint64_t output_capacity_per_channel,
    uint64_t output_stride,
    uint64_t* output_count_per_channel);

crossdsd_result reset(Modulator* modulator);

crossdsd_result get_channel_metrics(
    const Modulator* modulator,
    uint32_t channel_index,
    crossdsd_modulator_channel_metrics* metrics);

uint64_t enhanced_samples(const Modulator* modulator);
uint64_t enhanced_segments(const Modulator* modulator);
uint32_t stability_failure(const Modulator* modulator);

}  // namespace crossdsd::enhanced

#endif
