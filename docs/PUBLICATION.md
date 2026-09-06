# Prepare the GitHub publication

The public repository is [JB67-DSD/CrossDSD](https://github.com/JB67-DSD/CrossDSD). Source files are versioned in Git; binary and audio archives are attached to GitHub Releases. This guide describes how to maintain that distribution.

## Source repository

Version the source code, CMake files, tests, examples, documentation, licenses, and corpus tools and manifests. The supplied `.gitignore` excludes local builds, distribution archives and large audio files. These files remain present in the local preparation directory.

## Separate downloads

Attach the five archives listed in [releases/README.md](../releases/README.md) to a Release, together with `releases/SHA256SUMS.txt`. For future versions, update the download links to the corresponding release tag.

Sources can also be obtained from the supplied standalone source archive or from GitHub-generated source archives. The audio corpus remains a separate download to extract into `reference-signals/`.

This arrangement avoids adding approximately 2.5 GB of audio to Git history. GitHub recommends Releases for large binary distributions and blocks files larger than 100 MiB in regular Git: see the [GitHub documentation](https://docs.github.com/en/repositories/working-with-files/managing-large-files/about-large-files-on-github).

## Version

The engine and SDK version remains **1.0.0 RC1**. RC2 fixes concern the graphical applications, which are not included in this repository. The `Apache-2.0` suffix identifies the public archive preparation and does not indicate a new audio engine.

## Reproduce an SDK

Build and install the library as described in the [developer guide](GETTING_STARTED.md), then archive the installation directory with its license and attribution notices. Examples and additional documentation from the repository can be added to the kit. Recalculate SHA-256 checksums whenever an archive changes.

The Windows distributions prepared here reuse previously validated binaries. Their exact provenance and the limits of this verification are described inside each archive. Future changes to the engine will require a new build and validation on each platform.

## Documentation language

English is the single documentation language for the public distribution. Keep repository documents, SDK documents and chart labels consistent when updating a release. Preserve names, public API identifiers and the official Apache license text.
