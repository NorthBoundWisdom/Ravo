# Source decomposition execution TODO

Status: active. Repository size ratchets and split-integrity checks are active;
production source debt remains to be removed.

## Invariants

- First-party Ravo `.cpp` and production `.qml` files have a hard 2,000-line
  limit. Registered debt may shrink but never grow, and its manifest entry is
  removed in the same change that brings the file within the limit.
- New split files target at most 1,500 lines; QML section components target at
  most 1,000 lines. Do not move implementation into headers or `.inc` files to
  evade the checks.
- Preserve target ownership, public APIs, schemas, JSON and command identifiers,
  lifecycle, thread boundaries, fallback behavior, and observable statement
  order unless a separate behavior change is explicitly approved.
- Test splits preserve ordered GoogleTest case identities, enabled state, and
  target membership. QML extraction preserves the `DevelopPanel` translation
  context.

## Execution queue

| Gate | Unfinished work | Acceptance gate |
| --- | --- | --- |
| G3 | Split the remaining command controller registry/dispatch and `DevelopPanel.qml` sections | Desktop C++ tests, QML contract/load tests, translation validation, and size checks pass |
| G4 | Split legacy XMP, SQLite catalog, and raster adapter implementations by responsibility | Adapter/catalog tests and size checks pass |
| G5 | Split image operations, RAW pipeline, and capability operations | Engine tests and size checks pass |
| G6 | Split CLI application, domain types, and catalog service | CLI/service/domain tests and size checks pass |
| G7 | Remove all debt-manifest entries and this TODO, then move any durable rule changes into their stable authorities | No first-party `.cpp` or production `.qml` exceeds 2,000 lines; full Ravo suite passes |

## Validation commands

```text
python3 configs/check_translation_unit_size.py
python3 configs/check_qml_file_size.py
python3 configs/check_test_split_inventory.py
cmake --preset mac_clang_debug -DBUILD_TESTING=ON
cmake --build --preset mac_clang_debug --target RavoCodeQuality
ctest --test-dir build/mac_clang_debug --output-on-failure
```

Before G7 acceptance, configure/build/test each available Windows, macOS, and
Linux toolchain. Unavailable platforms remain explicitly untested.
