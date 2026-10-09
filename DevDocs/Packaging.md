# Ravo packaging

Video packages require Qt Multimedia and its FFmpeg backend plus the same
FFmpeg 7.1.5 shared runtime used by the CLI adapter (ADR-0165). The runtime
prefix and identity requirements are in [Dependency_Workflow.md](Dependency_Workflow.md).
Windows import libraries are generated as build-only link metadata from the
selected Qt DLLs with MSVC tools; the runtime payload remains those Qt DLLs.
CI bootstrap installs `qtmultimedia` alongside image formats and shader tools;
its Qt cache identity includes that module set.
Linux build and clean-package runners install PipeWire and VA-API runtime
libraries (`libpipewire-0.3-0`, `libva2`, `libva-drm2`, `libva-x11-2`). The DEB
declares these host dependencies; AppImage users need them from their
distribution. They satisfy Qt's runtime symbol resolvers even for a headless
CLI invocation. Successful CLI JSON commands must retain empty stderr; missing
libraries are repaired as dependencies rather than muted diagnostics.
Package templates copy the FFmpeg LGPL notice alongside existing notices.
The isolated checker requires video import, no-replace frame artifact and
Qt playback stages. Its muted frame test does not establish audible output or
real iPhone HDR quality; those qualifications remain in
[TODO_VIDEO_SUPPORT.md](TODO_VIDEO_SUPPORT.md).

Ravo has one release packaging graph for local FreeCM Package actions and
GitHub Actions. Configure remains explicit; packaging never initializes source
roots or configures a build tree as a hidden side effect.

CI Qt installation uses Python 3.14: earlier `zipfile` detection can mistake
Qt 6.11.2 Linux ARM64 7z archives for ZIP files and abort aqt extraction with
`Bad offset for central directory` (aqtinstall issue #1042). The shared bootstrap
owns this requirement for both build and package jobs; installation errors remain
fatal.

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
Both executables declare `$ORIGIN/../lib` first in their Linux runtime search
paths because FreeCM copies the build programs into the payload's `bin`
directory. Direct CLI, catalog and offscreen checks must resolve bundled Qt
without the AppRun/DEB launcher's environment or a build-host SDK. The package
checker uses `readelf` to reject missing, SDK-first or incorrectly anchored
private library paths before executing either program.
The Linux runtime list includes the ICU libraries from the configured runtime
prefixes, preserving versioned SONAME links alongside Qt. Package verification
uses `readelf` to check ICU `DT_NEEDED` entries throughout the shipped ELF
payload against its private `lib` directory. Host-installed ICU cannot satisfy
this check; missing versions fail even if a build-host smoke succeeds (issue #3).
Linux verification therefore requires binutils/readelf in addition to the
existing unpack tools.

## GitHub Actions

Every branch push and pull request runs the configure/build/test matrix:
macOS ARM64/Linux x86_64 Debug full, Windows x86_64 Release full, plus
macOS ARM64/Linux x86_64 Release smoke
(`-L ravo-desktop-smoke|ravo-contract|ravo-catalog`). macOS Intel and Linux
ARM64 run the full Release suite on native runners. Packaging runs for tags or
manual rehearsals; publication remains tag-gated. A tag push runs that gate first, then starts
Release package jobs for macOS, Windows, and Linux; each package job runs
`check_packaged_runtime.py` on the real artifact before upload. Each package entry prepares the same pinned source roots and host
dependencies, configures the release preset, calls `RavoPackage`, and uploads
only the expected platform artifact. Five package jobs produce seven assets:

| Runner | Package architecture | Formats |
| --- | --- | --- |
| `macos-15` | `arm64` | DMG |
| `macos-15-intel` | `x86_64` | DMG |
| `ubuntu-24.04` | `x86_64` (DEB `amd64`) | AppImage, DEB |
| `ubuntu-24.04-arm` | `aarch64` (DEB `arm64`) | AppImage, DEB |
| `windows-2022` | `x86_64` | ZIP |

The CI lock writer derives package/DEB architecture from `runner.arch` and
sets `CMAKE_OSX_ARCHITECTURES` explicitly. The committed lock remains a local
setup example, not the CI architecture authority. Qt, ccache, seed caches,
workflow artifacts and evidence names are separated by architecture. Linux
ARM64 installs the native `linux_arm64` / `linux_gcc_arm64` Qt kit and pinned
aarch64 AppImage tools with SHA256 validation.
Package jobs compile `ravo` and `ravo_studio`, explicitly save the compiler cache,
then invoke `RavoPackage`. A later package generation or validation failure does
not discard that uploaded build cache. The restore/save path and key ownership
are specified in [TESTING.md](TESTING.md).

After packaging, `package-smoke` downloads the final artifacts onto fresh
runners without running bootstrap, installing a Qt SDK, or restoring build
caches. Each artifact must pass the existing catalog/offscreen checks plus
`check_packaged_runtime.py --require-native-smoke`. The native check starts the
packaged CLI and Studio with a fresh home and minimal system PATH, strips Qt
and loader overrides, and explicitly selects `cocoa`, `windows`, or `xcb`.
Studio's `--startup-smoke` rejects non-native plugins, loads production QML,
shows the main window, and requires a first frame within 15 seconds. It emits
`ravo.native_startup` JSON v1 with `first_frame=true` and exits through normal
owner teardown. The runner requires this marker plus exit zero. The separate
offscreen `--smoke` retains the full interaction/layout checks.

Linux uses Xvfb/DBus and declared system desktop runtime packages, not Qt
development kits. `configs/package_linux.json.in` declares the system EGL/GL,
font, DBus, GLib, X11 and complete XCB runtime dependencies for DEB installation.
The fresh Ubuntu runners install the same runtime libraries for both DEB and
AppImage checks; AppImage users need these host libraries as well. The list
covers the shipped Qt XCB plugin's ELF dependencies, including shape, randr,
sync and xfixes, as described in the
[Qt 6.11 Linux requirements](https://doc.qt.io/qt-6.11/linux-requirements.html).
Missing system libraries remain startup failures rather than selecting another
platform plugin. DMG/ZIP programs start from their extracted payload outside
the checkout; AppImage starts the final executable through FUSE; DEB is
installed with `dpkg --install` and starts `/usr/bin/ravo` and
`/usr/bin/ravo_studio`. The DEB check refuses an existing Ravo installation and
purges its own package in `finally`, including startup failure. This option is
for disposable hosts. Each process has a timeout; nonzero exit, missing
display/plugin/library, install or cleanup failure blocks publication. JSON
evidence and native loader logs upload even on failure. Job cancellation
discards the disposable runner. Software rendering tests native startup, not
GPU hardware or all desktop use.

The release job waits for every package and clean-startup job, downloads the
same run's artifacts, requires exactly one file for each architecture/format
above with the tag's exact version, and creates the GitHub Release for the
existing tag with generated notes and all seven files attached. Missing output, an
existing Release for the tag, or any GitHub API failure is a hard workflow
failure; the workflow does not overwrite an existing release.
The inventory searches downloaded artifacts recursively: Linux preserves
`dist/` and `package/` beneath its artifact root, unlike the single-file DMG/ZIP
artifacts. Directory depth does not change filename or uniqueness checks.


## Main history safety and release qualification

Ravo is currently maintained as a single-developer repository.
Direct pushes to `main` are the normal development path.
A failed main CI run blocks further planned work and release qualification,
but does not require rewriting published history.
Repairs are pushed forward as new commits.

Two protections stay separate:

### Main history safety

Repository ruleset `22562825` (`Ravo main history safety`) protects
`refs/heads/main` with:

| Item | Value |
| --- | --- |
| Ruleset id | `22562825` |
| Name | `Ravo main history safety` |
| Target | branch `refs/heads/main` |
| Enforcement | `active` |
| Protect rules | `deletion`, `non_fast_forward` |
| Required status checks | none (post-push CI verifies) |
| Bypass actors | empty |
| HTML | https://github.com/NorthBoundWisdom/Ravo/rules/22562825 |

History safety means: `main` may not be deleted and may not be rewritten with
non-fast-forward updates. Direct pushes that advance `main` are allowed.

### Release qualification

GitHub Actions owns release qualification on the exact SHA:

```text
same SHA static/build/test
→ same SHA package
→ same SHA packaged runtime verification
→ fresh-runner native startup for every final artifact
→ release
```

Every push to `main` still receives the normal CI matrix (Static checks plus
macOS ARM64/Linux x86_64 Debug full, Windows/macOS Intel/Linux ARM64 Release
full, and macOS ARM64/Linux x86_64 Release smoke).
That matrix is post-push verification, not a ruleset-required merge gate.

Do not create a release from a failed, cancelled, incomplete, or superseded
`main` tip. Package and Publish GitHub Release remain tag- or rehearsal-gated
and already require `static` + `build` on the same workflow run / SHA.

### Development loop on red CI

When a pushed `main` SHA goes red:

1. Stop further planned product commits.
2. Do not reset, force-push, or amend published `main`.
3. Push a minimal fix-forward commit on `main`.
4. Resume planned work only after that SHA's matrix is green.

Read-back verification (maintainers):

```text
gh api repos/NorthBoundWisdom/Ravo/rulesets/22562825
```

Confirm `enforcement == active`, target `refs/heads/main`, rules contain
`deletion` and `non_fast_forward`, and rules do **not** contain
`required_status_checks`. If the API ever returns 404 or an empty ruleset list,
treat `main` history as unprotected until the payload below is re-applied and
read back.

### Ruleset payload (recreate if missing)

```json
{
  "name": "Ravo main history safety",
  "target": "branch",
  "enforcement": "active",
  "conditions": {
    "ref_name": {
      "include": ["refs/heads/main"],
      "exclude": []
    }
  },
  "rules": [
    {"type": "deletion"},
    {"type": "non_fast_forward"}
  ],
  "bypass_actors": []
}
```

Create with:

```text
gh api repos/NorthBoundWisdom/Ravo/rulesets -X POST --input ruleset.json
```

Then confirm id/enforcement/rules via the read-back command above.
Do not claim history safety exists without that evidence.

## AI provider packaging residual (AI-00 / AI-01)

AI-01 ships with the in-tree deterministic stub provider only
(`ravo.local.stub` / `deterministic-global-v1`). Packaging a real local or remote
model/runtime/weight pack remains residual: record the source, licence/GPL
compatibility, third-party notices, optional download vs bundle size, update
channel, and local cache root in this document and Dependency Workflow before
enabling any non-stub provider. Default network posture stays no automatic
upload; credentials remain desktop-only (ADR-0121).


## Probe artifact evidence (packaged runtime)

Packaged `catalog probe --output` evidence requires nested `artifact` fields:
`type/version/path/mime_type/width/height/byte_count/color_profile/color_profile_fingerprint/content_sha256`.
Meta JSON validation requires `artifact`, `digest_sha256`, `source_sha`, `run_id`,
and `run_attempt`. This remains distinct from build-tree CLI evidence.

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

Without `--require-native-smoke`, the checker records these UNTESTED residuals:
- native display / installed desktop session (offscreen smoke != native plugins)
- Debian `dpkg` install and `/usr/bin` launcher success (unpack != install)
- AppImage FUSE direct launch (extract-via-`--appimage-extract` is a separate PASS when unpack succeeds)
- host package rehearsal evidence until a rehearsal/tag run uploads digests for the same SHA

The clean-startup job requires the first three checks to pass; configuring a
job is not host execution evidence. New architectures remain unqualified until
a rehearsal/tag run supplies successful evidence for the exact source SHA.

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
Distribution Studio binaries always ship the native Qt platform plugin
(`cocoa`, `windows`, or the Linux desktop plugin). macOS packages also set
`mac.includeOffscreenPlugin` so FreeCM copies `libqoffscreen.dylib` beside
cocoa: packaged `--require-smoke` forces `QT_QPA_PLATFORM=offscreen` with a
cleaned env that cannot see the host Qt plugin path. Linux copies the full
Qt `plugins` tree (including offscreen); Windows packages set `windows.includeOffscreenPlugin` so FreeCM passes
`--include-plugins qoffscreen` to windeployqt (same as the Studio POST_BUILD
deploy). Offscreen packaged smoke still does not prove
a native desktop session. Linux DEB launchers
under `/usr/bin` exec `/opt/RavoStudio/bin/...` and are valid only after
`dpkg -i`. A Linux payload may still `RUNPATH` the packaging host's Qt prefix
for transitive ICU; that is a host-kit leftover, not a build-tree dependency.
Linux `appimagetool` is a PATH wrapper around the checksum-verified native
`appimagetool-<arch>.AppImage` and `runtime-<arch>` in CI. Live-control workspace
discovery treats an unreadable ancestor marker as absent so a packaged
executable outside a Ravo checkout can start.
