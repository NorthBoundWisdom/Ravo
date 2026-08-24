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

Windows uses `win_msvc_debug` / `win_msvc_release`. Linux configure presets are
`linux_clang_*`; build those trees with `cmake --build build/linux_clang_debug`
because the generated Linux build preset is named `ClangDebug`.
FreeCM Config/Build/Run/Test call the same cmake commands.

## Continuous integration

GitHub Actions (`.github/workflows/ci.yml`) follows the same FreeCM path as a
local machine: `--init` creates the ignored `source_roots.lock.jsonc` from the
template, CI then rewrites that lock's Qt/host `CMAKE_PREFIX_PATH` and
`cmakeEnvironment.PATH`, and `--update` generates presets. Configure uses
`cmake --preset *-debug -DBUILD_TESTING=ON`. CI then builds
`build/<preset>` so the runner PATH (including Qt) is visible to Windows
gtest discovery; Linux also cannot use `cmake --build --preset linux_clang_debug`
because the generated build preset is named `ClangDebug`. Frozen `legacy/` is
not configured or built.

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
| `DevDocs/` | Live Ravo developer docs |
| `legacy/` | Frozen 0.9 sources, fixtures, and maps (not built) |
| `FreeCM/` | Dependency-management submodule |

## Further reading

- [Ravo project](Ravo/README.md)
- [Product plan](TODO_REWRITE.md)
- [Dependency workflow](DevDocs/Dependency_Workflow.md)
- [GPU baseline](DevDocs/GPU_Baseline.md)
- [Frozen 0.9 tree](legacy/README.md)

## License

Ravo is distributed under the [GNU General Public License, version 3](LICENSE).
