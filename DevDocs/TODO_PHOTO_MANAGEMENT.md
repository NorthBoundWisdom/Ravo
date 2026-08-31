# Product P0/P1 Release Evidence TODO

> **Status: implementation complete; release evidence pending**
>
> **Updated: 2026-08-31**

ADR-0099–0101 and the current capability, architecture, migration, and testing
documents own the implemented P0/P1 behavior. This TODO contains only evidence
that cannot be completed on the current macOS host or without a private photo
corpus. It must be deleted, not archived, when every gate below is current.

## 1. Private real-photo Release evidence

Dependency: an explicit read-only mixed RAW/raster tree in
`RAVO_PHOTO_CORPUS`; generated catalogs, previews, recovery files, backups, and
reports must remain under a unique temporary root.

Run on the release candidate host:

```text
RAVO_PHOTO_CORPUS=/absolute/private/photos \
  build/mac_clang_release/Ravo/tests/ravo_catalog_tests \
  --gtest_filter=CatalogServiceTest.PrivatePhotoManagementReleaseProbePreservesCorpus
RAVO_INTERACTIVE_PERF_CATALOG=/temporary/private-corpus/library.sqlite \
RAVO_INTERACTIVE_PERF_ASSET_ID=<imported-raw-asset-id> \
RAVO_INTERACTIVE_PERF_P90_BUDGET_MS=30 \
  build/mac_clang_release/Ravo/tests/ravo_desktop_command_tests \
  --gtest_filter=StudioInteractivePreviewPerformanceProbe.MeasuresExposureIntentThroughImagePublication
```

Acceptance gate:

- record RAW/raster import, cold/warm settled preview, page, and interactive
  P50/P90/max using the definitions in `DevDocs/TESTING.md`;
- freeze host-local budgets only from a repeatable candidate run, then rerun
  with those budget variables enabled;
- prove every corpus file retains exact SHA-256, size, and modification time;
- retain reports outside the repository and do not generalize one host result
  to another OS/toolchain.

Risk: `RAVO_PHOTO_CORPUS` is not available in this workspace, so an unset probe
is skipped and is not release evidence.

## 2. Windows release loop

Dependency: a Windows host with the current source-root lock materialized and
the supported Qt/MSVC toolchain.

```text
python configs/source_roots.py show --format json
python configs/source_roots.py resolve --format json
python configs/source_roots.py verify
python3 Ravo/tools/freeze_legacy_manifest.py --check
python3 Ravo/tools/check_capability_inventory.py
python3 Ravo/tools/check_freeze_reference.py
python3 Ravo/tools/check_ravo_dependency_boundary.py
cmake --preset win_msvc_debug -DBUILD_TESTING=ON
cmake --build --preset win_msvc_debug
ctest --test-dir build/win_msvc_debug --output-on-failure
cmake --preset win_msvc_release -DBUILD_TESTING=ON
cmake --build --preset win_msvc_release
ctest --test-dir build/win_msvc_release --output-on-failure
```

Acceptance gate: Debug and Release cover restore publication/cleanup,
cancellable shutdown, 10,000-row paging, deterministic import, verified
retention, schema-v9 migration/relink rollback, translations, and the staged
installed-product create/import/reopen/recovery loop without leaked temporary
files or locked catalogs.

## 3. Linux release loop

Dependency: a Linux host with the current source-root lock materialized and the
supported Qt/Clang toolchain.

```text
python configs/source_roots.py show --format json
python configs/source_roots.py resolve --format json
python configs/source_roots.py verify
python3 Ravo/tools/freeze_legacy_manifest.py --check
python3 Ravo/tools/check_capability_inventory.py
python3 Ravo/tools/check_freeze_reference.py
python3 Ravo/tools/check_ravo_dependency_boundary.py
cmake --preset linux_clang_debug -DBUILD_TESTING=ON
cmake --build --preset linux_clang_debug
ctest --test-dir build/linux_clang_debug --output-on-failure
cmake --preset linux_clang_release -DBUILD_TESTING=ON
cmake --build --preset linux_clang_release
ctest --test-dir build/linux_clang_release --output-on-failure
```

Acceptance gate: the same behavior and installed-product loop as Windows pass
without platform fallback. A macOS result is not evidence for either gate.

## 4. Final closeout

- Confirm macOS, Windows, and Linux results all target the same commit and
  current dependency pins.
- Update only stable authorities with any durable platform constraint; do not
  append per-run logs or completed checklists.
- Delete this file and every tracked reference to it in the same change, then
  explicitly resume the first Ready `MR*` item in `TODO_LEGACY_MIGRATION.md`.
