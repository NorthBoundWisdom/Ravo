# Ravo

[![CI](https://github.com/NorthBoundWisdom/Ravo/actions/workflows/ci.yml/badge.svg)](https://github.com/NorthBoundWisdom/Ravo/actions/workflows/ci.yml)

> A photo library and RAW editor rebuilt for local photography workflows.

Ravo is a cross-platform local photo library and RAW editor rebuilt from the photo-workflow and image-processing implementation of **darktable 0.9**. The frozen darktable source and test fixtures are read-only behavioral evidence; Ravo is a new C++20 and Qt 6 implementation with its own service layer, not a wrapper around the former GTK application. It neither depends on nor launches the old application at runtime.

Ravo Studio brings the library, browsing, filtering, review, and non-destructive editing into one local workspace. The `ravo` CLI exposes the same core capabilities to automated and headless workflows.

## Ravo Studio

![Ravo Studio gallery view showing a local photo grid, histogram, photo details, and metadata panel.](assets/screenshots/ravo-studio-gallery.png)

*Gallery: browse JPEG and RAW photos, then inspect metadata, ratings, tags, and histograms.*

![Ravo Studio edit view showing a single photo with non-destructive White Balance and Color Calibration controls.](assets/screenshots/ravo-studio-edit.png)

*Edit: inspect a photo and adjust Develop parameters without changing the original.*

## Built for photography

- **Original-safe by default:** reference-only import never copies, moves, renames, or rewrites source files during import or editing.
- **A local library:** a portable SQLite catalog stores photos, ratings, color labels, rejection state, tags, metadata, and editing history.
- **Focused browsing:** gallery grid and loupe views, Fit / Fill / 100% zoom, filtering and sorting, histograms, and RGB parade.
- **Non-destructive Develop:** edits are versioned recipes; previews, CLI commands, and exports use the same CPU image engine.
- **RAW-oriented:** JPEG, PNG, TIFF, and LibRaw-supported RAW files are accepted. The RAW path uses the Sigmoid Standard SDR display transform.
- **Predictable export:** export JPEG, PNG, TIFF, or the original file, with explicit output-conflict and cancellation results.

## Current capabilities and scope

Ravo is actively developed and already supports the core loop of creating or opening a local library, reference-only import, browsing and review, non-destructive Develop, and local export. Current editing operations include RAW highlight reconstruction, hot-pixel and chromatic-aberration correction, denoising, lens correction, white balance, color calibration, Color Balance RGB, a graduated filter, tone curves, and tone equalizer.

Ravo is not yet a complete darktable replacement. Full ICC and metadata export, general masks, additional image operations, and end-to-end installation verification on every platform are still being migrated and validated. [The Ravo product document](Ravo/README.md) and the [active migration TODO](TODO_LEGACY_MIGRATION.md) define the supported scope, validation status, and next work item.

## The rewrite path

The frozen darktable 0.9 copy in [`legacy/`](legacy/README.md) is used only for static source reading and fixture validation. Ravo production code does not include old private headers, link old libraries, load old plugins, or configure, build, or run the former application. Its Engine, CLI, and Studio are one independent implementation.

This path retains verified photography behavior while removing the former GTK UI, dynamic plugin ABI, global state, and OpenCL path in favor of a clearer C++20 and Qt 6 architecture.

## Build from source

Prepare a workspace for the first time:

```text
git submodule update --init FreeCM
python3 configs/source_root_workflow.py --init
python3 configs/source_root_workflow.py --update
```

Build and launch the macOS Debug configuration:

```text
cmake --preset mac_clang_debug -DBUILD_TESTING=ON
cmake --build --preset mac_clang_debug
./build/mac_clang_debug/Ravo/desktop/ravo_studio.app/Contents/MacOS/ravo_studio
```

Use `win_msvc_debug` / `win_msvc_release` on Windows and `linux_clang_*` on Linux. [The Ravo developer guide](Ravo/README.md) and [Packaging](DevDocs/Packaging.md) describe complete build, test, package, and platform instructions.

## Learn more

- [User handbook](userdoc/README.md) · [Read online](https://northboundwisdom.github.io/Ravo/)
- [Ravo capabilities and CLI](Ravo/README.md)
- [Architecture and data lifetimes](Ravo/ARCHITECTURE.md)
- [Testing and validation strategy](Ravo/TESTING.md)
- [Migration policy and capability ledger](Ravo/MIGRATION.md)
- [Active migration work](TODO_LEGACY_MIGRATION.md)
- [Developer documentation index](DevDocs/README.md)
- [Frozen darktable 0.9 reference tree](legacy/README.md)

## License

Ravo is distributed under the [GNU General Public License, version 3](LICENSE).
