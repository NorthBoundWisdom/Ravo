# Linux AppImage Packaging

DarkTableNext packages Linux Clang Release builds as an x86_64 AppImage. The
workflow keeps downloaded tooling, first-party installation, and runtime
deployment separate:

1. FreeCM materializes the pinned source roots, `appimagetool 1.9.1`, and the
   versioned x86_64 type-2 AppImage runtime.
2. CMake builds the application, CLI, and every retained loadable module, then
   uses the normal install graph to create
   `build/linux_clang_release/package/linux/AppDir/usr`.
3. `packaging/linux/package.py` uses FreeCM's package helpers to add GTK input,
   pixbuf, print, GLib, icon, and fontconfig resources. It inspects every ELF
   executable and module with `ldd`, bundles the non-glibc dependency closure,
   and collects first-party and Homebrew runtime licenses.
4. The package script writes a relocatable `AppRun`, desktop file, and icon,
   then invokes the pinned tool and runtime to create
   `build/linux_clang_release/DarkTableNext-<version>-x86_64.AppImage`.

`AppRun` sets the runtime search and GTK data paths. GDK pixbuf and GTK input
module caches contain absolute paths, so they are regenerated in a temporary
directory on every launch and removed when the application exits. This avoids
embedding either the build host's Homebrew prefix or the transient AppImage
mount point.

## Prerequisites

Prepare dependencies and package assets through the normal FreeCM workflow:

```sh
git submodule update --init FreeCM
python3 configs/source_root_workflow.py --init
python3 configs/source_root_workflow.py --update
```

`--init` is the only step that accesses the network. `--update`, CMake
configuration, compilation, and AppImage generation operate from the active
lock and local seed files.

## Build

```sh
cmake --preset linux_clang_release
cmake --build --preset clang_release --target package-linux
```

`package-linux` always recreates the AppDir. The generated AppImage tool is
executed in extract-and-run mode, so package creation does not require FUSE.

## Validation

Check the artifact type and launch its version path without relying on FUSE:

```sh
file build/linux_clang_release/DarkTableNext-0.9.0-x86_64.AppImage
build/linux_clang_release/DarkTableNext-0.9.0-x86_64.AppImage \
  --appimage-extract-and-run --version
```

Release validation should additionally run the GUI on clean supported Linux
systems under both Wayland and X11, import a representative RAW image, enter
Darkroom, and export JPEG, PNG, and TIFF files. Build-host validation alone
does not establish compatibility with older glibc versions; publication should
use the project's oldest supported Linux build image.
