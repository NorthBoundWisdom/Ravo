# Windows and Linux release evidence TODO

> **Status:** independent host-evidence queue; does not block other product execution
>
> **Updated:** 2026-09-01

This file owns Windows and Linux Debug/Release verification and the
three-platform same-commit closeout. Product execution, private-corpus
latency, and Gallery measurement remain in [TODO.md](TODO.md). Current
behavior belongs in `Ravo/README.md`, [ARCHITECTURE.md](ARCHITECTURE.md),
[TESTING.md](TESTING.md), code, and tests.

A macOS result is not evidence here. Do not describe a skip as a pass.

## WL-01 — Windows Debug and Release loop

**Dependency:** a Windows host with the current source-root lock materialized and
the supported Qt/MSVC toolchain.

```text
python configs/source_roots.py show --format json
python configs/source_roots.py resolve --format json
python configs/source_roots.py verify
python3 Ravo/tools/freeze_legacy_manifest.py --check
python3 Ravo/tools/check_ravo_dependency_boundary.py
cmake --preset win_msvc_debug -DBUILD_TESTING=ON
cmake --build --preset win_msvc_debug
ctest --test-dir build/win_msvc_debug --output-on-failure
cmake --preset win_msvc_release -DBUILD_TESTING=ON
cmake --build --preset win_msvc_release
ctest --test-dir build/win_msvc_release --output-on-failure
```

**Acceptance gate:** Debug and Release cover restore publication/cleanup,
cancellable shutdown, 10,000-row paging, deterministic import, verified
retention, schema migration/relink rollback, translations, QML smoke, and the
staged installed-product create/import/reopen/recovery loop without leaked
temporary files, locked catalogs, build-tree dependencies, or hidden platform
fallback.

## WL-02 — Linux Debug and Release loop

**Dependency:** a Linux host with the current source-root lock materialized and
the supported Qt/Clang toolchain.

```text
python configs/source_roots.py show --format json
python configs/source_roots.py resolve --format json
python configs/source_roots.py verify
python3 Ravo/tools/freeze_legacy_manifest.py --check
python3 Ravo/tools/check_ravo_dependency_boundary.py
cmake --preset linux_clang_debug -DBUILD_TESTING=ON
cmake --build --preset linux_clang_debug
ctest --test-dir build/linux_clang_debug --output-on-failure
cmake --preset linux_clang_release -DBUILD_TESTING=ON
cmake --build --preset linux_clang_release
ctest --test-dir build/linux_clang_release --output-on-failure
```

**Acceptance gate:** the same behavior and installed-product loop as Windows
passes without a Linux-only fallback.

## WL-03 — Three-platform same-commit closeout

**Dependency:** WL-01, WL-02, and the current macOS results from [TODO.md](TODO.md)
on the same commit and dependency pins.

- Confirm macOS, Windows, and Linux results target that commit.
- Confirm package contents, version metadata, checksums, generated third-party
  notices, translations, settings/support directories, and installed launch do
  not depend on the build tree.
- Record only durable platform constraints in stable authorities; keep run logs,
  private corpus details, and transient screenshots outside the repository.

**Acceptance gate:** do not label the commit release-ready until WL-01, WL-02,
and this closeout are current. A failed or skipped host remains open.
