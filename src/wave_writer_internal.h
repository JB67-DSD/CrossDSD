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

#ifndef CROSSDSD_WAVE_WRITER_INTERNAL_H
#define CROSSDSD_WAVE_WRITER_INTERNAL_H

#include "crossdsd/audio.h"

namespace crossdsd {

uint64_t wave_writer_size_bytes(const crossdsd_wave_writer& writer);

}  // namespace crossdsd

#endif
