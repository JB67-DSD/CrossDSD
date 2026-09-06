# Public distribution validation — September 5, 2026

## Sources and build

- Release build with CMake from the `Github` directory alone, without application sources.
- Universal `arm64`/`x86_64` macOS SDK, targeting macOS 13, with Metal and channel parallelism enabled.
- CTest suite: **19 out of 19 tests passed**, including corpus-based tests.
- SDK installation followed by separate builds of the C decoding and encoding examples.
- Both examples executed on the synthetic B004 signal: DSF-to-WAV decoding, then WAV-to-DSD64 encoding succeeded.
- Standalone CMake package-consumer test built and executed.
- Executable and library architectures verified with `lipo`; the Intel `--version` command executed under Rosetta.

Preparation also fixes an older SDK-consumer test: its DSP and conversion API version numbers were outdated, and its static-library target requires the C++ linker.

## Windows

The 34 engine and API files in the Windows RC1 source kit match the current working sources after line-ending normalization. The public sources add Apache headers without changing those algorithms.

The six binary files in each SDK (executables, DLL and libraries) are preserved byte for byte during repackaging. The original Windows logs record successful validation on August 25, 2026. No new Windows execution was performed for this preparation.

## Corpus

- 217 audio files present.
- 253 entries in the SHA-256 index verified without errors during initial preparation.
- 254 archive files compared with the extracted directory without differences during initial preparation.

The supplied scientific results are those of the existing corpus. Full scientific analyses were not recalculated during this preparation.

## English documentation update

French documentation and author labels were translated into English. The reference charts and their generator now use English labels; chart geometry, numerical results and all audio files are unchanged. The corpus checksum index and distribution archives were updated accordingly.

A contradictory cancellation instruction in the API guide was corrected against the public header, implementation and example: return non-zero from the progress callback to continue, or zero to cancel.

This documentation update does not change executable code or rebuild SDK binaries. Archive contents, links and checksums are checked again after packaging.

## Scope

No graphical application, private music recording or obsolete evaluation kit is included in the public repository. SDK archives contain the Apache 2.0 notices.
