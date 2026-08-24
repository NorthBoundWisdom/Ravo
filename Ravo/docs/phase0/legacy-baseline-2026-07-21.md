# Legacy Baseline Report: 2026-07-21

## Scope and host

This report was produced before Ravo source targets existed. It is retained as
historical evidence only. The old project is now statically frozen: none of the
commands or experiments below may be repeated, extended, or used as a
prerequisite for Ravo. Ravo does not configure, compile, execute, test, or
package the old project.

| Field | Value |
| --- | --- |
| Host | Windows x64 |
| Frozen source/fixture commit | `320970bf7c9cbbc6611cfc3eb60f8f2b0424b782` (“Freeze 0.9 and prepare the Ravo rewrite”) |
| Frozen `src` tree | `a3ac761ecbb0cf668ecad49aff8bd0e29235f5f7` |
| Frozen `legacy/tests` tree | `1dc38893f39e113620aebbbdc927218ca4a2b8af` |
| CMake | 4.4.0 |
| Generator/preset | Ninja / `win_msvc_release` |
| Compiler | MSVC 19.51.36248.0, x64 |
| Legacy image backend | CPU requested with `--disable-opencl` |
| Image fixtures | 158 XMP files, 158 `expected.png` files, 5 source images |
| Legacy XMP schema | 6 |
| Covered operation names | 68 |

The source and fixture tree IDs match the current committed HEAD; no `src` or
`legacy/tests` path changed after the freeze commit. The freeze checker also
guards the legacy root CMake graph, `cmake/`, `data/`, and `packaging/` against
working-tree or committed drift. The fixture hashes and exact paths are in
[`../../tests/fixtures/legacy_manifest.json`](../../tests/fixtures/legacy_manifest.json).
The manifest is the authoritative static inventory. The stored PNGs are frozen
reference inputs; policy does not require or permit reproducing them with an
old executable on this or any other toolchain.

Before a Ravo change or a baseline attempt, verify the committed and working
trees without writing assets:

```powershell
python Ravo/tools/check_freeze_reference.py
```

## Historical legacy CTest result

The historical CTest targets were initially absent because the existing build
used `BUILD_TESTING=OFF` and did not have CMocka. After the workspace CMocka
installation, the following commands completed without changing expected-image
files. The build also contained investigative Windows source changes described
below, so this result is diagnostic evidence rather than certification of the
frozen 0.9 source. These commands are recorded for provenance and must not be
run again:

```powershell
cmake --preset win_msvc_release -DBUILD_TESTING=ON
cmake --build --preset win_msvc_release --target darktable-test-variables test_sample test_database_schema
ctest --test-dir build/win_msvc_release --output-on-failure -L unit
```

Result: 3/3 passed in 0.15 seconds (`darktable-test-variables`, `test_sample`,
and `test_database_schema`).  The passing empty sample remains a legacy test
asset only; Ravo will not copy it as evidence of engine behavior.

## Historical image-regression result

Before the freeze policy was finalized, the legacy image runner was invoked in
CPU-only, no-write, fast-fail mode. The invocation is intentionally not
preserved as a runnable command because Windows Ravo tooling is limited to
Python and PowerShell and old execution is now prohibited.

The initial unchanged Windows run stopped at `0000-nop` before rendering
because no IOP module loaded. An investigative working-tree build then gave
first-party `MODULE` targets the `lib` prefix expected by GLib, exported module
API functions, and moved runtime DLLs out of plugin scan directories.
`dumpbin /exports libagx.dll` reported `dt_module_dt_version`,
`dt_module_mod_version`, and IOP API exports such as `name`; that build loaded
the IOP registry without reporting third-party DLLs as failed modules.

The investigative build also used the pinned FreeCM Exiv2 source root with
`EXIV2_ENABLE_XMP=ON` because the installed vcpkg Exiv2 lacked XMP support. Its
configure/build and all three legacy unit tests succeeded.

The direct `0000-nop` invocation now reaches legacy processing and stops before
PNG output because the LensFun database directory is absent at
`build/win_msvc_release/share/lensfun/version_1`. Existing history diagnostics
also report unsupported blend parameters. The local vcpkg LensFun package
contains the DLL, headers, and metadata but no XML database. NumPy is absent,
so `legacy/tests/count-diff-pixels` returns its fallback value of `50000`.
Neither condition is an image result or a valid reason to change an expected
PNG. Under [ADR-0004](../adr/0004-freeze-09-ravo-only-growth.md) these source
experiments are not a new 0.9 implementation plan and must not be extended to
repair LensFun, history, module loading, or packaging. Ravo proceeds from the
committed fixture hashes and produces only Ravo-owned acceptance evidence.

No expected PNG, XMP, threshold, or source image was written.  The resulting
classification is:

| Run set | Result | Reason |
| --- | --- | --- |
| Legacy CTest unit label | Diagnostic pass, 3/3 | Investigative Windows build; not frozen 0.9 certification |
| `legacy/tests` CPU fixture 0000 | Frozen-oracle result unavailable | Unchanged build failed module loading; investigative build then lacked LensFun XML and emitted blend diagnostics |
| Remaining 157 image fixtures | Not run | Fast-fail stopped after fixture 0000 |
| OpenCL comparison | Not run | CPU-only baseline command and unavailable runner |

## Required Ravo follow-up

1. Validate the frozen commit, tree IDs, and complete fixture manifest with the
   Python freeze checks; do not run any old target or runner.
2. Keep checked-in RAW/XMP/PNG assets immutable and classify product disposition
   separately from asset presence.
3. Add Ravo-owned canonical recipe, float output, metadata, image-comparison,
   recovery, and performance evidence for the first vertical slice.
4. Never fix, configure, compile, or execute the old LensFun/blend path as a
   prerequisite for Ravo implementation.
