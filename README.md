# CrossDSD Library

## For developers

- [Build, test and integrate the library](docs/GETTING_STARTED.md)
- [Explore the C API](docs/API_GUIDE.md) and [examples](examples/README.md)
- [Use the crossdsd command-line tool](docs/CROSSDSD_COMMAND_LINE_REFERENCE.md)
- [Get the Mac and Windows SDKs](releases/README.md)
- [Install and verify the reference signals](reference-signals/README.md)
- [Read the changelog](CHANGELOG.md)

This repository covers only the CrossDSD library and its command-line tools.

## Introduction

The CrossDSD library is primarily intended for integration into software for music enthusiasts, making it easy to work with DSD (.dsf) and DXD (.wav/RF64) audio files while maintaining very high audio quality. It provides straightforward conversion between these formats with the needs of audiophiles in mind.

I started this project after finding that affordable, high-quality DSD tools for amateur users were scarce. As the owner of a SONY PCM D100 used to record concerts in DSD64, I encountered the editing difficulties inherent in DSD, despite its outstanding audio quality.

On macOS, some of the software I used, such as KORG AudioGate and TASCAM Hi-Res Editor, appeared to be nearing the end of its life, with little prospect of an Apple Silicon port. I also use TwistedWave which, despite its many strengths, relied on FFmpeg to import DSD and could not export it. Although FFmpeg makes it possible to import DSD streams, the lack of ultrasonic filtering and the change in SACD reference level in that conversion path posed problems for some audiophiles and audio equipment.

Creating CrossDSD therefore became a natural next step. With help from AI and an extensive set of reproducible tests, it became a reality. The guiding principles of the project have been quality and simplicity. The aim was not to provide hundreds of functions already available in audio editors, but a small set of useful, less common functions that preserve the essence of my recordings.

To support this work, a corpus of reference DXD and DSD files was created (CrossDSD_Reference_Signals). Every encoding and decoding algorithm was tested scientifically and methodically to achieve the best possible quality. Once candidate algorithms had been selected, they were challenged with demanding audiophile DSD music recordings: sudden level increases, very quiet passages, and high levels approaching the SACD reference limit of +3.1 dB, among other cases. These tests helped confirm the choices made.

I believe the result meets both my expectations and those of many DSD enthusiasts. CrossDSD has the potential to become a reference library for working with these files while preserving the audio quality of the recordings. I am therefore making the library available to the community, which is free to use it, integrate it into more extensive software, or extend it as new needs arise.

## What can CrossDSD do?

CrossDSD converts DSD64, DSD128, DSD256 and DSD512 files to DXD (32-bit floating point, 352.8 kHz), and vice versa. While DXD (Digital eXtreme Definition) conventionally refers to 24-bit audio at 352.8 kHz, I chose 32-bit floating point to preserve the maximum SACD reference levels (+3.1 dB) without clipping. I also introduced a DSD64 Enhanced encoding mode. Our algorithm explorations identified a treatment that improves quantization noise in passages where it provides a measurable benefit, at a significant computational cost. We retained it only for DSD64, particularly for quiet passages, where it offers a substantial improvement at a reasonable processing cost. This mode is therefore especially useful for recordings containing low-level material.

The library also supports direct DSD (.dsf) transcoding, with every combination of DSD64, DSD64 Enhanced, DSD128, DSD256 and DSD512 supported. To be clear, converting DSD64 to DSD512 does not improve it by creating information that was never recorded. It can, however, help standardize a music collection on a single format.

In my experience, some DACs labelled “Hi-Res” cannot handle levels above the 0 dB SACD reference level and produce distortion when that level is exceeded. This is disappointing when a purchased DSD recording does not sound as expected. I therefore added an option to reduce the output level so that it does not exceed 0 dBFS. The resulting file is better suited to DACs sensitive to levels above the reference. This reduction can be applied individually to each file, or a common attenuation can be selected for an album to preserve the relationship between its tracks. CrossDSD provides audio peak analysis to support this workflow.

CrossDSD can also convert 44.1 kHz PCM AIFF/AIFC files to 32-bit floating-point DXD at 352.8 kHz. This is particularly useful for working with CD-quality source files.

As an additional benefit, CrossDSD preserves compatible ID3v2 metadata from the source files. During DSF-to-DXD, DXD-to-DSF and DSF-to-DSF conversions, the high-level processing functions also record the encoding parameters, output file type and cumulative processing history. This makes it easier to identify a file’s origin and subsequent transformations without removing its existing musical information.

This is only a brief overview of the core features. Please explore the accompanying documentation for the full range of capabilities offered by CrossDSD.

## Technical overview

### An engine designed for integration

CrossDSD is written in C++20 and exposes a public C interface to simplify integration into software written in different languages. It can be built as a static or shared library using CMake.

Two levels of integration are available. High-level functions handle file conversions, including progress reporting, cancellation and processing reports. Block-processing interfaces allow the host application to supply and retrieve audio data directly, independently of files. They preserve filter and modulator state between calls to maintain signal continuity.

This design allows long recordings to be processed progressively without loading them entirely into memory. The command-line tool exposes the main operations and supports automated processing.

### Calculation precision and filtering

The CPU audio processing core uses double precision, or 64-bit floating point. Metal decoding and output DXD files use 32-bit floating point. This distinction maintains high precision during calculations while producing files suited to audio editing software.

DSD decoding uses a sequence of linear-phase digital filters and sample-rate reductions. The Standard profile attenuates ultrasonic content with a transition band from 20 to 40 kHz. The Raw profile retains a wider bandwidth, up to 150 kHz, while preserving the filtering required for conversion to 352.8 kHz.

Encoding follows the complementary path: filtering the PCM signal, increasing its sample rate, and applying sigma-delta modulation to produce the one-bit DSD stream. The modulators are tailored to each supported DSD rate.

DSD64 Enhanced is an encoding mode that produces an ordinary DSD64 file. Its additional processing is applied selectively, particularly in quiet passages, while maintaining modulator continuity when switching between processing modes.

### What DSD-to-DSD transcoding means

Transcoding between DSD rates does not create an intermediate PCM file on disk. It does, however, reconstruct the audio internally in 64-bit floating point before encoding it back to DSD.

This is signal processing, rather than a container-only change or a copy of the original bits. The architecture provides a consistent way to apply filtering, DSD rate changes and, when requested, level attenuation.

### Explicit level handling

The default level convention maintains continuity between DSD and PCM: a PCM value of 1.0 corresponds to the 0 dB SACD reference level. Floating-point representation preserves values above that level without clipping them in the DXD file.

When level reduction is requested, CrossDSD applies uniform attenuation. This preserves the programme’s dynamic variations and, when the same gain is used for several tracks, their relative levels.

Analysis measures the peaks of the reconstructed audio signal after filtering. Adaptive search examines candidate regions precisely, while exhaustive search reconstructs the entire programme. The resulting values describe this digital reconstruction; they are not measurements of the analogue output of individual DACs.

### Portability and validation

CrossDSD has a portable CPU engine. On Apple platforms, optional acceleration uses Accelerate/vDSP and, for file decoding, Metal. Windows builds use the CPU engine. Parallel channel processing can also be enabled. Since 1.1.0, the Metal file decoder maintains filter state between blocks and bounds its audio buffers independently of recording length. See the [1.1.0 validation results](docs/VALIDATION_1.1.0.md).

Processing choices are documented through automated tests, reference signals and measurement reports. These resources allow developers to examine algorithm behaviour, reproduce checks and verify changes to the library.

The function reference describes accepted formats, parameters, level conventions and the constraints of each interface.

## License

CrossDSD is distributed under the Apache License, Version 2.0. The complete text is available in [LICENSE](LICENSE).

Attribution is provided in [NOTICE.md](NOTICE.md). CrossDSD is supplied “AS IS”, without warranty, under Sections 7 and 8 of the license; see the [summary of the disclaimer of warranty and limitation of liability](DISCLAIMER.md).
