# Ravo

Ravo is the only buildable photo software in this repository. Its current
product goal is to deliver a cross-platform first version quickly: create or
open a local SQLite catalog, import JPEG/PNG/TIFF/RAW by reference, and browse
images in Ravo Studio. The existing C++20 Engine and `ravo` CLI are the
software's foundation and headless client; desktop does not implement a second
set of business logic or algorithms.

Current implementation status:

- Foundation/recipe/engine/adapters/CLI/test scaffolding and versioned
  JSON/error contracts are complete.
- `ravo inspect` reads the first LibRaw-supported 16-bit Bayer RAW slice and
  reports width and height in camera-oriented display dimensions.
- `ravo render` executes nop and `ravo.core.exposure`, including crop,
  black/white normalization, camera WB, profile-aware camera-to-working
  conversion, basic 3×3 Bayer interpolation, declared output-profile
  conversion, embedded ICC state, and atomic PNG output.
- Legacy XMP supports empty history, a strict nop baseline with explicit
  `colorin`/`colorout` mapping, and the demonstrated schema-6/v5 manual
  singleton-exposure subset.
- The catalog vertical slice is implemented: reference-only JPEG/PNG/RAW
  import, preview cache outside the library, and the `ravo_studio` Qt Quick
  window using controls from the `GeoControls` source root.
- Browse & Review includes catalog schema v2; ratings, color labels, and reject
  state; Gallery grid/loupe and an Edit pane; a filmstrip that contains whole
  images like the grid and shows number/rating/flags in its letterbox;
  collapsible folder tree; left Import/Export; Fit/Fill/100%; filtering and
  sorting; additive Cmd/Ctrl click and range Shift selection; plus RGB
  histogram/parade scopes in the right panel.
- Studio built-in commands are projected by one C++ registry into menus,
  shortcuts, controls, and the top command palette. macOS uses
  `Cmd+Shift+P`; Windows/Linux use `Ctrl+Shift+P`; unavailable commands retain
  a visible reason.
- Studio UI supports English and Simplified Chinese. The desktop-owned language
  manager persists the selected UI language, loads only the build-produced Qt
  catalog, and leaves service/engine machine errors outside the catalog
  contract.
- Basic Develop provides catalog schema v4 with one canonical recipe per image,
  tags/writable metadata, and persistent history/snapshots. CPU supports RAW
  highlight reconstruction (opposed by default), wavelets+Y0U0V0 denoising,
  lensfun poly3/vignette, dt UCS `colorequal`, graduated filter, and nine-band
  toneequal. Studio provides an Edit panel, before/after, and session undo/redo.
  RAW interactive preview reuses a scene-linear working image; superseded
  requests cancel and late results are dropped by revision; recipe/history/
  revision save atomically, and catalog unit tests cover L2–L9 parameters and
  pixel reopen contracts.
- RAW Repair provides `ravo.raw.hotpixels` v1 on an owned Bayer CFA copy under
  the frozen same-colour four-neighbor path. `ravo.raw.cacorrect` v1 retains
  RawTherapee two-pass tile/polynomial fitting and avoid-color-shift. Unit
  tests cover cancellation, sensor rejection, memory budget, cache immutability,
  catalog reopen, and real RAW references for both.
- White Balance provides `ravo.color.temperature` v1 before demosaic using
  R/G1/B/G2 four-channel coefficients. LibRaw `cam_mul` / `pre_mul` provide
  as-shot and camera-reference defaults; manual stores explicit coefficients,
  while late-reference permits only a following explicit channel-mixer CAT. The
  old Kelvin/tint RGB approximation and generic fallback are removed.
- Input Color provides `ravo.color.input` v1 with explicit input/working
  profile identifiers, intent, gamut-normalization target, and RAW blue
  mapping. RAW publishes a camera-to-XYZ D50 matrix; raster decode preserves
  embedded ICC state. Matrix/shaper profiles use the frozen LUT and unbounded
  path, while general RGB/XYZ/Lab ICC input uses private pinned LittleCMS.
  Missing, corrupt, singular, or unsupported profiles fail structurally; no
  generic matrix or sRGB fallback is used. Canonical recipe schema v3 upgrades
  prior Ravo recipes by inserting explicit source → linear Rec709 and output
  colour boundaries. Input profile state and external ICC content participate
  in the scene-linear and preview cache keys, and Studio exposes the canonical
  Input Profile controls.
- Unbreak Input Profile provides `ravo.color.profilegamma` v1 as an explicit
  pre-input correction for profiles that expect non-linear RGB. Logarithmic
  mode retains the frozen `fastlog2` and `2^-16` floors; gamma mode retains the
  65,536-sample piecewise table and unbounded extrapolation. RAW runs it after
  demosaic and raster runs it on decoded RGB, both before Input Color. The
  operation is opt-in, cache-keyed, and never substituted by the simplified
  `ravo.core.gamma`. Studio exposes manual mode controls; legacy picker/
  autotune remains unsupported until it has a deterministic analysis contract.
- Output Color provides `ravo.color.output` v1 with built-in/file ICC output,
  four rendering intents, soft proof, gamut warning, proof intent, and black-
  point compensation. Matrix/shaper output uses the frozen 65,536-sample LUT
  and unbounded extrapolation; general RGB/XYZ/Lab and proof transforms use
  render-local LittleCMS. Preview contract v7 and `RenderedImage` carry owned
  ICC state. CLI PNG emits standard sRGB metadata or `iCCP`, Catalog PNG/JPEG/
  TIFF embeds the same declared profile, and missing/corrupt profiles fail
  before atomic publish. Studio presents the engine-owned result through Output
  & Soft Proof controls; it never infers a monitor profile or performs a QML
  colour transform.
- RGB Primaries provides `ravo.color.primaries` v1 with working-profile-aware
  red/green/blue hue and purity plus achromatic-axis tint. The engine derives
  the frozen custom-primary matrix from immutable RGB→XYZ D50 state before the
  linear-Rec709 compatibility bridge, retains the declared working profile,
  and publishes an owned result only after finite/cancellation checks. Studio
  exposes all eight canonical controls; hue is persisted in radians and shown
  in degrees.
- Color Calibration provides `ravo.color.channelmixerrgb` v1 with frozen V3
  CPU matrix normalization, CAT16/Bradford/XYZ/RGB, XYZ gamut, saturation,
  lightness, and grey paths. Studio exposes an explicit 3×3 matrix, while CLI,
  preview, and export reuse the same engine operation.
- Color Balance RGB provides `ravo.color.colorbalancergb` v1 in the explicit
  `linear_srgb_d50` workspace, with Filmlight Yrg three-zone luminance mask,
  grading RGB offset/slope/power, fulcrumed luminance, and DT UCS 2022 as the
  default saturation/brilliance gamut path. JzAzBz 2021 is an explicit optional
  formula. Studio exposes the complete canonical parameters; the prior
  Lift/Color gamma/Gain approximation operation is removed.
- RAW preview/export uses `ravo.display.sigmoid` v1 as the sole Standard SDR
  display transform. Recipes may adjust contrast/skew/hue preservation, while
  the default baseline is not marked as a user edit. Gallery embedded-JPEG
  thumbnails and inspect dimensions are corrected to camera orientation.
  Configure requires JPEG/GIF/WebP/TIFF imageformat plugins and the QSQLITE
  driver; missing them is a hard error.

The current legacy migration order is in
[TODO_LEGACY_MIGRATION.md](../TODO_LEGACY_MIGRATION.md); changes of direction
are in [ADR-0007](docs/adr/0007-first-usable-catalog-viewer.md).

## First-version loop

The first version must complete the following:

1. Create or open a catalog database in Ravo Studio.
2. Import local files/directories, carrying at least one PNG and real
   `mire1.cr2` through tests.
3. Show assets in Gallery; select one and view it at fit, 100%, and with pan.
4. Restart and reopen the same catalog for viewing.
5. Duplicates, corruption, missing files, non-writable paths, and cancellation
   have visible, recoverable structured results.
6. Originals are always read-only; previews are atomically written,
   rebuildable caches outside the database.

The first desktop uses Qt 6 Quick/QML. C++20 composition/presenters own
services, tasks, and resources; QML performs only layout, presentation,
binding, and input. A private QSQLITE adapter owns SQLite and a private
`QImageReader` adapter decodes the first JPEG/PNG path. UI consumes only
presenter-exposed service state and read-only previews; SQL, codecs, RAW
processing, tasks, and cache do not enter QML. The first version links no Qt
Widgets and retains no Widgets fallback.

## Build and test

First inspect active source-root state from the repository root:

```text
python3 configs/source_roots.py show --format json
python3 configs/source_roots.py resolve --format json
python3 configs/source_roots.py verify
```

After authorized first workspace preparation or an active-lock change, run:

```text
python3 configs/source_root_workflow.py --init
python3 configs/source_root_workflow.py --update
```

`--init` is the only dependency action allowed to use the network; `--update`
materializes source roots offline and generates the root `CMakePresets.json`.
Packaging runtime paths are supplied by the active lock's
`RAVO_PACKAGE_RUNTIME_SEARCH_PATHS`; the template stores only three-platform
examples. Ordinary Build/Test/Run does not implicitly run Config or dependency
updates.

macOS Debug:

```text
cmake --preset mac_clang_debug -DBUILD_TESTING=ON
cmake --build --preset mac_clang_debug
```

Use `win_msvc_debug` / `win_msvc_release` on Windows and `linux_clang_*` on
Linux. FreeCM Config/Build/Run/Test use the same CMake preset commands.
`Ravo/tools/freecm_project.py` and `.ps1` are optional wrappers only.

Release staged install:

```text
cmake --preset mac_clang_release
cmake --build --preset mac_clang_release
cmake --install build/mac_clang_release --prefix install/mac_clang_release
```

Release package with FreeCM runtime deployment:

```text
cmake --preset mac_clang_release
cmake --build build/mac_clang_release --target RavoPackage
```

The same target produces a Windows ZIP with `win_msvc_release` and a Linux
AppDir tar.gz with `linux_clang_release`. `RavoPackage` includes Ravo Studio,
the `ravo` CLI, Qt/QML runtime dependencies, and the license. Output paths and
CI artifact ownership are documented in [Packaging](../DevDocs/Packaging.md).

FreeCM Package follows the active Config, so Debug and Release each have a
compatible Package variant. Run Config before Package; tagged CI releases
always use Release.

The repository-root CMake builds only Ravo; it must not configure, compile, or
run frozen 0.9 (`legacy/src/`). Windows/MSVC and local macOS/Clang have
previously validated the current engine/CLI graph; Linux still requires
validation on its target host. The addition of Qt Gui/Qml/Quick/Sql, QML
modules, runtime plugins, and desktop requires renewed three-platform results.

## Studio localization

Studio localization source is versioned under desktop/i18n, while QM files are
build output. Refresh source and reuse the persistent Chinese translation
memory with the project-local
[i18n workflow](../.codex/skills/i18n-translation-workflow/SKILL.md):

~~~text
python3 .codex/skills/i18n-translation-workflow/run_i18n_workflow.py --part 1
# Translate only <unfinished> values in Ravo/desktop/i18n/zh_translate.ini.
python3 .codex/skills/i18n-translation-workflow/run_i18n_workflow.py --part 2
cmake --build --preset mac_clang_debug --target ravo_studio_translations
~~~

The workflow requires the Qt kit's LinguistTools. It fails on malformed
translation memory, incomplete active translations, placeholder mismatch, or
invalid TS XML; it never writes source catalogs into the build tree.

## FreeCM project workflow

`configs/freecm.commands.jsonc` uses manifest v2. Debug/Release and
Windows/macOS/Linux are independent Configs. Build, Run, Test, and Package
explicitly bind a compatible Config and never configure secretly. Release
Package calls `RavoPackage` directly and shares the FreeCM deployment path used
by GitHub Actions.

Maintenance actions:

```text
python3 configs/source_root_workflow.py --refreshpin
python3 configs/source_root_workflow.py --pinlatest
python3 configs/source_root_workflow.py --update
python3 configs/source_root_workflow.py --cleanbuild --dry-run
```

`--pinlatest` uses only commits visible in local seeds and leaves the active
lock in `latest`; it is a dependency-refresh candidate, not a release baseline.
`--cleanbuild` preserves `build/dependency_seed_repos` and
`build/dependency_source_roots`. See [Dependency Workflow](../DevDocs/Dependency_Workflow.md)
for complete authority boundaries, FreeCM gitlink updates, manual integration,
and publication order.

## Current CLI capabilities

```text
ravo inspect <input> --json
ravo recipe import-xmp <legacy.xmp> --asset-id <id> --input <input-uri> --output <recipe> --json
ravo recipe validate <recipe> --json
ravo render <input> --recipe <recipe> --output <png> --backend cpu [--width N] [--height N] --json
ravo catalog create --path <library.sqlite> --json
ravo catalog import --catalog <library.sqlite> --input <file-or-folder> --json
ravo catalog list --catalog <library.sqlite> --json
ravo catalog preview --catalog <library.sqlite> --asset-id <id> --json
ravo catalog probe --catalog <library.sqlite> --asset-id <id> [--baseline] [--set <field>=<number>]... [--max-edge N] --json
ravo catalog rate --catalog <library.sqlite> --asset-id <id> --rating 0-5 --json
ravo catalog develop --catalog <library.sqlite> --asset-id <id> --exposure-ev N --json
ravo catalog recipe --catalog <library.sqlite> --asset-id <id> --json
ravo catalog export --catalog <library.sqlite> --asset-id <id> --output <file> --format png|jpeg|tiff|original [--quality 90] --json
```

An existing output path returns structured `conflict`; it is never overwritten
implicitly. Catalog commands call the same services as Studio and serve as the
headless acceptance client.

`catalog probe` is a read-only Develop diagnostic. It renders the current recipe,
or the synthesized product baseline with `--baseline`, through the same
non-persistent interactive-preview path as Studio. Repeated `--set name=value`
overrides accept every numeric Develop field, reject unknown, duplicate,
non-finite, or out-of-range values, and return dimensions, output-profile ID,
RGB sums/means/extrema/clipping counts, and display-luma mean. The command
reloads the stored recipe and preview-record set after rendering and fails if
either changed; it writes neither a recipe nor a preview record. CLI logging
remains file-only so machine JSON is the only stdout content.

## Names and directories

| Name/directory | Purpose |
| --- | --- |
| Ravo Engine / `engine/` | RAW/raster, CPU preview/render, colour, and operations |
| `ravo` / `cli/` | Supported CLI and machine-JSON client |
| `foundation/` | errors, IDs, cancellation, and resource contracts |
| `recipe/` | versioned recipe/operation schema |
| `adapters/` | filesystem, codec, SQLite catalog, raster JPEG/PNG, preview cache |
| `domain/` | Asset/Catalog/Import/Preview state and ports |
| `services/` | create/open/import/list/preview use cases |
| Ravo Studio / `desktop/` | C++ presenters with Qt Quick/QML Gallery and viewer |
| `tests/` | unit, contract, catalog integration, fixtures, and later desktop smoke |

After a Debug build, Studio is at
`build/mac_clang_debug/Ravo/desktop/ravo_studio.app` (Windows:
`build/win_msvc_debug/Ravo/desktop/ravo_studio.exe`; Linux:
`build/linux_clang_debug/Ravo/desktop/ravo_studio`). Pass
`--catalog <library.sqlite>` to open an existing library directly. FreeCM Run
works like GeoDebugger/DwgParser: first run
`cmake --build --preset … --target ravo_studio`, then start the GUI directly.
The first manual loop is: Create Library → Import
`legacy/tests/0000-nop/expected.png` and `legacy/tests/images/mire1.cr2` →
select an asset → Fit / 100%.

## Relationship to frozen `legacy/src/`

`legacy/src/` is the read-only factual source for 0.9 behavior; Ravo is the
only growth direction. Ravo may statically read source and fixtures, but
production targets must not include old private headers, link old libraries,
load old IOPs, or access global `darktable`. The frozen application also
receives no Ravo adapter. Handle the remaining old application only after Ravo
meets the root TODO's release-transition and rollback gates.

## Documentation entry points

- [AGENTS.md](AGENTS.md): Ravo subtree implementation constraints;
- [ARCHITECTURE.md](ARCHITECTURE.md): target, data, ownership, and thread
  boundaries;
- [MIGRATION.md](MIGRATION.md): one-way migration, ledger, and retirement
  rules;
- [TESTING.md](TESTING.md): first-version catalog/import/viewer and frozen
  fixture acceptance;
- [i18n workflow](../.codex/skills/i18n-translation-workflow/SKILL.md):
  source extraction, Chinese translation memory, and catalog validation;
- [ADR index](docs/adr/README.md): durable architecture decisions;
- [root legacy migration TODO](../TODO_LEGACY_MIGRATION.md): unfinished
  execution items and gates only.

The repository is distributed under GPLv3; see the root [LICENSE](../LICENSE).
