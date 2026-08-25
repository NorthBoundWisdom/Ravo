# Ravo

[![CI](https://github.com/NorthBoundWisdom/Ravo/actions/workflows/ci.yml/badge.svg)](https://github.com/NorthBoundWisdom/Ravo/actions/workflows/ci.yml)

Ravo is a cross-platform photo library and RAW-oriented editor. The current
product slice creates a local SQLite catalog, imports JPEG/PNG/TIFF/RAW by
reference, and browses them in Ravo Studio.

Frozen Darktable 0.9 lives in [`legacy/`](legacy/README.md) as read-only
reference. It is not configured, compiled, run, or tested.

## Build

```text
git submodule update --init FreeCM
python3 configs/source_root_workflow.py --init
python3 configs/source_root_workflow.py --update

cmake --preset mac_clang_debug -DBUILD_TESTING=ON
cmake --build --preset mac_clang_debug
```

Commit hooks format staged `Ravo/` C/C++ (and QML/JS when `qmlformat` is
configured). Create a local `hooks/path.ini` from the sample and install:

```text
cp hooks/path.ini.sample hooks/path.ini
python3 hooks/install.py
```

See [`hooks/README.md`](hooks/README.md).

Windows uses `win_msvc_debug` / `win_msvc_release`. Linux configure presets are
`linux_clang_*`; build those trees with `cmake --build build/linux_clang_debug`
because the generated Linux build preset is named `ClangDebug`.
FreeCM Config/Build/Run/Test/Package call the same CMake targets.

Release package:

```text
cmake --preset mac_clang_release
cmake --build build/mac_clang_release --target RavoPackage
```

Use `win_msvc_release` or `linux_clang_release` on the corresponding host.
Artifacts use `RavoStudio-<version>-<architecture>-<platform>` names: macOS
produces a DMG, Windows a ZIP, and Linux an AppImage plus DEB. See
[`DevDocs/Packaging.md`](DevDocs/Packaging.md).

The FreeCM Package button follows the active Config. Debug and Release each
have a compatible package variant; run the selected Config once before using
its Package action. Release remains the distribution configuration.

## Continuous integration

GitHub Actions (`.github/workflows/ci.yml`) follows the same FreeCM path as a
local machine: `--init` creates the ignored `source_roots.lock.jsonc` from the
template, CI then rewrites that lock's Qt/host `CMAKE_PREFIX_PATH`,
`RAVO_PACKAGE_RUNTIME_SEARCH_PATHS`, and `cmakeEnvironment.PATH`, and `--update`
generates presets. Configure uses
`cmake --preset *-debug -DBUILD_TESTING=ON`. CI then builds
`build/<preset>` so the runner PATH (including Qt) is visible to Windows
gtest discovery; Linux also cannot use `cmake --build --preset linux_clang_debug`
because the generated build preset is named `ClangDebug`. Frozen `legacy/` is
not configured or built. Branch pushes and pull requests stop after the Debug
build/tests. Tag pushes then run separate Release jobs that call `RavoPackage`,
upload the FreeCM-produced platform artifacts, and publish one GitHub Release
with the macOS DMG, Windows ZIP, Linux AppImage, and Linux DEB attached.

GeoControls is public today; if it becomes private, add a `GEOCONTROLS_TOKEN`
repository secret with `contents:read`.

Studio after a macOS Debug build:

```text
./build/mac_clang_debug/Ravo/desktop/ravo_studio.app/Contents/MacOS/ravo_studio
```

## Layout

| Path | Purpose |
| --- | --- |
| `Ravo/` | Engine, CLI, catalog services, and Studio |
| `configs/` | FreeCM source-root lock and workflow |
| `hooks/` | Host installer for FreeCM commit hooks |
| `DevDocs/` | Live Ravo developer docs |
| `legacy/` | Frozen 0.9 sources, fixtures, and maps (not built) |
| `FreeCM/` | Dependency-management submodule |

## Further reading

- [Ravo project](Ravo/README.md)
- [Commit hooks](hooks/README.md)
- [Product plan](TODO_REWRITE.md)
- [Dependency workflow](DevDocs/Dependency_Workflow.md)
- [Packaging](DevDocs/Packaging.md)
- [GPU baseline](DevDocs/GPU_Baseline.md)
- [Frozen 0.9 tree](legacy/README.md)

## License

Ravo is distributed under the [GNU General Public License, version 3](LICENSE).
