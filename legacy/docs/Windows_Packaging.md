# Windows MSI Packaging

DarkTableNext packages Windows Release builds as a per-machine x64 MSI. The
workflow keeps dependency ownership and application packaging separate:

1. FreeCM materializes the pinned source roots and generates the
   `win_msvc_release` preset.
2. CMake builds the first-party application and uses its normal install graph
   to create `build/win_msvc_release/package/windows/stage`.
3. `packaging/windows/package.py` reuses FreeCM's Windows PE dependency parser
   and WiX fragment generator. It moves the core DLL beside the executables,
   verifies the complete DLL closure, and adds relocatable GTK/fontconfig/GLib
   resources from the active vcpkg triplet.
4. WiX v3 compiles the staged payload into
   `build/win_msvc_release/DarkTableNext-<version>-win64.msi`.

The generated MSI uses the stable upgrade code
`A7391ACD-F5E6-5EA2-B772-5E1C61383A32`. WiX creates a new product code for
each package and Windows Installer replaces older versions during a major
upgrade. Rebuilt packages with the same version must be uninstalled before
installation. The installer adds a system-wide Start menu shortcut, supports
a selectable installation directory, and removes installed files on uninstall.

## Prerequisites

Prepare dependencies through the normal FreeCM workflow and install WiX
Toolset v3.14:

```powershell
python3 configs/source_root_workflow.py --update
winget install --id WiXToolset.WiXToolset --exact
```

The default WiX location is
`C:/Program Files (x86)/WiX Toolset v3.14/bin`. Set `DT_WIX_BIN_DIR` during
CMake configuration when WiX is installed elsewhere.

## Build

```powershell
setenv
cmake --preset win_msvc_release
cmake --build --preset win_msvc_release --target package-windows
```

`package-windows` depends on the application, CLI, and all retained loadable
modules. It always recreates the staging tree before generating the MSI. The
runtime closure comes from the build output and active vcpkg triplet; no files
are read from or written into FreeCM-managed dependency source roots except for
copying their license texts.

## Validation

At minimum, verify the MSI database and payload with an administrative extract,
then launch the extracted binary's version path:

```powershell
msiexec /a build/win_msvc_release/DarkTableNext-0.9.0-win64.msi /qn `
  TARGETDIR="$PWD/build/win_msvc_release/package/windows/extracted"
build/win_msvc_release/package/windows/extracted/DarkTableNext/bin/darktable.exe --version
```

Release publication should additionally install and uninstall the MSI on a
clean Windows machine and verify GUI startup, image import, Darkroom module
loading, and local JPEG export. Signing is intentionally outside this local
packaging target; published installers must be Authenticode-signed by the
release workflow.
