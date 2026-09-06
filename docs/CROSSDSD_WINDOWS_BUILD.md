# Build CrossDSD on Windows

## Prerequisites

Windows 10/11 x64 or Windows 11 ARM64, Visual Studio 2022 with Desktop development with C++, the required x64/ARM64 compilers, and CMake 3.25 or later for the supplied presets.

## Build from this repository

Open a Visual Studio developer terminal at the repository root.

### x64

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64-release
ctest --preset windows-x64-release
cmake --install build/windows-x64 --config Release --prefix install/windows-x64
```

### ARM64

```powershell
cmake --preset windows-arm64
cmake --build --preset windows-arm64-release
ctest --preset windows-arm64-release
cmake --install build/windows-arm64 --config Release --prefix install/windows-arm64
```

Run tests on a machine capable of executing binaries for the target architecture. Cross-compiling ARM64 from x64 does not mean that ARM64 tests can run on that machine.

These presets disable Metal and Accelerate and enable channel parallelism. The audio corpus is optional for standalone unit tests. To enable audio-based tests, see the [getting started guide](GETTING_STARTED.md).

## Use the prebuilt SDKs

The archives in [the GitHub Release](https://github.com/JB67-DSD/CrossDSD/releases/tag/v1.0.0-rc1) contain `bin/crossdsd.exe`, the DLL, static and import libraries, headers and CMake integration files.

```powershell
.\bin\crossdsd.exe --version
```

Configure a host application with `-DCMAKE_PREFIX_PATH` pointing to the extracted SDK directory. With a Visual Studio generator, specify `--config Release` when building. See the [examples](../examples/README.md).

The supplied RC1 binaries were validated on Windows on August 25, 2026. Preparation for this repository updates documentation and license notices without rebuilding or modifying the executables; see the [distribution provenance](../releases/README.md).
