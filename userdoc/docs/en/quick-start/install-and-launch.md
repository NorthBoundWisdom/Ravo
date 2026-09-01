# Install and Launch

## Goal

Build the current Ravo source tree, launch Ravo Studio, and reach the library
home page.

**Last verified:** 2026-08-27 against the current repository source and macOS
Debug build layout.

## Applies to

- Source builds on macOS, Windows, and Linux.
- Ravo Studio users and testers validating a build before importing photos.

## Prerequisites

- A checkout of this repository on a supported toolchain host.
- CMake 3.26 or newer and a C++20 compiler.
- Qt 6.11 with Core, Gui, Sql, Qml, Quick, Quick Controls 2, Quick Dialogs 2,
  Quick Layouts, LinguistTools, and Svg.
- The complete Qt image-format/runtime kit, including JPEG, GIF, WebP, TIFF,
  and SQLite plugins.
- FreeCM source roots prepared for the checkout. The repository's active lock
  and source-root commands are the authority for dependency locations.

## Prepare a new checkout

From the repository root, inspect the active source-root state first:

```text
python3 configs/source_roots.py show --format json
python3 configs/source_roots.py resolve --format json
python3 configs/source_roots.py verify
```

If the workspace has not been prepared yet, initialize and materialize its
dependencies:

```text
git submodule update --init FreeCM
python3 configs/source_root_workflow.py --init
python3 configs/source_root_workflow.py --update
```

`--init` is the only first-workspace step above that may use the network.
`--update` materializes the existing lock offline and regenerates the host
presets. A normal build does not perform either action implicitly.

## Build on macOS

```text
cmake --preset mac_clang_debug -DBUILD_TESTING=ON
cmake --build --preset mac_clang_debug
```

Launch the resulting application from the repository root:

```text
./build/mac_clang_debug/Ravo/desktop/ravo_studio.app/Contents/MacOS/ravo_studio
```

## Build on Windows

Configure and build with the matching MSVC preset:

```text
cmake --preset win_msvc_debug -DBUILD_TESTING=ON
cmake --build --preset win_msvc_debug
```

The executable is:

```text
build/win_msvc_debug/Ravo/desktop/ravo_studio.exe
```

Use `win_msvc_release` for a release build.

## Build on Linux

Use the available Linux Clang preset:

```text
cmake --preset linux_clang_debug -DBUILD_TESTING=ON
cmake --build --preset linux_clang_debug
```

The executable is:

```text
build/linux_clang_debug/Ravo/desktop/ravo_studio
```

The exact Linux preset variant is host-dependent; use `cmake --list-presets`
if the named preset is not present in the active generated file.

## Open a known library directly

Pass a local SQLite catalog path when launching Studio:

```text
./build/mac_clang_debug/Ravo/desktop/ravo_studio.app/Contents/MacOS/ravo_studio \
  --catalog "/path/to/Ravo Library.sqlite"
```

The equivalent option is accepted as `--catalog=/path/to/library.sqlite`.
Without this option, Studio opens the existing default library when it exists;
otherwise it opens the Create Library dialog.

## Choose the Studio language

Studio supports English, German, Spanish, French, Brazilian Portuguese,
Simplified and Traditional Chinese, Japanese, and Korean. English is selected with:

```text
--language en_US
```

The in-app path is **File → Settings → Language**. The selection is persisted
for the next launch. If a build does not contain the selected Qt translation
package, selecting that language reports the missing package instead of
silently showing a partial translation.

## Result

- Ravo Studio starts without a dependency or QML-load error.
- The window shows the library home page, or opens the requested catalog.
- A new workspace is ready for [library creation and import](first-launch-and-import.md).

## Common questions

### Why does CMake stop while looking for a Qt plugin?

Ravo requires the Qt image-format plugins and the QSQLITE driver at configure
time. Install a complete Qt kit, then configure again. Do not replace the
plugin with a host-selected image codec.

### Why is there no library on the first launch?

That is normal. Create a `.sqlite` library from **File → Create Library**.
Studio's default suggestion is `Ravo Library.sqlite` in the writable Pictures
directory.

### Where is the CLI binary?

For the macOS Debug preset it is:

```text
build/mac_clang_debug/Ravo/cli/ravo
```

Windows and Linux use the corresponding preset directory and
`Ravo/cli/ravo` or `Ravo/cli/ravo.exe`.

### Does launching Studio start the old application?

No. Ravo Studio is a separate Qt Quick application. The frozen leftover darktable
application is not configured, built, or launched by this workflow.
