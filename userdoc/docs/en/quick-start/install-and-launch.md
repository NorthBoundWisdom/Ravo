# Install and Launch

## Download a release

Open [Ravo Releases](https://github.com/NorthBoundWisdom/Ravo/releases/latest)
and choose the package matching your operating system and processor.

| Computer | Download and launch |
| --- | --- |
| Apple-silicon macOS | ARM64 DMG; copy Ravo Studio to Applications and open it |
| Intel macOS | x86_64 DMG; copy Ravo Studio to Applications and open it |
| Windows x86_64 | ZIP; extract the complete folder, then run `ravo_studio.exe` |
| Linux x86_64 or ARM64 | Matching AppImage: mark it executable and open it; DEB: install with your package manager |

Keep the complete packaged runtime together. Copying only the executable can
leave Qt plugins, translations or codec libraries behind. Release notes
describe the features and installation constraints of that download; the
handbook follows the current development branch and can be ahead of it.

For Linux, packages target the documented modern distribution baseline.
DEB installation should use the distribution package manager so it resolves
desktop, PipeWire and VA-API dependencies. AppImage users need the corresponding
host libraries and a working display session. On Ubuntu-family systems the
multimedia libraries include `libpipewire-0.3-0`, `libva2`, `libva-drm2` and
`libva-x11-2`. See
[Packaging](https://github.com/NorthBoundWisdom/Ravo/blob/main/DevDocs/Packaging.md)
for the complete runtime baseline.

Ravo is under active development before 1.0. Choose the exact release notes
and platform evidence for your download rather than assuming all features on
`main` have already shipped.

## First launch

Open Studio and create a library at a writable local location, or open an
existing Ravo `.sqlite` catalog. The first-launch filename suggestion is
`Ravo Library.sqlite` in Pictures.

Studio restores its last window geometry and workspace panel sizes. A stored
window outside the current displays is fitted onto a connected screen.
Choose **File → Settings → General** to select the interface language.
Settings also groups workspace sizes, current-catalog backups and Assistant
connection preferences.

Continue with [First launch and import](first-launch-and-import.md).

## Build from source

These instructions are for contributors and people testing an unreleased
development baseline.

You need CMake 3.26+, a C++20 compiler, Python, Ninja and Qt 6.11.2 with Core,
Gui, Sql, Network, Qml, Quick, Quick Controls/Dialogs/Layouts, LinguistTools,
Svg, ShaderTools and Multimedia. Install the complete image-format and SQLite
runtime plugins. The selected Qt kit supplies matching FFmpeg 7.1.5 libraries.

Dependency preparation needs access to the pinned source-root repositories.
Follow the
[dependency guide](https://github.com/NorthBoundWisdom/Ravo/blob/main/DevDocs/Dependency_Workflow.md)
for kit paths and dependency access.

For a new checkout:

```text
git submodule update --init FreeCM
python3 configs/source_root_workflow.py --init
python3 configs/source_root_workflow.py --update
```

For a prepared checkout, inspect its current roots before changing them:

```text
python3 configs/source_roots.py show --format json
python3 configs/source_roots.py resolve --format json
python3 configs/source_roots.py verify
```

`--init` is the network-enabled preparation step. `--update` materialises
existing dependencies offline and generates host presets; ordinary builds do
not do either implicitly.

### macOS

```text
cmake --preset mac_clang_release -DBUILD_TESTING=ON
cmake --build --preset mac_clang_release
./build/mac_clang_release/Ravo/desktop/ravo_studio.app/Contents/MacOS/ravo_studio
```

### Windows

From a matching MSVC developer environment:

```text
cmake --preset win_msvc_release -DBUILD_TESTING=ON
cmake --build --preset win_msvc_release
build/win_msvc_release/Ravo/desktop/ravo_studio.exe
```

### Linux

```text
cmake --preset linux_clang_release -DBUILD_TESTING=ON
cmake --build --preset linux_clang_release
./build/linux_clang_release/Ravo/desktop/ravo_studio
```

Use the matching `*_debug` preset for Debug. Generated presets are
host-specific; `cmake --list-presets` lists those prepared for the checkout.
Build and package qualification is platform-specific.

## Open a known library

Studio accepts these launch arguments on every supported host:

```text
ravo_studio --catalog "/path/to/Ravo Library.sqlite" --language en_US
```

Use the actual executable path from the package or build tree. The catalog
option also accepts `--catalog=/path/to/library.sqlite`. Language choices are
English, German, Spanish, French, Brazilian Portuguese, Simplified/Traditional
Chinese, Japanese and Korean. A missing selected language package reports an
error.

## Verify a source build

```text
ctest --test-dir build/mac_clang_release --output-on-failure --parallel 4
```

Substitute your preset's build directory. For a short user-run workflow check,
follow the [smoke test](../qa/smoke-test.md).
