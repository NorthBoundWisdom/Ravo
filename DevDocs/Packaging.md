# Ravo packaging

Ravo has one release packaging graph for local FreeCM Package actions and
GitHub Actions. Configure remains explicit; packaging never initializes source
roots or configures a build tree as a hidden side effect.

## Ownership and lifecycle

- Ravo CMake owns the `ravo_studio` and `ravo` payload, generated package JSON,
  stable `RavoDeploy` / `RavoPackage` targets, and final archive names.
- FreeCM `repomgrcpp.package` owns Qt/QML deployment, runtime dependency
  collection, the platform dist directory, macOS signing, and DMG creation.
- `configs/freecm.commands.jsonc` owns the local plugin action. GitHub Actions
  owns CI host preparation and artifact retention, but calls the same
  `RavoPackage` target.

The lifecycle is:

```text
source-root preparation -> Release Config -> RavoPackage
  -> build Studio and CLI -> FreeCM RavoDeploy -> platform artifact
```

FreeCM cleans only the selected build tree's `dist` child. Windows and Linux
remove an existing final archive before recreating it. A failed or cancelled
command returns a failure and is not uploaded; rerunning `RavoPackage` rebuilds
the dist payload. There is no alternate deploy implementation or silent
fallback.

The macOS config explicitly removes Qt's optional Mimer SQL driver after
`macdeployqt` because Ravo supports only QSQLITE and the official Qt package
does not ship Mimer's external `libmimerapi` dependency. FreeCM validates this
bundle-relative exclusion and applies it before dependency-closure scanning;
the required QSQLITE driver remains packaged. The bundled `ravo` CLI is also
declared as an additional macOS executable so `macdeployqt` gives it the same
portable Qt runtime paths as Studio. Ravo does not request FreeCM's optional
second rpath-normalization pass because universal Qt slices can carry different
per-architecture rpath sets.

## Local commands and artifacts

Machine-specific runtime roots are owned by the ignored active lock:

```jsonc
"cmakeCacheVariables": {
  "mac": {
    "RAVO_PACKAGE_RUNTIME_SEARCH_PATHS": "/path/to/Qt/lib;/path/to/package/lib"
  }
}
```

Use the matching example in `source_roots.lock.jsonc.in`, edit only the active
lock for the current machine, then run
`python3 configs/source_root_workflow.py --update`. FreeCM writes the value to
the generated configure presets. Ravo CMake consumes it without probing host
package layouts; an empty value is a configuration error.

Windows runtime roots must also include the active MSVC toolset's
`x64/Microsoft.VC143.CRT` directory so FreeCM can satisfy the explicit
`MSVCP140.dll` / `VCRUNTIME140*.dll` dependency closure. CI obtains this root
from `VCToolsRedistDir` and writes it to the active lock before `--update`.

Run the Release Config action first, then the matching FreeCM Package action.
The direct CMake equivalent is:

```text
cmake --preset mac_clang_release
cmake --build build/mac_clang_release --target RavoPackage
```

The FreeCM plugin also exposes a Debug Package variant for the default Debug
Config, which keeps the Package button connected to the active configuration.
Config readiness remains mandatory: select and run Debug or Release Config,
then Package runs the matching build tree. Only Release artifacts are intended
for distribution.

| Host | Configure preset | Artifact |
| --- | --- | --- |
| macOS | `mac_clang_release` | `build/mac_clang_release/dist/Ravo-Studio-<version>-macOS.dmg` |
| Windows | `win_msvc_release` | `build/win_msvc_release/package/Ravo-<version>-Windows.zip` |
| Linux | `linux_clang_release` | `build/linux_clang_release/package/Ravo-<version>-Linux.tar.gz` |

The macOS bundle places the CLI at `Ravo Studio.app/Contents/MacOS/ravo`.
Windows and Linux place `ravo` beside `ravo_studio` in the deployed binary
directory.

Linux currently publishes a FreeCM AppDir archive, not an AppImage. This makes
the supported artifact explicit without downloading an unpinned AppImage tool;
portability is limited to compatible Linux/glibc environments.

## GitHub Actions

Every branch push and pull request runs only the Debug configure/build/test
matrix. A tag push runs that same gate first, then starts Release package jobs
for macOS, Windows, and Linux. Each Release entry prepares the same pinned
source roots and host dependencies, configures the release preset, calls
`RavoPackage`, and uploads only the expected platform artifact. Missing output
is a hard workflow failure.

## Minimum validation

Packaging changes require these checks:

```text
python3 FreeCM/tools/validate_repo_commands.py .
python3 -m repomgrcpp.package.cli validate-config --config build/<preset>/package/package_<platform>.json --platform <platform>
cmake --build build/<release-preset> --target RavoPackage
```

The package build must be run on each target host. A macOS result does not
validate Windows DLL deployment or the Linux AppDir runtime.
