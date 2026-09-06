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

#include <crossdsd/audio.h>
#include <crossdsd/convert.h>
#include <crossdsd/dsp.h>
#include <crossdsd/formats.h>

int main(void) {
    return CROSSDSD_DXD_SAMPLE_RATE == 352800u &&
                   CROSSDSD_DSP_API_VERSION == 7u &&
                   CROSSDSD_CONVERT_API_VERSION == 9u
               ? 0
               : 1;
}
