# Gallery Browse Performance TODO

> **Status: Release measurement and broader candidates pending**
>
> **Updated: 2026-08-31**

This queue covers unfinished Gallery-to-viewer latency work. It does not relax
catalog publication, preview colour, cancellation, source-safety, or GPU
admission contracts.

## 1. Release measurement before broader concurrency

Dependency: an explicit read-only mixed RAW/raster corpus and a Release build.

- Measure cold and warm folder enumeration/import, first thumbnail, viewport
  completion, placeholder publication, exact 1600-edge publication, and
  adjacent-photo revisit over two warmups plus at least eight samples.
- Record P50/P90/max, cache state, source kind, file count, host, storage kind,
  worker count, peak owned bytes, and source SHA-256/size/mtime preservation.
- Add a bounded browse worker pool only if the same-corpus end-to-end P90
  improves without foreground, cancellation, memory, or HDD-seek regression.
- Evaluate a profiled medium browse resource, byte-bounded adjacent-preview
  LRU, deferred optional capture metadata, and a non-PNG browse encoding as
  separate measured candidates.

Suggested validation:

```text
cmake --preset mac_clang_debug -DBUILD_TESTING=ON
cmake --build --preset mac_clang_debug --target ravo_desktop_command_tests ravo_studio
ctest --test-dir build/mac_clang_debug --output-on-failure -R 'StudioPresenterTest|StudioQmlContract|ravo_studio_qml_smoke'
cmake --preset mac_clang_release -DBUILD_TESTING=ON
cmake --build --preset mac_clang_release --target ravo_desktop_command_tests ravo_studio
```

## Risks and exclusions

- Browse placeholders and embedded JPEGs are presentation resources, never RAW
  correctness references or silent Develop fallbacks.
- Parallel RAW thumbnails can oversubscribe Engine row workers and multiply
  large float buffers; an item-count limit is not an acceptable memory bound.
- Direct RapidRAW source reuse requires separate licence review. Ravo
  implementation remains C++20/Qt and follows its own service contracts.
- GPU work remains governed exclusively by `DevDocs/GPU_Baseline.md`.
