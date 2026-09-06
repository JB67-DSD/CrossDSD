# Build and integrate CrossDSD 1.0.0 RC1

This repository contains the library and its command-line tools. The engine is version 1.0.0 RC1. Graphical applications are distributed separately.

## Prerequisites

- A C++20 compiler and a C compiler.
- CMake 3.20 or later for a regular build; CMake 3.25 or later for the supplied presets (format version 6).
- macOS: Xcode development tools; the supplied universal SDK targets macOS 13 or later.
- Windows: Visual Studio 2022 with C++ tools for the target architecture. See the [Windows guide](CROSSDSD_WINDOWS_BUILD.md).
- Python 3 for the command-line integration test using the corpus. NumPy, SciPy and FFmpeg are not needed to build the library; they are used for the corpus’s scientific analyses.

No third-party audio library is downloaded during the build. Metal and Accelerate acceleration use Apple system frameworks.

## Build on macOS

From the repository root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build --prefix "$PWD/install"
```

To produce a universal SDK for Apple Silicon and Intel, add these configuration options:

```sh
-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0
```

The main executable is `build/crossdsd`. The installation contains `bin/`, `include/crossdsd/`, `lib/` and documentation under `share/doc/CrossDSD/`.

## Build options

| Option | Default | Effect |
| --- | --- | --- |
| `CROSSDSD_BUILD_SHARED` | `ON` | Also builds the shared library; the static library is always built. |
| `CROSSDSD_BUILD_TESTS` | `ON` | Builds tests and registers them with CTest. |
| `CROSSDSD_ENABLE_METAL` | `ON` on Apple | Accelerates file decoding with Metal. |
| `CROSSDSD_USE_ACCELERATE` | `OFF` | Uses Accelerate/vDSP on Apple. |
| `CROSSDSD_ENABLE_CHANNEL_THREADS` | `OFF` | Enables channel parallelism. |

`CROSSDSD_BUILD_METAL_TOOLS`, `CROSSDSD_BUILD_PHASE7_TOOLS` and `CROSSDSD_BUILD_PHASE8_TOOLS` enable experimental tools retained for algorithm research. They are disabled by default.

## Test with the reference signals

Standalone unit tests are available without downloading audio. For the transcoding and command-line tests, install the complete corpus as described in [reference-signals/README.md](../reference-signals/README.md), then configure and build again.

To use a corpus stored elsewhere:

```sh
cmake -S . -B build -DCROSSDSD_REFERENCE_SIGNALS_DIR=/path/to/CrossDSD_Reference_Signals_v0.2
```

Tests that use the corpus are registered only when the required audio test files are present. Full scientific analyses are separate from CTest and documented in the corpus.

## Integrate with CMake

After installing the SDK:

```cmake
cmake_minimum_required(VERSION 3.20)
project(MyIntegration LANGUAGES C CXX)
find_package(CrossDSD 1.0 REQUIRED)
add_executable(my_tool main.c)
target_link_libraries(my_tool PRIVATE CrossDSD::formats-static)
set_property(TARGET my_tool PROPERTY LINKER_LANGUAGE CXX)
```

Configure this project with `-DCMAKE_PREFIX_PATH=/path/to/sdk`. The `CrossDSD::formats` target selects the shared library when available. On Windows, its DLL must be accessible at runtime, for example beside the executable. The static target propagates `CROSSDSD_STATIC` and the system dependencies to the host project.

## Ready-to-build examples

From the repository root, after installation:

```sh
cmake -S examples/twistedwave -B build-example-decode -DCMAKE_PREFIX_PATH="$PWD/install" -DCMAKE_BUILD_TYPE=Release
cmake --build build-example-decode
cmake -S examples/twistedwave-encoder -B build-example-encode -DCMAKE_PREFIX_PATH="$PWD/install" -DCMAKE_BUILD_TYPE=Release
cmake --build build-example-encode
```

Usage:

```sh
./build-example-decode/crossdsd-file-example input.dsf output.wav
./build-example-encode/crossdsd-export-example input.wav output.dsf 64
```

The third example, `examples/twistedwave-windows-decoder`, demonstrates the same decoding operation in a Windows context. These examples use the public API and do not depend on TwistedWave.

Continue with the [API guide](API_GUIDE.md), [command-line reference](CROSSDSD_COMMAND_LINE_REFERENCE.md) and [error model](ERRORS.md).
