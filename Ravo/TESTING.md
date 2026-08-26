# Ravo Testing Strategy

## Current evidence baseline

Frozen 0.9 assets are used only for static evidence:

- `legacy/tests/` contains 158 XMP + `expected.png` fixture sets and five
  source images.
- Fixtures cover 68 operation names; that number is an asset inventory, not
  Ravo coverage.
- The old project, old CLI, old CTest, old package targets, and
  `legacy/tests/run` are all prohibited from execution.

Boundary checks:

```text
python3 Ravo/tools/freeze_legacy_manifest.py --check
python3 Ravo/tools/check_capability_inventory.py
python3 Ravo/tools/check_freeze_reference.py
python3 Ravo/tools/check_ravo_dependency_boundary.py
```

The dependency-boundary checker covers the product target graph:
`Qt6::Sql` is adapters-only, `Qt6::Gui` is raster adapters/desktop-only,
`Qt6::Qml`/`Qt6::Quick`, `QtQuick.Controls`/`QtQuick.Dialogs`/
`QtQuick.Layouts`, `GeoControls`/`GeoControls.AppShell` imports, and production
`.qml` are desktop-only; every Ravo target rejects Qt Widgets. Do not remove a
check to admit a new dependency.

The current Ravo Debug graph covers foundation/recipe/engine/CLI and catalog
integration. Review contracts include schema v2→v4 migration, review
persistence, filtering, and missing-source state. Develop contracts include one
canonical recipe per image, schema-v1/v2 → v3 explicit colour-boundary upgrade,
CPU Develop operations, edited previews, and
`ravo catalog` JSON commands going through the same CatalogService. Schema v4
covers Unicode tag filtering, writable metadata, and history/snapshot save and
restore. FreeCM Test and `ctest --test-dir build/<preset>` run the same suite
from the repository root. GitHub Actions runs the same CTest set on
`mac_clang_debug`, `linux_clang_debug`, and `win_msvc_debug`, plus static
freeze/capability/boundary checks. CI runs `--init`, updates Qt/PATH in the
active lock, then runs `--update`. Builds use `cmake --build build/<preset>` so
they do not depend on the Linux template's `ClangDebug` build-preset name and
Windows gtest discovery can see Qt on runner `Path`. CI does not build
`legacy/`.

Unit/contract coverage includes foundation/recipe/executor, CLI JSON/exit
codes, limited XMP mapping, and real `mire1.cr2` inspect/render. Catalog tests
cover schema create/reopen/newer-version reject, idempotent PNG/JPEG/RAW
import, directory sidecar skipping, unchanged source hashes, preview cache,
missing/unsupported input, and recipe/review independence. They do not replace
local manual Studio Fit/100%/Develop acceptance. Each successful link of
`ravo_studio` runs `--smoke` on the `offscreen` platform to load the root QML
and exit immediately; a QML component failure fails the target build. Manual
check: `$<TARGET_FILE:ravo_studio> --smoke`.
`ravo_desktop_command_tests` validates built-in command/action coverage,
stable IDs, runtime-state rechecks, invalid dispatch, shortcut conflicts,
three-platform primary modifiers, and Unicode fuzzy search; its label is
`ravo-desktop-smoke`. Develop automated contracts also cover injected rollback
failures for recipe/history/revision, pixel-for-pixel equivalence of RAW
interactive and full CPU render, L2–L9 + temperature + input/output profile +
profile gamma + RGB primaries + channel mixer + Color Balance RGB parameter/
pixel reopen, and preview-owner cancellation of a superseded token with old
revision/asset rejection. Separate final-display contracts cover private RGB8
packing and strict legacy display-boundary absorption; neither is a Develop
recipe operation or pixel-reopen claim. Desktop QML smoke verifies that Input
Profile,
Unbreak Input Profile, Output & Soft Proof, White Balance, RGB Primaries, Color
Calibration, and full Color Balance RGB bindings load;
these automated checks do not rely on Computer Use.

The real `ravo` process is also a protocol contract: `--json` stdout contains
exactly one parseable envelope and logging remains file-only. `catalog probe`
drives the same non-persistent Develop preview used by Studio, supports strict
repeatable numeric overrides, reports deterministic display-RGB statistics, and
proves the stored recipe serialization and preview-record set are unchanged. It
is the preferred local entry point for parameter-response sweeps; external image
decoders and manual UI observations are not pixel or persistence oracles.

Focused engine references pin `-1 EV` to an exact one-stop linear reduction,
the basic-adjustments contrast/saturation/vibrance equations, D50 Lab output,
and the non-jumping hidden defaults for Bloom and negative Dehaze. Parameter
response sweeps use a committed RAW or raster input and compare exact channel
sums plus display-luma movement; a qualitative “looks less strong” result is
not an acceptance value.

Localization is a desktop build contract. Ravo/tools/check_i18n.py requires
well-formed TS XML, no active unfinished/empty translations, matching
placeholders/newlines, and English source identity. The
ravo_studio_translations target then compiles both catalogs; the desktop command
test loads the built Chinese QM and verifies QML, command, and presenter
contexts. Refresh catalogs only through the project i18n workflow before these
checks, so current source and historical Chinese translations remain separate
and reproducible.

## Test framework and target boundaries

All Ravo C++ unit, contract, and integration tests use GoogleTest; GoogleMock
may be used where port interaction requires it. CMocka belongs only to frozen
`src/tests` and must not link Ravo targets. Test dependencies are discovered
through the existing FreeCM/CMake toolchain; do not use FetchContent or CMake
network downloads.

Test targets link only new Ravo targets. After Qt Core/Gui/Qml/Quick/Sql and
SQLite enter the first version, tests still must not include GTK, Qt Widgets,
old `src` headers, old database types, or dynamic IOP. `QSqlDatabase` and
`QImageReader` appear only in adapter implementation and their contract tests;
QML source and Qt Quick Test appear only in desktop/test owners.

## First-version test layers

| Layer | Goal | Current representative content |
| --- | --- | --- |
| Unit | Pure value types/algorithms | schema version, URI normalization, asset ID, state machine, cache key |
| Port contract | Implementation and abstraction contract | SQLite repository, filesystem, codec, preview cache |
| Service integration | Real use case without UI | create → import PNG/RAW → list → preview → reopen |
| Engine reference | Pixels/metadata | RAW/raster size, orientation, colour, finite values, bounded output |
| Failure/recovery | Trusted state | duplicate, unsupported, missing, cancellation, transaction failure, cache corruption |
| Desktop acceptance | Minimum product loop | create/open, import, list, selection, fit/100%, restart |
| Resource/performance | Deliverability | import-to-preview, peak memory, long list, window close, cache budget |
| Platform/package | Real deployment | Windows/macOS/Linux configure/build and staged-install runtime loop |

UI testing does not replace service integration. Minimum manual desktop
acceptance may be used today, but catalog, import, preview, and failure paths
need headless automated tests first. QML components may use Qt Quick Test for
binding, intent forwarding, and state presentation; GoogleTest service/contract
tests still validate business outcomes.

## Catalog contract

The SQLite adapter tests at least:

- schema v1 creation, empty-library reopen, each-version migration, and
  unknown-newer-version rejection;
- transaction commit/rollback, foreign keys, unique URI, and concurrent/serial
  connection owners;
- idempotent duplicate import, where a failed item does not produce a ready
  asset;
- a trusted reopenable state for read-only, non-writable, corrupt, disk-full,
  or commit-failed databases;
- close waits for tasks and releases statements/connections; no exception
  crosses a target ABI.

Tests use independent databases in temporary directories and never read or
overwrite a user catalog. Commit schema fixtures with migration versions; never
fake upgrade success by directly editing an old fixture.

## Import and source-image safety

The first integration covers both a repository PNG and
`legacy/tests/images/mire1.cr2`. Later coverage expands to JPEG/TIFF,
directories, corrupt files, and more RAW files.

- Compare the source image hash/size/mtime before and after import to prove a
  reference-only path does not modify the original.
- Confirm formats through codec probing and test misleading extensions and
  unsupported content.
- Each item distinguishes imported/duplicate/unsupported/failed; partial
  failure does not lose details.
- Directory enumeration, sorting, and batch boundaries are fixed in
  deterministic mode.
- Cancellation stops undispatched work; already committed trusted assets remain
  valid.
- After a source moves, the catalog retains missing state; the viewer must not
  show a previous image as its result.

## Preview and viewer

- Write preview cache to a temporary file and publish atomically; a failed
  request never overwrites an existing trusted file.
- Cache keys include source fingerprint, size, and contract version; corrupt or
  missing cache can be rebuilt.
- RAW and raster jointly validate orientation, target size, alpha, colour
  description, NaN/Inf, and memory budget.
- RAW preview contract v7 validates complete decode, explicit input/output
  profiles, and default Sigmoid; the raster baseline must not receive a second
  display transform. Sigmoid requires
  at least schema round-trip, synthetic colour patches, `mire1.cr2`
  channel-sum reference, and catalog reset/reopen.
- Cached PNG validation requires exactly one standard `sRGB` chunk for built-in
  sRGB output, or one `iCCP` and no `sRGB` chunk for every other RGB profile.
- RAW import and Gallery thumbnails may persist embedded-JPEG browse cache. Its
  key must use the `embedded-jpeg` digest and be a separate file from the
  1600px processed preview; `prefer_embedded_preview` must not affect
  interactive/develop/export.
- Interactive preview uses a scene-linear working buffer: CatalogService caches
  RAW unpack/demosaic and a drag applies a recipe only to the cached linear
  buffer. Embedded JPEG must not become editable data. CLI/Studio share the
  `request_preview` contract; late results are dropped by request revision.
- Scopes collect from the current declared display-referred RGB preview: RGB
  histogram skips bin 0 for its peak, and parade uses 8/9 mapping with 160 tone
  bins. The Gallery grid computes scopes from browse thumbnails to avoid full
  RAW decode on selection; loupe/develop still use processed preview and never
  treat embedded JPEG as editable data.
- Quickly switching assets drops late results from old request revisions.
- A superseded Develop request's independent token cancels when a new revision
  publishes; even if cancellation races with completion, an old revision/asset
  cannot update preview.
- After window or catalog close, there is no detached task, late UI update,
  uncommitted transaction, or temporary preview.
- Manual viewer acceptance covers at least loading/ready/missing/unsupported/
  failed, fit, 100%, and pan.
- Gallery-grid scrolling uses browse thumbnails only; it must not queue a
  1600px processed preview for the selected grid item. Opening a catalog with
  existing cache must not rerun an `ensureThumbnail` work queue for every image.
- Import uses system file/folder dialogs. After paths are chosen, scanning and
  import run on workers; left Import/Previews progress is visible from Scanning
  onward, the window remains responsive, and every successfully imported photo
  appears immediately in the grid with a browse thumbnail.

## Frozen fixture reuse

- `tests/fixtures/fixture_classification_ledger.json` remains exactly aligned
  with the fixture-ID set in the legacy manifest.
- Committed RAW, XMP, and `expected.png` are read-only input; store Ravo-owned
  float/goldens, metadata summaries, and tolerances separately.
- Ravo CPU compares pixels, NaN/Inf, size/ROI, alpha, colour, metadata, and
  error state against frozen assets.
- A removed product capability may be excluded only after its compatibility
  decision is recorded and tested as a readable structured rejection.
- One 8-bit PNG alone cannot satisfy operation or colour acceptance.
- `channelmixerrgb` uses two statically decoded schema-v3 parameter instances
  from `0085-channelmixerrgb`, identity/single-channel/cross-channel/singular-
  matrix synthetic inputs, and a `mire1.cr2` channel-sum reference. A bare 3×3
  cannot replace the CAT/gamut/V3 saturation-lightness path.
- `temperature` uses statically decoded `0000-nop` schema-v3,
  `0171-capture-sharpen` schema-v4 late-reference, and `0177-bayer4`
  fourth-channel coefficients. It covers Bayer/X-Trans/RGB channel mapping,
  LibRaw as-shot/daylight metadata, manual, late-reference + explicit CAT,
  missing/zero/nonfinite rejection, cancellation/input immutability,
  preprocess cache key, `mire1.cr2` default/manual/camera-reference channel
  sums, and catalog reopen. A Kelvin/tint RGB approximation is not a substitute.
- `colorbalancergb` uses statically decoded `0083-colorbalancergb` schema-v4
  and `0093-colorbalancergb-ucs` schema-v5 parameters, Filmlight Yrg/grading
  RGB plus three-zone-opacity synthetic tests, DT UCS gamut/soft clip, JzAzBz
  92³ LUT/negative-LMS clip, cancellation/no publication on nonfinite values,
  and a `mire1.cr2` channel-sum reference. Catalog reopen and Studio QML smoke
  must cover all 32 parameters + formula; lift/gamma/gain is not accepted as a
  substitute.
- `colorin` uses statically decoded `0107-colorin-gamma`, `0108-colorin-clip`,
  and `0109-colorin-gamma-and-clip` schema-v7 parameter payloads plus the
  `0000-nop` enhanced-matrix `mire1.cr2` channel-sum reference. Synthetic
  coverage includes identity/matrix, 65,536-sample shaper LUT, frozen
  unbounded extrapolation, all five normalization modes, RAW blue mapping,
  complex LittleCMS Lab/ICC input, file/embedded ICC, missing/corrupt/singular/
  non-finite rejection, row cancellation, source immutability, and external
  ICC cache invalidation. Untagged raster must fail unless the recipe declares
  a concrete profile; sRGB fallback is not accepted.
- `profile_gamma` has no enabled history in the 158 frozen XMPs, so tests do
  not invent a legacy payload or golden. Synthetic coverage freezes the CPU
  float `fastlog2`, both `2^-16` floors, grey/shadow/dynamic-range boundaries,
  the 65,536-entry piecewise gamma LUT, negative/index truncation, the exact
  `x == 1` extrapolation boundary, and values above one. Tagged-raster and
  `mire1.cr2` references cover both modes before input colour; cancellation,
  dimension/finite failure, input/profile immutability, scene-linear cache
  identity, CLI/Catalog pixel parity, and Catalog save/reopen are explicit.
  Legacy XMP naming the operation rejects structurally. Picker/autotune has no
  product API and cannot be inferred from display scopes.
- `colorout` statically decodes every distinct schema-v5 payload from all 158
  frozen XMPs. Synthetic coverage includes matrix/shaper and frozen unbounded
  output, matrix-free ICC LUT profiles, RGB/XYZ/Lab, four rendering intents,
  black-point compensation, built-in/file soft proof, cyan gamut warning,
  corrupt/missing/singular/non-finite rejection, row cancellation, and working-
  buffer immutability. `0000-nop` plus `mire1.cr2` has sRGB, Display P3, and
  file-ICC channel-sum references. CLI PNG and Catalog PNG/JPEG/TIFF verify
  embedded profile state; output/proof ICC content invalidates final preview
  cache without invalidating the scene-linear cache. Studio presenter/QML and
  catalog reopen cover Output & Soft Proof intent forwarding. Relabelling or
  falling back to sRGB is not accepted. The private final RGB8 packer covers
  negative, zero, half-rounding, one, super-white, RGB order, invalid dimensions
  and model, every non-finite sample, row cancellation, source immutability,
  owned output, and exact sRGB/Display P3/file-ICC state through CLI and Catalog
  publication without a second transfer curve.
- Legacy `gamma` census covers all 158 frozen XMPs: each has exactly one enabled
  schema-v1 instance, zeroed eight-byte payload, one of 12 exact versioned blend
  tuples, zero `multi_priority`, empty `multi_name`, missing-or-zero
  `multi_name_hand_edited`, and no mask attributes. Synthetic contracts absorb
  each exact tuple without emitting a recipe operation and reject modified or
  missing version, disabled state, payload mutation, duplicate instance,
  unknown or cross-paired blend state, non-default multi state, and mask state.
- `primaries` statically decodes the sole enabled schema-v1 instance in
  `0152-rgb-primaries`. Synthetic coverage includes exact identity, each
  primary hue/purity, achromatic tint, frozen forward ray/edge intersection,
  custom RGB→XYZ construction, alternate working profiles, pre-bridge facade
  scheduling, invalid/singular/backwards geometry, float overflow, row
  cancellation, and input/profile immutability. CLI render and Catalog preview/
  export share pixels; `mire1.cr2` has a Ravo-owned channel-sum reference;
  Studio and Catalog cover all eight values through save/reopen. Generic hue
  gradients, display-profile state, masks, and a linear-Rec709 substitution are
  not accepted.
- `hotpixels` covers strict four-neighbor single points, permissive
  three-neighbor adjacent points, an unchanged two-pixel boundary, cancellation,
  raster/X-Trans rejection, decoded-frame immutability, cache-key/reopen, and
  `mire1.cr2` reference. It must not repair after demosaic or modify service
  cache in place.
- `cacorrect` covers the default two-pass `mire1.cr2`, five passes from
  `0084-cacorrect` + avoid-color-shift, Bayer/raster/X-Trans boundaries,
  cancellation, memory budget, decoded-frame immutability, cache key, and
  catalog reopen. A fixed R/B shift cannot meet tile/median/polynomial/
  interpolation gates.

## Deterministic mode

Tests must fix:

- CPU backend, worker count, scheduling, and memory budget;
- catalog schema, URI normalization, and directory-enumeration order;
- preview size, orientation, interpolation, colour, and metadata strategy;
- cache root, cache-contract version, and source-file fingerprint inputs;
- engine, recipe, operation, and third-party dependency versions;
- random seed, if an algorithm requires it.

Floating point may have individually recorded tolerances, but geometry,
orientation, operation order, masks, discrete states, and catalog transaction
semantics cannot change because of a floating-point difference.

## Local labels and validation cadence

Current labels:

- `ravo-unit`: fast pure logic;
- `ravo-contract`: facade, adapter, CLI, and frozen-boundary tests.

Added with the desktop product:

- `ravo-catalog`: schema/repository/service integration;
- `ravo-desktop-smoke`: static command-registry contracts and automatable
  window-lifecycle/composition smoke. It does not replace Studio manual
  acceptance of command-palette focus, keyboard navigation, confirmation
  dialogs, and text-input isolation.

Later add `ravo-regression`, `ravo-sanitizer`, and `ravo-performance`. Never
describe a nonexistent label as passing. Documentation changes do not require a
forced build; CMake/dependency/public-header changes require at least
configure/build; catalog/import/desktop behavior changes run relevant labels
and the real vertical slice; broad scheduling or schema changes run the full
Ravo test set.

Revalidate each dependency or public build-graph upgrade on every actually
available host. Report historical results for other platforms separately from
those untested in this change; never present one platform as passing on all.
