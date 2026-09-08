# Ravo packaging

Ravo has one release packaging graph for local FreeCM Package actions and
GitHub Actions. Configure remains explicit; packaging never initializes source
roots or configures a build tree as a hidden side effect.

## Ownership and lifecycle

- Ravo CMake owns the `ravo_studio` and `ravo` payload, generated package JSON,
  stable `RavoDeploy` / `RavoPackage` targets, and final archive names.
- `DevDocs/THIRD_PARTY_NOTICES.md` is the tracked notice source and is packaged
  beside the root AGPL license under its basename.
- FreeCM `repomgrcpp.package` owns Qt/QML deployment, runtime dependency
  collection, the platform dist directory, macOS signing, and DMG creation.
- `configs/freecm.commands.jsonc` owns the local plugin action. GitHub Actions
  owns CI host preparation, short-lived workflow artifacts, and tag-only
  GitHub Release publication, but calls the same `RavoPackage` target.

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

HEIC/HEIF input on macOS 14+ uses the explicitly linked ImageIO/CoreGraphics/
CoreFoundation system frameworks (ADR-0159). They stay at system runtime paths
and are not copied into the bundle; the packaged notices record that boundary.
The same provider must be reachable from bundled Studio and CLI. Package
acceptance includes primary HEIC import/probe/export and corrupt-input failure;
Windows/Linux packages report an unavailable provider and do not claim HEIC
support. A host Qt HEIC plugin is not the provider or an acceptance substitute.

Studio's versioned locale manifest is the single catalog inventory. Every
declared checked-in TS catalog is validated before lrelease creates build-local
QM files; an undeclared TS file also fails validation. macOS copies those files to
Ravo Studio.app/Contents/Resources/i18n; Windows and Linux package the same
i18n tree beside the executable. A missing or incomplete catalog fails the
Studio translation target before deployment rather than producing a partial
language package. The localization smoke target launches every manifest locale.

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
| macOS | `mac_clang_release` | `build/mac_clang_release/dist/RavoStudio-<version>-<arch>-macOS.dmg` |
| Windows | `win_msvc_release` | `build/win_msvc_release/package/RavoStudio-<version>-<arch>-Windows.zip` |
| Linux | `linux_clang_release` | `build/linux_clang_release/dist/RavoStudio-<version>-<arch>-Linux.AppImage` and `build/linux_clang_release/package/RavoStudio-<version>-<arch>-Linux.deb` |

The macOS bundle places the CLI at `Ravo Studio.app/Contents/MacOS/ravo`.
Windows and Linux place `ravo` beside `ravo_studio` in the deployed binary
directory.

Linux publishes both an AppImage and a Debian package from the same FreeCM-owned
AppDir payload. CI uses pinned appimagetool 1.9.1 and type2 runtime 20251108
assets with fixed SHA256 checks. The DEB installs the private payload under
`/opt/RavoStudio` and owns launchers for `ravo_studio` and `ravo` under
`/usr/bin`; neither format falls back to the former AppDir tar archive.

## GitHub Actions

Every branch push and pull request runs the configure/build/test matrix:
macOS/Linux Debug full, Windows Release full, plus macOS/Linux Release smoke
(`-L ravo-desktop-smoke|ravo-contract|ravo-catalog`). Package and Publish GitHub
Release jobs remain tag-gated. A tag push runs that gate first, then starts
Release package jobs for macOS, Windows, and Linux; each package job runs
`check_packaged_runtime.py` on the real artifact before upload. Each package entry prepares the same pinned source roots and host
dependencies, configures the release preset, calls `RavoPackage`, and uploads
only the expected platform artifact. After all three package jobs succeed, the
single release job downloads those artifacts from the current run, requires
exactly one DMG, ZIP, AppImage, and DEB, and creates the GitHub Release for the
existing tag with generated notes and all four files attached. Missing output, an
existing Release for the tag, or any GitHub API failure is a hard workflow
failure; the workflow does not overwrite an existing release.


## AI provider packaging residual (AI-00 / AI-01)

AI-01 ships with the in-tree deterministic stub provider only
(`ravo.local.stub` / `deterministic-global-v1`). Packaging a real local or remote
model/runtime/weight pack remains residual: record the source, licence/GPL
compatibility, third-party notices, optional download vs bundle size, update
channel, and local cache root in this document and Dependency Workflow before
enabling any non-stub provider. Default network posture stays no automatic
upload; credentials remain desktop-only (ADR-0121).


## Packaged runtime out-of-tree check

`Ravo/tools/check_packaged_runtime.py` validates a DMG/ZIP/AppImage/DEB from the
current package output **outside** the build tree: prints artifact SHA256, locates
CLI/Studio binaries, optionally runs CLI `--help` and `smoke_ravo_studio.py` with
Qt/QML env cleaned. CI package jobs invoke it with `--require-smoke` after
`RavoPackage`.

Role-separated payload identity (commit `f8ffe260`): CLI never accepts `ravo_studio`;
known macOS `.app`, DEB `/opt/RavoStudio/bin`, and flat layouts are used.

Catalog workflow stages (commit `d809cb66`) execute real `catalog create|import|probe|list`
JSON contracts with a synthetic sRGB PNG and are required under `--require-smoke`.
Fake exit-0 CLIs that do not create `library.sqlite` FAIL create (never PASS).

Isolation contracts (commit `8534e048` / checker): AppRun must sit at AppImage extract root;
runtime PATH is minimal system dirs; Qt/DYLD inject vars are scrubbed; evidence JSON is written
on failure. Opt-in `workflow_dispatch` `package_rehearsal` (commit `af61a522`) runs Package jobs
without creating tags or GitHub Releases and uploads per-artifact evidence JSON.

Explicit non-claims (recorded as UNTESTED residuals by the tool):
- native display / installed desktop session (offscreen smoke ≠ native plugins)
- Debian `dpkg` install and `/usr/bin` launcher success (unpack ≠ install)
- AppImage FUSE direct launch (extract-via-`--appimage-extract` is a separate PASS when unpack succeeds)
- host package rehearsal evidence until a rehearsal/tag run uploads digests for the same SHA

`Ravo/tools/test_check_packaged_runtime.py` covers identity resolution, versioned CLI envelope
acceptance/rejection, exact asset membership, probe IHDR integrity, DMG top-level absolute symlink
skip, AppImage AppRun-at-root, minimal PATH, and evidence-on-failure without requiring Qt.
CI publishes packaged evidence before the validate loop so failed checkers still upload JSON.

## Minimum validation

Packaging changes require these checks:

```text
python3 FreeCM/tools/validate_repo_commands.py .
python3 -m repomgrcpp.package.cli validate-config --config build/<preset>/package/package_<platform>.json --platform <platform>
cmake --build build/<release-preset> --target RavoPackage
```

The package build must be run on each target host. A macOS result does not
validate Windows DLL deployment or the Linux AppDir runtime.

Installed launch must work after the archive is copied away from the build
tree. Settings, logs, and live-control sockets use `QStandardPaths` under the
user home or an explicit `RAVO_LIVE_CONTROL_DIR`, never the build tree.
Distribution Studio binaries ship the native Qt platform plugin (`cocoa`,
`windows`, or the Linux desktop plugin), not `offscreen`. Linux DEB launchers
under `/usr/bin` exec `/opt/RavoStudio/bin/...` and are valid only after
`dpkg -i`. A Linux payload may still `RUNPATH` the packaging host's Qt prefix
for transitive ICU; that is a host-kit leftover, not a build-tree dependency.
Linux `appimagetool` may be a PATH command or the seeded
`appimagetool-x86_64.AppImage` plus `runtime-x86_64`. Live-control workspace
discovery treats an unreadable ancestor marker as absent so a packaged
executable outside a Ravo checkout can start.
