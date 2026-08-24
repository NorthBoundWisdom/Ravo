# Ravo

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

Windows uses `win_msvc_debug` / `win_msvc_release`. Linux uses `linux_clang_*`.
FreeCM Config/Build/Run/Test call the same cmake preset commands.

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
