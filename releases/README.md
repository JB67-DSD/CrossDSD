# CrossDSD library distributions

The archives below contain only the library, its command-line tools and developer resources.

## 1.1.0

- [Universal macOS SDK](https://github.com/JB67-DSD/CrossDSD/releases/download/v1.1.0/CrossDSD-macOS-SDK-1.1.0-universal-Apache-2.0.zip) for macOS 13 or later, with Apple Silicon and x86_64 slices.
- [Standalone source archive](https://github.com/JB67-DSD/CrossDSD/releases/download/v1.1.0/CrossDSD-Source-1.1.0-Apache-2.0.tar.gz) for building on macOS or Windows.
- [SHA-256 checksums](https://github.com/JB67-DSD/CrossDSD/releases/download/v1.1.0/SHA256SUMS.txt) and [validation results](../docs/VALIDATION_1.1.0.md).

## 1.0.0 RC1 archives

| Platform | Archive | Contents |
| --- | --- | --- |
| macOS 13+, Apple Silicon and Intel | [Universal SDK](https://github.com/JB67-DSD/CrossDSD/releases/download/v1.0.0-rc1/CrossDSD-macOS-SDK-1.0.0-RC1-universal-Apache-2.0.zip) | `crossdsd` command, tools, static and shared libraries, headers, CMake, examples and documentation |
| Windows x64 | [x64 SDK](https://github.com/JB67-DSD/CrossDSD/releases/download/v1.0.0-rc1/CrossDSD-Windows-SDK-1.0.0-RC1-x64-Apache-2.0.zip) | Executables, DLL, libraries and integration kit |
| Windows ARM64 | [ARM64 SDK](https://github.com/JB67-DSD/CrossDSD/releases/download/v1.0.0-rc1/CrossDSD-Windows-SDK-1.0.0-RC1-arm64-Apache-2.0.zip) | Equivalent contents for ARM64 |
| All platforms | [Reference corpus v0.2](https://github.com/JB67-DSD/CrossDSD/releases/download/v1.0.0-rc1/CrossDSD_Reference_Signals_v0.2.zip) | 217 audio files, manifests, tools and reference results |
| All platforms | [Library sources](https://github.com/JB67-DSD/CrossDSD/releases/download/v1.0.0-rc1/CrossDSD-Source-1.0.0-RC1-Apache-2.0.tar.gz) | Sources, build files, tests, examples, documentation and corpus description; audio and SDKs are separate downloads |

Download the archives from the [CrossDSD 1.0.0 RC1 release](https://github.com/JB67-DSD/CrossDSD/releases/tag/v1.0.0-rc1) in this repository. GitHub hosts both the source code and all release assets.

## RC1 provenance and validation

The macOS SDK was rebuilt on September 5, 2026 from the public source directory. It contains `arm64` and `x86_64` architectures, with a minimum target of macOS 13, Metal enabled and channel parallelism enabled. All 19 tests passed on Apple Silicon. Conversion examples were also built and executed. The Intel binary responds to `--version` under Rosetta; it has not been validated on a physical Intel Mac.

The Windows SDKs come from the RC1 archives built and validated on August 25, 2026 for x64 and ARM64. Their executables, DLL and libraries are unchanged. Repackaging adds Apache 2.0 and updates documentation, examples and header comments. Each archive contains `BUILD-INFO.txt` and `REPACKAGING.md`. Windows tests have not been rerun from this Mac.

The v0.2 corpus audio and numerical results are unchanged. During initial preparation, its 253-entry checksum index passed verification, and all 254 files in the archive matched the extracted corpus. The subsequent English documentation update translated chart labels and their generator, then updated the corpus checksum index and archive.

Repository and SDK documentation are provided in English only. The documentation update does not modify the SDK binaries.

## RC1 integrity

Download [SHA256SUMS.txt](https://github.com/JB67-DSD/CrossDSD/releases/download/v1.0.0-rc1/SHA256SUMS.txt) and the five archives into the same directory, then run:

```sh
shasum -a 256 -c SHA256SUMS.txt
```

In PowerShell, use `Get-FileHash -Algorithm SHA256` on the selected archive and compare its result with `SHA256SUMS.txt`. Each SDK also has its own checksum index for the extracted files.

## Installation and build

Extract the SDK for the selected architecture and read its README. To rebuild from source, see [GETTING_STARTED.md](../docs/GETTING_STARTED.md). For the corpus, see [reference-signals/README.md](../reference-signals/README.md).
