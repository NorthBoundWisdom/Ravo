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
`Qt6::Network` is control/CLI-local-socket or desktop-only, `Qt6::Qml`/`Qt6::Quick`,
`QtQuick.Controls`/`QtQuick.Dialogs`/`QtQuick.Layouts`,
`GeoControls`/`GeoControls.AppShell` imports, and production `.qml` are
desktop-only; every Ravo target rejects Qt Widgets. Do not remove a check to
admit a new dependency.

The current Ravo Debug graph covers foundation/recipe/engine/CLI and catalog
integration, including first-frame Bayer RAW/DNG errors and preview-cache miss
rebuild. Review contracts include schema v2→v10 migration, review
persistence, filtering, and missing-source state. Develop contracts include one
canonical recipe per image, schema-v1/v2 → v3 explicit colour-boundary upgrade,
CPU Develop operations, edited previews, and
`ravo catalog` JSON commands going through the same CatalogService. Schema v4
covers Unicode tag filtering, writable metadata, history/snapshot save and
restore, history preview without appending, and atomic discard of newer history
steps. Schema v5 adds capture local time/offset and GPS magnitude/reference
columns with strict migration, import-transaction, storage-decoding, and reopen
contracts. Opening a v5 file that still has signed `gps_altitude_mm` repairs
the ADR-0040 magnitude/ref columns in place. Schema v6 adds transactional
per-asset recovery generations, exact acknowledgement, post-preview Studio
Develop publication, restart retry, tamper rejection, and catalog backup/
verification with strict layout, hashes, integrity, destination conflicts, and
preview/original exclusion. ADR-0099 tests add cancellable chunked database
copy, every backup/restore publication checkpoint, destination races,
support-first/catalog-last commit, post-commit durable-fact errors, ordinary
reopen, source immutability, CLI round trip, Studio progress/cancellation, and
explicit preview rebuild. Schema v7 page tests compare every accepted filter
and sort against the domain oracle, traverse 10,000 real SQLite rows in 50
bounded pages, require at most 200 materialized assets per page, and pin page,
tag, and stable-folder query plans. A sparse-model test exposes 10,000 logical
rows while retaining no more than three pages. Import tests require one-item
dispatch, deterministic result order, foreground interleaving, and cancellation
that commits no undispatched input (ADR-0100). Studio additionally pins a
stable Gallery model throughout an active batch, one final switch to the
successful Last Imported Photos range, partial-cancellation membership, folder
exit, command/QML wiring, and catalog-lifecycle reset. Managed-import tests
add exact Copy/Move bytes, same-stem XMP, three destination organizations,
bounded deterministic rename expansion, primary/second-tree portable conflict
preflight, independent byte verification, verification mismatch and
cancellation cleanup, source-change and Move cleanup failures, CLI JSON,
workspace selection, and cancellable background preview policies (ADR-0102/
0104). LibraryQuery tests cover every
supported predicate, missing capture values, inclusive numeric/time endpoints,
ASCII-insensitive plus exact-Unicode text matching, invalid rating/color/media/
text/range state, deterministic capture/file-size sorting, Catalog validation
propagation, Studio command/QML wiring, clear state, translations, and QML
smoke. They also pin the ADR-0059 decision not to persist recent filters or
silently map legacy-only bookkeeping fields, and the ADR-0077 compact filter
bar (default rating stars, opt-in extra chips, command-owned query).
Schema v8 scheduling tests persist and reopen policy state, exercise due and
forced runs, display last verified success/next run/bytes/failure, and retain
unknown or checksum-invalid paths while deleting only strict-name, reverified,
same-catalog artifacts after quarantine re-verification. Schema v9 tests assign
and preserve stable direct-folder IDs through migration/reopen, derive missing
state, validate every replacement file identity, reject conflict/mismatch/
cancellation without mutation, inject failure after folder/asset/revision
updates, and prove transaction/revision/recovery rollback. CLI and Studio
tests use explicit folder IDs and verify that source hashes remain unchanged
(ADR-0101).
Schema v10 tests create manual membership and smart `LibraryQuery` sets,
reject stale catalog revisions and invalid/unknown IDs, keep empty sets, page
listing without materializing the catalog, reopen and restore the rows through
verified backup, and pin Studio/CLI/QML wiring (ADR-0103).
Assistant tests pin default xAI endpoint/model, reject non-http(s) URLs and
blank models, persist valid URL/model/key, repair malformed stored URL, and
toggle the floating panel command. Copy/paste parameter tests pin an empty
session clipboard, no-op without a photo, the two-button History/context-menu/
command wiring, and the shared initially-empty modified-parameter chooser.
Accepted copy stores only explicit stable field IDs; paste overlays those fields,
including required mask nodes, while preserving every unselected destination
value and entering ordinary history/undo. Empty, duplicate, unknown, and stale
selections reject without replacing the clipboard. The old complete clipboard
and Paste Light / Paste Color paths must be absent (ADR-0078/0098). CRS XMP
tests pin leftover
rejection of Camera Raw documents, PV2012 mapping onto Develop owners, calibrated
RAW sigmoid contrast, post-sigmoid display-sRGB curve order, nested Look
isolation, identity-only Point Colors, overlay that keeps destination crop,
unknown-key/Kelvin fail-closed, and `recipe import-xmp` dialect=crs. Synthetic
engine tests pin the scene-EV highlight/shadow endpoints, Whites/Blacks exact
envelopes, strict tone order, positive-sample preservation, shared RGB scale,
single-pass/ordered-composition equivalence, and cancellation for all four;
recipe validation rejects display-sRGB curves carrying scene-only policies. The
Edit left-rail QML contract pins Presets above History with Import and apply
commands. Copy Info and Copy Parameters tests pin the
`ravo.debug.photo` / `ravo.debug.parameters` / `ravo.debug.preset` clipboard
payloads, current saved/pending canonical recipe text, empty selection/unknown-file
failure, and photo/preset context-menu command wiring (QML does not assemble
the identity or parameter text). QML contract tests pin the default grading
stack, the compact Color grouping for White Balance/Presence/Hue, three-way
and global Color Balance RGB wheels, the eight-swatch Color Mixer with
Hue/Saturation/Luminance tracks, common-first Light control order, first-class
Curves, Camera Calibration after Color, vignette geometry including centre,
HSL band names, Detail profile denoise, Color Mixer versus Graduated ND, Color
· Advanced, and RAW white-balance pick wiring.
Engine tests pin CFA sampling of a warm Bayer patch to manual coefficients;
catalog tests reject raster pick.
Session undo is the single stack for right-panel Develop commits and
left-rail history/Original/snapshot restore; live preview/overlay drags do
not push that stack. Consecutive commits for one control retain one history row
and one pre-adjustment Undo anchor. A different control, snapshot, history
restore, selection/view change, undo/redo, or a newer client row breaks the
group; expected-latest replacement and injected SQLite update failure preserve
recipe/history/revision atomicity.
Navigation tests cover the single
presenter zoom owner, 0.1–8 clamps, wheel step, Actual-size toggle restoring
the last non-1:1 mode, bounded Flickable/navigator seek, inspect magnifier
click wiring, active-asset comparison, recenter triggers, crop pan exclusion,
and QML smoke; same-asset review notifications are required not to reset pan.
Progressive-preview coverage uses a source larger than both preview classes and
requires the 960px interactive image to retain the preceding 1600px viewport
extent until settlement; QML must use that accepted presenter extent instead of
the image loader's transient implicit dimensions.
Inspect-click scale/pan animation is QML-only and is loaded by smoke rather
than a C++ timing contract. Develop toolbar comparison tests require that its
baseline is non-persistent and immutable while the edited pane refreshes,
rapid exit rejects a late baseline, and QML presents two whole images through
one shared zoom/pan plane.
Scope tests pin RGB Parade as the initial Studio mode, histogram bins/max plus
Rec.709 luma, Waveform/Parade 8/9 white placement and RGB
composition, neutral-center versus saturated-red D50 u*v*, exact image sizes,
Split left equality and max-preserved right signal, exact-buffer rejection,
five presenter/command/QML modes, provider URLs, translations, and offscreen
smoke. Canvas is checked not to contain the engine color math. Asset mutation
tests cover duplicate revision neutrality, missing/error publication, every
preview cache variant, original-preserving catalog removal, successful disk
deletion, unknown/missing paths, and forced SQLite revision failure. The latter
must restore row, revision, original bytes, and remove the quarantine; repository
delete and revision are exercised as one transaction. These tests do not
complete I11/I12/I13 shared-consumer retirement, PNG pHYs, or TIFF multipage
masks. FreeCM Test and
`ctest --test-dir build/<preset>` run the same suite
from the repository root. GitHub Actions runs the same CTest set on
`mac_clang_debug`, `linux_clang_debug`, and `win_msvc_debug`, plus static
freeze/capability/boundary checks. CI runs `--init`, updates Qt/PATH in the
active lock, then runs `--update`. Builds use `cmake --build build/<preset>` so
they do not depend on the Linux template's `ClangDebug` build-preset name and
Windows gtest discovery can see Qt on runner `Path`. CI does not build
`legacy/`.

Unit/contract coverage includes foundation/recipe/executor, CLI JSON/exit
codes, bounded strict XMP mappings, and real `mire1.cr2` inspect/render. Catalog tests
cover schema create/reopen/newer-version reject, idempotent PNG/JPEG/RAW/DNG
import, directory sidecar skipping, unchanged source hashes, preview cache
including corrupt-PNG miss, missing/unsupported/cancelled input, and
recipe/review independence. They do not replace
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
profile gamma + exposure + RGB primaries + channel mixer + Color Checker +
Legacy Color Balance + Color Balance RGB + Color Correction + Color Contrast
parameter/pixel reopen, and preview-owner cancellation of a superseded token
with old revision/asset rejection.
Separate final-display
contracts cover private RGB8 packing and strict legacy display-boundary
absorption; neither is a Develop recipe operation or pixel-reopen claim.
Desktop QML smoke verifies that Input Profile, Unbreak Input Profile, Output &
Soft Proof, White Balance, Exposure, RGB Primaries, Color Calibration, Color
Checker, Legacy Color Balance, Color Correction, Color Contrast, and full Color
Balance RGB bindings load;
these automated checks do not rely on Computer Use.

The real `ravo` process is also a protocol contract: `--json` stdout contains
exactly one parseable envelope and logging remains file-only. `catalog probe`
drives the same non-persistent Develop preview used by Studio, supports strict
repeatable numeric overrides, reports deterministic display-RGB statistics, and
proves the stored recipe serialization and preview-record set are unchanged.
Optional probe `--output` writes a throwaway PNG of that in-memory RGB without
creating a preview record, rejects a non-PNG path, and returns `conflict` when
the destination exists. `develop-fields` / `catalog fields` list the recipe-owned
`--set` inventory, including kind and measured bounds, and reject unknown names
through the same `apply_develop_field_strict` path. Probe is the preferred local
entry point for parameter-response sweeps; external image decoders and manual UI
observations are not pixel or persistence oracles.
Catalog snapshot tests prove `revision` is re-read from SQLite so a second
connection's bump is visible. Desktop presenter tests open a library, apply a
second CatalogService `save_develop`, call `pollCatalogRevision`, and require
the selected Develop values to match the committed recipe. Live-control
coverage also keeps an in-memory edit across the first post-import poll, proving
that Studio's own import revision is not replayed as an external write.

Live Studio control tests isolate an owner-only registry, prove multiple live
sessions and stale descriptor rejection, enforce the 4 MiB message bound, and
remove discovery state with its owner. A real CLI subprocess reads the selected
asset/current and saved recipes, proves assistant credentials are absent,
rejects a stale revision and an unknown field without mutation, commits one
ordered strict parameter batch, waits for saved/preview settlement, and
publishes a no-replace PNG whose dimensions and SHA-256 are independently
checked. Existing catalog polling covers the concurrent second-client write;
selection and recipe rechecks prevent an in-flight artifact from publishing
for changed state. Repeated subprocess discovery and state requests require
each successful `ping` connection to be released before the next request;
the unresponsive-session test distinguishes a connected response timeout from
a transient Windows named-pipe `not_found` result and bounds same-endpoint
reconnection by the requested timeout. Explicit-session subprocesses resolve a
validated descriptor once and use the requested method as their liveness proof
(ADR-0090).

Focused engine references pin `-1 EV` to an exact one-stop linear reduction,
the basic-adjustments contrast/saturation/vibrance equations, D50 Lab output,
the Color Contrast per-axis float affine/clamp order, and the non-jumping hidden
defaults for Bloom and negative Dehaze. The unified
engine-private RGB↔XYZ D50↔Lab owner has source-derived bit goldens for matrix
order, D50 white/black, both piecewise thresholds, reciprocal-multiply source-order
vectors, negative/extended values, and NaN/Inf propagation. `cbrtf`-dependent
round trips require bit-exact production/scalar-oracle agreement on each host
plus a recorded 1e-5 component reference tolerance for supported-platform libm
variation.
Parameter response sweeps use a committed RAW or raster input and compare exact
channel sums plus display-luma movement; a qualitative “looks less strong”
result is not an acceptance value.

Localization packaging is a desktop build contract: tracked TS XML must remain
well formed and the `ravo_studio_translations` target must compile every catalog
declared by the versioned locale manifest.
Translation wording, completeness, and source-extraction inventory are periodic
localization-maintenance work rather than per-feature acceptance gates. Feature
slices do not add assertions for individual translated strings or extraction
lists; the existing compiled-catalog smoke remains only as a packaging/context
wiring signal. During an explicit localization-maintenance run,
`Ravo/tools/check_i18n.py` requires no active unfinished/empty translations,
matching placeholders/newlines/protected literals, and English source identity.
Contract tests resolve representative system aliases, prove missing packages
leave the prior language active, and load every manifest QM; the offscreen smoke
launches every locale. Refresh catalogs only through the project i18n workflow
so current source and locale-specific historical translations remain separate
and reproducible.

## Photo-management performance evidence

All timing uses one host monotonic clock. Enumeration begins immediately before
`enumerate_import_inputs` and ends when its sorted bounded path vector is
owned. Per-item import begins before `import_one` and ends after the item result,
catalog commit, browse-preview publication, and recovery publication return.
Cold settled preview begins before the first non-embedded 1600-edge request;
warm preview repeats the same request against the verified cache. Page time is
the adapter-owned `LibraryPage.query_elapsed_us`, measured from validated SQL
construction through current-page tags/metadata attachment. The Studio
interactive probe begins at the presenter intent and ends only when the owned
live `QImage` publication signal is observed. Results report P50, P90, and max;
RAW and raster import are separate populations. A zero-entry population is
reported as zero, never merged into another media class.

`CatalogServiceTest.PrivatePhotoManagementReleaseProbePreservesCorpus` is the
explicit private-corpus workflow. Set `RAVO_PHOTO_CORPUS` to a read-only mixed
photo tree and run that single test from a Release build. It snapshots SHA-256,
size, and modification time for every enumerated source, creates all catalog,
preview, recovery, and report state under the test temporary root, imports in
deterministic order, samples at most eight RAW and eight raster cold/warm
previews, traverses the complete catalog in bounded pages, then proves every
source snapshot unchanged. Optional host-local gates are
`RAVO_PRIVATE_PAGE_P90_BUDGET_US`,
`RAVO_PRIVATE_COLD_PREVIEW_P90_BUDGET_MS`, and
`RAVO_PRIVATE_WARM_PREVIEW_P90_BUDGET_MS`. Run the existing
`StudioInteractivePreviewPerformanceProbe` with its fixture/sample/budget
variables for the first slider frame; ADR-0087/0089 remain the exact 960/1600
pixel and 30 ms Release P90 authority. Queue depth is structurally one
dispatched import plus one pending foreground executor item, and sparse-model
memory is structurally three pages, so neither is inferred from process RSS.

Example:

```text
RAVO_PHOTO_CORPUS=/absolute/private/photos \
  build/mac_clang_release/Ravo/tests/ravo_catalog_tests \
  --gtest_filter=CatalogServiceTest.PrivatePhotoManagementReleaseProbePreservesCorpus
```

Corpus results are host-local evidence and stay outside the repository. They
cannot satisfy another OS/toolchain gate, and an unset corpus is a skipped
probe rather than a pass.

## Test framework and target boundaries

All Ravo C++ unit, contract, and integration tests use GoogleTest; GoogleMock
may be used where port interaction requires it. CMocka belongs only to frozen
`src/tests` and must not link Ravo targets. Test dependencies are discovered
through the existing FreeCM/CMake toolchain; do not use FetchContent or CMake
network downloads.

Test targets link only Ravo targets. Tests must not include GTK, Qt Widgets,
old `src` headers, old database types, or dynamic IOP. `QSqlDatabase` and
`QImageReader` appear only in adapter implementation and their contract tests;
QML source and Qt Quick Test appear only in desktop/test owners.

## Test layers

| Layer | Goal | Current representative content |
| --- | --- | --- |
| Unit | Pure value types/algorithms | schema version, URI normalization, asset ID, state machine, cache key |
| Port contract | Implementation and abstraction contract | SQLite repository, filesystem, codec, preview cache |
| Service integration | Real use case without UI | create → import PNG/RAW → list → preview → reopen |
| Engine reference | Pixels/metadata | RAW/raster size, orientation, colour, finite values, bounded output |
| Failure/recovery | Trusted state | duplicate, unsupported, missing, cancellation, transaction failure, cache corruption |
| Desktop acceptance | Minimum product loop | create/open, import, list, selection, fit/100%/click-to-1:1, restart |
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

The baseline integration covers both a repository PNG and
`legacy/tests/images/mire1.cr2`. JPEG/PNG/TIFF catalog import now fully decodes
before publication: truncated JPEG/PNG/TIFF publish no asset or preview;
recognized but unimplemented TIFF layouts stay `unsupported` and do not become
RAW; a camera TIFF without a RAW suffix may still import through
`unsupported_tiff_raw_container`. First-frame RAW/DNG coverage includes
structured LibRaw reasons, `.dng` suffix import of the Bayer fixture, X-Trans
6×6 decode and Engine-rendered preview publication, unpack-before-publish when no embedded JPEG exists,
RAW import cancellation, corrupt PNG cache miss, and close/reopen preview.
`DngOpcodeTest` freezes the big-endian OpcodeList2/3 envelope, checked bounds,
four-parity and partial GainMaps, declared operation order, repeated List3
operations, cubic WarpRectilinear, radial vignette correction, logical-range
clipping, unknown mandatory/optional policy, cancellation, memory ownership,
and a libtiff-written CFA DNG that crosses the LibRaw inspect/decode boundary
without changing its source. A Pixel 6 public reference identified by SHA-256
`c564190aa06cc8006abf3e856e9ca40f9d8af699b1a7f917b6dcfb72975fdf58`
is the manual CLI render reference for four List2 GainMaps plus one List3 Warp.
`BayerDemosaicTest` freezes RCD/PPG smooth-field accuracy, a sharp monochrome
edge false-colour envelope, same-CFA preview reduction, cancellation/memory,
unsupported/duplicate state, source immutability and quantized goldens from
`mire1.cr2`. `XTransDemosaicTest` freezes validated 8/20/8 CFA phase,
Markesteijn 1/3-pass smooth-field accuracy, same-CFA preview reduction,
sensor/mode mismatch, cancellation/memory, source immutability and a quantized
golden from `mire1-xtrans.raf`. Recipe/CLI/Catalog/Studio persistence is tested
independently. Leftover
flip v2 orientation bits 1–7 map to canonical rotate-then-flip; `NULL`/`NONE`
stay identity because camera EXIF is applied at decode. Leftover crop v1–v3
left/top/right/bottom maps to canonical x/y/width/height; full-frame 0,0,1,1
is identity. Leftover ashift v4/v5 generic rotation, vertical/horizontal lens
shift and shear map to canonical Perspective. Specific-lens mode, unsupported
crop modes, manual crop boxes, masks, custom blends, and ambiguous UI state
fail structurally. Export
`max_edge` is the G7 output-size contract. Leftover rgblevels v1 maps to
`ravo.color.rgblevels` with the frozen 65536 LUT, linked/independent modes,
and last-write singleton import of `0054`/`0055`. Auto-levels picker UI is not
a live engine pass. Leftover rgbcurve v1 monotone-hermite maps to
`ravo.color.rgbcurve`, including `compensate_middle_grey` through the live
working D50 matrix so `0060` imports. Leftover rawdenoise v2 Bayer and X-Trans
square-root wavelet paths map to `ravo.raw.denoise`; cancellation leaves the
borrowed decoded frame unchanged and the four-float-plane peak is included in
RAW memory preflight.

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

Radiance RGBE tranche-1 adapter tests pin exact flat and new-RLE float output,
default/custom primary matrices, non-effective gamma/exposure provenance,
canonical orientation, bounded allocation/header parsing, structured malformed
and unsupported input, cancellation, path/memory parity, and source
immutability. Qt raster and Catalog tests exercise both magic tokens and require
the exact `format=rgbe` / `reason=unsupported_rgbe_input` result with zero asset
or preview publication. A dynamic legacy consumer census remains blocked while
the frozen image-I/O dispatcher is reachable; these tests do not claim I9
completion or legacy wrapper retirement.

Original-copy service tests stream across multiple 64 KiB chunks and compare
exact output bytes/byte count while freezing source hash, size, mtime, mode, and
xattrs. They prove that destination metadata is newly created and no XMP is
written; a stale fixed temporary sentinel is preserved. Deterministic internal
hooks cover entry/mid-read/mid-write/pre-publish cancellation, source mutation,
exclusive temporary open, write/finish/publish failures, late no-clobber races,
and write/finish disk-full mapping with owned-temp cleanup. Path taxonomy covers
missing, non-regular, unreadable, missing/non-directory/unwritable parents, and
same/pre-existing outputs with complete `reason`/`source`/`output` context. CLI
tests cover all three aliases plus complete conflict/I/O JSON. CLI end-to-end
cancellation is not injectable in this tranche and must not be reported as
covered. These contracts do not retire `legacy/src/imageio/format/copy.c` or
claim I14 batch/storage policy; ADR-0068 tests that higher owner separately.

Encoded-publication tests freeze exact multi-chunk and empty output, immutable
input bytes, preservation of the old fixed temporary sentinel, and cleanup of
only a uniquely opened adjacent temporary. Regular, directory, symlink,
dangling-symlink, FIFO, and concurrent late targets remain untouched. A private
synchronous hook deterministically covers
entry/mid-write/pre-sync/pre-close/pre-publish cancellation, partial-write
state, temporary open/write/sync/close/publish
failures, stage-preserving disk-full context, a cleanup failure that cannot
replace the primary error, and two parallel writers with exactly one exact
winner. Every result retains `path` and adds explicit `output` context.
Original-copy tests rerun against the extracted destination primitives. The
macOS evidence does not claim parent-directory synchronization,
Windows/Linux execution or generic dynamic-storage ABI retirement.

Batch-export tests freeze the four-token filename grammar, sequence padding,
implicit extension, UTF-8/byte bounds, traversal/portable-name/device rejection,
and unknown-brace failures. Catalog tests prove ordered typed output, progress,
duplicate-name and pre-existing-target zero-publication preflight, cancellation
after the first completed item, and a source disappearing after preflight; the
latter two retain the first output and report completed/total/index/asset/path
partial context. CLI JSON exercises two real assets and shared codec/privacy
options; Studio command/QML tests pin multi-selection template/folder routing.
The old disk module is deleted, while generic storage ABI retirement remains
untested and blocked by U10/J2 (ADR-0068).

Output Dither tests pin all 18 schema names and explicit-default Develop
presence; bit-exact float references cover source-order 1-bit gray and 4-bit
RGB Floyd–Steinberg, tiny no-diffusion, every posterize level, target-aware
auto, and the fixed serial 8-round TEA stream from the random fixture. Invalid
layout/profile/non-finite/mask/schema, pre-publication cancellation, and source
immutability fail atomically. Engine tests prove Output Color precedes
posterize; exact focused 0043/0044/0136 records map while disabled/modified/
cross-paired state rejects. CLI sets the three Develop fields, Catalog proves
save/cache-delete/reopen/export pixel equality, and Studio QML contains no
quantization math (ADR-0069).

Canvas/Frame tests pin both strict schemas, explicit-default Develop state,
operation uniqueness, Output Color → optional Dither → Frame ordering, and the
explicit rejection of post-Canvas rotate/flip/lens or a mask consumer after
composed geometry. Canvas references freeze
percentage truncation, asymmetric placement, fill pixels, attached photo frame,
All/Circle mask coordinates, zero alpha outside the content rectangle, and
pixel-for-pixel overlay-alpha composition through Perspective plus crop.
Frame references freeze constant/aspect/basis/orientation layout and the full
4×4 source-order border-line endpoint result. Invalid dimensions, aspect
underflow, layout/profile/buffer/non-finite input, nested/masked operations,
mid-row/pre-publication cancellation, and source mutation fail explicitly.
Exact 0157 Canvas plus 0030/0154/0155 Frame records map; modified payload state
rejects. Catalog deletes cache, reopens, and exports PNG/JPEG/TIFF with exact
dimensions; CLI, Studio presenter/QML, compiled translations, and offscreen
smoke share the same fields (ADR-0070).

Perspective tests pin the exact eight-field schema, identity, bounded homography
and inverse, deterministic maximal safe crop, bilinear/Lanczos2/Lanczos3
quantized grid goldens, finite/resource/cancellation failure, source ownership,
and robust guide fitting with outliers. A synthetic axis grid covers bounded
Sobel/non-maximum suppression/Hough detection and explicit no-line failure.
CLI analysis emits normalized last-pixel coordinates and does not mutate its
input; Catalog save/cache-delete/reopen/export reproduces the same result;
Studio shares the async service path and analyzes an in-memory 900-edge preview
with crop and existing Perspective removed. Exact frozen v4/v5 generic ashift
payloads cover legacy import; the complete 0018 history is not a full positive
oracle because unrelated `mask_manager` state remains unsupported (ADR-0096).

Watermark tests pin the ten-field schema, printable subset, unknown token and
Unicode rejection, `{stem}`/`{asset_id}` expansion, Develop round-trip, unique
final order, exact 5×7 `A` coverage/blend, and visible execution after Dither
and Frame. Non-finite input plus row/pre-publication cancellation preserve the
source and publish nothing. The sole 0032 v5 `promo.svg` record is negative
evidence because that external file is absent; import returns
`unsupported_legacy_watermark_resource` rather than accepting the old silent
no-op. CLI saves literal text and numeric fields; Catalog proves preview/cache
delete/reopen plus PNG/JPEG/TIFF pixel/dimension paths; Studio command/QML,
compiled translations, and offscreen smoke cover the complete editor
(ADR-0071).

Color Zones tests pin strict 2–20-node schemas, node separation and periodic
gap, all L/C/h partitions, cubic/Catmull–Rom/monotone LUT construction in both
periodic and nonperiodic modes, exact 16-bit LUT normalization, identity and
constant Lab scalar formulas, canonical All-mask equality, LUT/row
cancellation, non-finite/source ownership, and explicit unsafe-spline output.
The exact 520-byte 0022 v5 singleton maps while a modified payload rejects.
Develop/CLI preserve the eight-band surface; Catalog proves preview, cache
delete, reopen, PNG/JPEG/TIFF export and source safety; Studio presenter/QML,
translations, and offscreen smoke cover the bounded editor (ADR-0073).

Monochrome tests pin schema-v2 bounds, schema-v1 amount→mix upgrade, source
bit-fast-exp, uniform-filter scalar lightness, exact zero-mix identity, cleared
Lab chroma, canonical All-mask equality, invalid scale/non-finite state,
pre-bilateral/row/pre-publication cancellation, and source ownership. The
exact 0017 v2 singleton maps while modified payloads reject. The extracted
bilateral lightness primitive reruns all Retouch references. CLI/Catalog/Studio
cover five controls, preview/cache delete/reopen, PNG/JPEG/TIFF export,
translations, and QML smoke (ADR-0074).

Split Toning tests pin schema-v2 bounds, schema-v1 amount upgrade, shared HSL
conversion, exact shadow/midtone/highlight scalar branches, compression/pivot
weights, zero-mix identity, canonical All-mask equality, non-finite/source
ownership, row/pre-publication cancellation, and the exact 0062 v1 singleton
plus modified-payload rejection. CLI/Catalog/Studio cover seven controls,
preview/cache delete/reopen, PNG/JPEG/TIFF export, translations, and QML smoke
(ADR-0075).

Velvia tests pin the exact four-field schema-v2 contract, schema-v1 and CLI
amount upgrades, strength/bias bounds, independent dark/midtone/highlight
scalar calculations at bias 0, 0.15, and 1, zero-strength identity, canonical
All-mask equality, invalid dimensions/profile/non-finite state, row
cancellation, source ownership, and the exact 0063 v2 singleton plus modified
and disabled rejection. CLI exposes the canonical enable/strength/bias fields;
Catalog proves preview difference, source hash safety, export equality, cache
delete, close/reopen, and exact rebuilt pixels; Studio QML and compiled
translations cover the complete editor (ADR-0095).

3D LUT tests pin the exact five-field schema, red-fastest `.cube` order,
independent trilinear and tetrahedral cross-term goldens, per-channel domain
mapping, explicit transfer/primary conversions, unbounded linear-strength
blend, and zero-strength resource validation. Parser tests reject 1D, unknown,
misordered, oversized, overlong, malformed, non-finite, count-mismatched,
missing, and cancelled inputs. Content mutation changes the fingerprint and a
subsequent corrupt file fails instead of using the previous LRU entry. A real
CLI subprocess saves a LUT through `--set-text`, reopens the Catalog, proves a
read-only strength probe, exports PNG, preserves source hash/size/mtime, and
retains the stored recipe after resource failure. `lut inspect`, field
discovery, Studio presenter/QML, compiled translations, and offscreen smoke
cover the supported surface. Synthetic legacy state asserts the stable
`unsupported_legacy_lut3d_resource` rejection because old XMP does not contain
an immutable LUT artifact (ADR-0096).

Camera-noise calibration tests recover a known Gaussian/Poisson model from
weighted uint16-domain samples while rejecting injected variance outliers.
They pin minimum/maximum sample count, signal-span and finite numeric bounds,
pre-cancellation, strict JSON fields/units, canonical source SHA-256,
deterministic profile bytes and payload-tamper rejection. A real CLI subprocess
publishes and inspects the profile, preserves the source sample document, and
proves an existing destination is never replaced. The tests do not imply that
the current denoisers consume the artifact; profile lookup remains separately
gated (ADR-0096).

## Preview and viewer

- Write preview cache to a temporary file and publish atomically; a failed
  request never overwrites an existing trusted file.
- Cache keys include source fingerprint, size, and contract version; a PNG
  without the 8-byte signature is a miss and is deleted so the next request
  rebuilds it. Close releases cache owners; reopen rebuilds from the source.
- Filesystem cache tests use exact byte budgets to prove oversized rejection,
  hit promotion, deterministic LRU eviction, accounting, asset-wide removal,
  and mtime/key indexing plus pruning after reopen. Catalog injection cancels
  after encode and proves that neither a cache file nor a preview row publishes
  at the pre-commit seam (ADR-0067).
- RAW and raster jointly validate orientation, target size, alpha, colour
  description, NaN/Inf, and memory budget.
- RAW preview contract v10 validates complete decode, explicit input/output
  profiles, and default Sigmoid; the raster baseline must not receive a second
  display transform. Sigmoid requires
  at least schema round-trip, synthetic colour patches, `mire1.cr2`
  channel-sum reference, and catalog reset/reopen.
- Profile Denoise tests require deterministic MAD calibration to reduce flat
  luminance and chroma variance while retaining a hard step, observable Radius
  response, independent Chroma mixing, canonical-scale agreement, exact
  identity, finite/parameter/scale failures, cancellation without mutation,
  and four-RGB-plane plus bounded sample/coordinate memory accounting. A real
  high-ISO RAW probe must remain recipe/cache neutral and is visual evidence,
  not a committed pixel oracle (ADR-0094).
- Tone Equalizer tests freeze five-control expansion into all nine one-stop EV
  bands, normalized RBF response, log-guided dark-texture retention, strong-edge
  halo rejection, full/preview scale consistency, finite/overflow errors,
  cancellation, source ownership, and exact five-plane-plus-LUT RAW memory
  accounting. A real RAW `catalog probe` remains read-only and must report both
  recipe and preview records unchanged.
- Cached PNG validation requires exactly one standard `sRGB` chunk for built-in
  sRGB output, or one `iCCP` and no `sRGB` chunk for every other RGB profile.
- RAW import and Gallery thumbnails may persist embedded-JPEG browse cache. Its
  key must use the `embedded-jpeg` digest and be a separate file from the
  1600px processed preview; `prefer_embedded_preview` must not affect
  interactive/develop/export.
- Interactive preview uses a scene-linear working buffer: CatalogService caches
  RAW unpack/demosaic and keeps independent bounded slots for the 960px live
  and 1600px settled size classes. Gallery browse decode/working state is a
  separate bounded lane and must not evict either Develop slot. The live slot
  owns one exact pre-light operation prefix and reusable row team; a prefix hit
  must equal an uncached full render byte-for-byte, while cancellation or a
  prefix parameter change must retain or replace the generation atomically.
  A drag applies the remaining complete recipe to that cached prefix. Entering
  Develop and an ordinary committed style/develop change must publish the exact
  live memory preview before its persisted settled preview. Embedded JPEG must
  not become editable data. CLI/Studio share the `request_preview` contract;
  late results are dropped by request revision.
- Scopes collect from the current declared display-referred RGB preview: RGB
  histogram skips bin 0 for its peak, and parade uses 8/9 mapping with 160 tone
  bins. The Gallery grid computes scopes from browse thumbnails to avoid full
  RAW decode on selection; loupe/develop still use processed preview and never
  treat embedded JPEG as editable data.
- Quickly switching assets drops late results from old request revisions.
- A rapid pure-interactive burst publishes the active frame, replaces only the
  bounded pending request, advances image revisions monotonically, and ends on
  pixels that match the latest parameters. Save, selection, close, comparison,
  and non-interactive supersession still cancel the independent token; an old
  revision/asset cannot update preview.
- Preview SHA-256 and scopes run on the presenter-owned latest-only analysis
  worker. Live control reports identity loading until the exact matching digest
  arrives; a newer frame, selection change, or destruction cancels analysis and
  cannot publish stale identity or diagnostics.
- After window or catalog close, there is no detached task, late UI update,
  uncommitted transaction, or temporary preview.
- Manual viewer acceptance covers at least loading/ready/missing/unsupported/
  failed, fit, 100%, click-to-1:1 restore, and pan.
- Gallery-grid scrolling uses browse thumbnails only; it must not queue a
  1600px processed preview for the selected grid item. Opening a catalog with
  existing cache must not rerun an `ensureThumbnail` work queue for every image.
  A cold page without cache also starts with zero thumbnail work until an active
  GridView/filmstrip delegate requests an asset; demanded work remains one
  request in flight, bounded by the resident sparse pages, and resumes after a
  foreground Develop request without losing or publishing stale demand.
- Before the first exact loupe/Develop result, a verified selected browse
  thumbnail may be visible only while `previewLoading` is true and `previewUrl`
  is empty. Tests require an exact result to replace it and require crop,
  white-balance pick, scopes, live control, and preview identity to continue
  depending on `previewUrl`, not the loading layer.
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
- `exposure` statically inventories all 158 frozen XMPs: 110 revisions in 73
  files (v5=5, v6=102, v7=3), with 109 enabled and one disabled. Tests select
  the greatest `num` independently of XML order, reject conflicting duplicate
  revisions, and cover the 73 final revisions (v5=4, v6=66, v7=3). Exact
  default-unmasked singleton state is accepted; mask/custom blend,
  `multi_priority > 0`, non-empty `multi_name`, and non-frozen lexical state
  reject structurally. Schema v2 and v1 upgrade tests cover manual/deflicker,
  black, percentile, target, and both compensation flags.
- Exposure CPU tests freeze
  `effective_ev = exposure_ev - clamp(exposure_bias, -5, 5) +
  clamp(highlight_preservation, -1, 4)` when enabled and
  `(sample - black) / (exp2(-effective_ev) - black)`. Canon/Fujifilm/Nikon/
  Olympus/Pentax metadata-priority and clamp cases are pure value tests;
  missing tags mean zero EV, while file-read failure matters only when
  compensation is requested. Deflicker uses the original 65,536-bin RAW
  histogram, double `pixels * percentile / 100`, first cumulative bin at the
  threshold, and `target_ev - raw_ev`, replacing manual EV and both
  compensation terms. Synthetic coverage includes finite boundaries,
  pre/row cancellation, memory budget, source/profile/context immutability, and
  owned output. Raster manual mode works, while raster deflicker/compensation
  reject structurally.
- The exposure `mire1.cr2` reference pins black 1015, white 16224, median bin
  2535, and display RGB sums near 251749/234182/220350 with tolerance 1500.
  The source hash, byte size, and mtime remain unchanged. CLI render, Catalog
  preview/save/reopen/export, Studio presenter/QML smoke, and interactive/full
  render parity all consume the same engine context; no test treats the GTK
  area picker as serialized recipe behavior.
- `colorchecker` statically inventories all 158 frozen XMPs. Exactly one actual
  record exists, in 0098: enabled v2, priority zero, empty name, blend v11,
  history `num=8`, default unmasked, and 24 active patches. A minimal document
  containing that verbatim record is positive evidence; the complete 0098
  history remains negative because an unrelated earlier operation is not
  mapped. Synthetic v1 proves only the historical 24-patch source-table upgrade.
  Disabled state, duplicate entries, non-canonical lexical/multi/name/blend/mask
  state, bad lengths/counts, and non-finite active patches reject structurally;
  inactive v2 tail planes are ignored, including stale NaN bits.
- Color Checker CPU tests use an independent scalar/Gaussian oracle plus fixed
  goldens for the exact bit-level fast-log kernel, matrix orientation, N=3 float
  addition, and the verbatim 0098 patch set. A libm substitution perturbation
  proves that the oracle detects kernel drift. N=0/1/2/3/4, RBF N=5 and N=49,
  sequential per-channel N=2/N=3 singular fallback, and shared N=4/RBF identity
  fallback are explicit. The RGB↔XYZ D50↔Lab bridge, dimensions/buffer/profile,
  denominator and all non-finite states, fit/output resource estimates,
  pre/row cancellation, Color Checker operation dispatch/mask rejection, owned
  output, source immutability, and profile/analysis retention are covered.
- Eight preset bit hashes are checked against the frozen source tables. IT8 and
  Expanded also match all 295 parsed C-assignment words; Helmholtz/Kohlrausch,
  Astia, Classic Chrome, Monochrome, Provia, and Velvia each match all 1180
  decoded frozen bytes. The 0098 operation on pinned `mire1.cr2` has bounded
  64×48 channel sums near 295886/283466/247458 and leaves source hash, size, and
  mtime unchanged. Recipe/Develop explicit-default presence, CLI descriptor and
  render parity, Catalog preview/save/reopen/export pixels, Studio presenter/QML
  bindings, localization, and offscreen smoke share the same canonical engine
  and cache path.
- `colorbalancergb` uses statically decoded `0083-colorbalancergb` schema-v4
  and `0093-colorbalancergb-ucs` schema-v5 parameters, Filmlight Yrg/grading
  RGB plus three-zone-opacity synthetic tests, DT UCS gamut/soft clip, JzAzBz
  92³ LUT/negative-LMS clip, cancellation/no publication on nonfinite values,
  and a `mire1.cr2` channel-sum reference. Catalog reopen and Studio QML smoke
  must cover all 32 parameters + formula; lift/gamma/gain is not accepted as a
  substitute.
- `colorbalance` uses synthetic exact default-unmasked schema-v3 and schema-v4
  payloads as its positive importer boundary. The complete 158-XMP census finds
  four enabled v3 revisions only in 0033/0034; their masks, custom blend, and
  named priority-one instances make both real fixtures negative evidence.
  History order does not select processing order, and malformed, duplicate,
  non-default presentation, or multi-instance state rejects structurally.
- Legacy Color Balance CPU tests cover all 17 legacy fields and both frozen
  modes through an independent scalar/matrix Lab D50/XYZ/ProPhoto reference.
  Each fixed SOP/LGG golden is required to match both the reference and
  production; a channel-order perturbation proves the reference detects drift.
  Explicit-default presence survives recipe-to-Develop-to-recipe and Catalog/
  Studio save/reopen because the default colour-space round trip is observable.
  Mode-specific contrast epsilon, finite/denominator/power-domain failure,
  row cancellation, source/profile immutability, and atomic owned output are
  covered. The `mire1.cr2` regression also pins output channel sums and source
  hash/size/mtime. CLI render, Catalog preview/save/reopen/export, and Studio
  presenter/QML smoke share the same engine path and cache identity.
- Color Correction schema tests lock all seven fields, five numeric hard
  bounds, finite/float narrowing, explicit presence, atomic strict editing,
  clamp repair, field/section reset, registry metadata, and canonical Color
  Balance RGB → Color Correction → Color Contrast order. An explicit default
  survives recipe-to-Develop-to-recipe and remains distinct from absence.
- Its independent scalar/D50 oracle and fixed bit goldens lock commit-time float
  narrowing and `saturation * (input + L * scale + base)` evaluation order.
  `cbrtf`-dependent RGB bridge references use the shared 1e-5 platform-libm
  tolerance while requiring bit-exact production/scalar-oracle agreement on
  each host. Engine tests cover canonical dispatch, dimensions/buffer/profile,
  non-finite input/output/parameters, disabled and mask states, owned profile
  plus analysis propagation, source immutability, and pre/row cancellation
  with no partial publication. Allocation failure maps to a structured engine
  error, while the generic working-buffer budget gate accounts for publication
  resources.
- The complete 158-XMP census finds actual Color Correction records only in
  0029 and 0092. Strict tests accept their enabled-v1 singleton,
  priority-zero, unnamed, default-unmasked blend-v9/v11 envelopes and reject
  unsupported version/enabled, duplicate, mask, custom blend, multi/name/
  priority, unknown, malformed, and non-finite state. CLI rendering matches the
  direct engine and preserves RAW hash/size/mtime; Catalog locks explicit-
  default save/preview/export/close/reopen pixels and cache identity; Studio
  locks its six-key presenter, five hard-bound generic intents, compiled Chinese
  strings, and offscreen QML smoke. GTK plane/picker, three presets, OpenCL, and
  canonical mask attachment are outside this Color Correction contract.
- The complete 158-XMP census finds one actual Color Contrast record, in 0038:
  enabled v2, priority zero, empty name, exact default-unmasked blend-v10 state,
  and a generic history position that does not define processing order. A
  minimal document containing the verbatim record is positive evidence; the
  complete 0038 document remains negative because it contains a real mask
  graph. Synthetic legacy v1 freezes the four-float copy plus `unbound=0`.
  Disabled, duplicate, custom blend, mask, multi/name/priority, unknown,
  malformed, non-finite, and unsupported-version state reject structurally.
- Color Contrast recipe tests lock all seven schema-v2 fields, [0, 5]
  steepness bounds, full finite-float offsets, explicit presence/default
  round-trip, field/group reset, canonical Color Correction → Color Contrast →
  Velvia order, and the former Ravo `amount` v1 mapping including its zero
  skip. An independent scalar/D50 oracle and fixed bit goldens distinguish
  axis order, float narrowing/evaluation, bounded ternary clamp, unbounded
  output, extended/negative values, and the observable default Lab bridge.
  Engine tests cover canonical dispatch, dimensions/buffer/profile, every
  parameter/input/output finite failure, Color Contrast mask rejection,
  pre-cancel and row-deadline cancellation, source immutability,
  profile/analysis retention, atomic publication, and separately owned output.
- CLI descriptor/import/render tests, Catalog explicit-default preview/save/
  export/close/reopen pixels and cache identity, and Studio's six-key presenter,
  two hard-bounded slopes, two full-float scientific offsets, unbound toggle,
  generic intents, compiled Chinese strings, and offscreen QML smoke all use
  the same canonical recipe/engine path. GTK sliders, OpenCL, and canonical
  mask attachment are not claimed by these Color Contrast tests.
- Color Harmonizer recipe tests lock schema v1's exact 17 required flat fields,
  hard bounds, float representability, all nine predefined rule names plus
  custom, registry metadata, JSON round-trip, and strict legacy
  mask/custom-blend/presentation rejection. Exact little-endian parameter
  decodes from frozen 0176 history records 12 and 13 cover the post-
  initialization default and an edited split-complementary state and feed the
  accepted singleton importer.
- Its independent source-order scalar oracle covers both 0176 parameter states
  through the declared profile matrix, private D50/dt-UCS bridge, S2.1 geometry,
  frozen negative `fmaxf` clipping, cubic neutral protection, attraction,
  saturation, and inverse matrix. Production and the oracle must match bits on
  each host; libm-dependent component references use the recorded 1e-5
  tolerance, while zero references remain bit-exact. Deliberate no-clip and
  linear-neutral perturbations compare against the same-host canonical oracle
  and prove it detects drift. All nine predefined rules and custom node counts
  two through four also match canonical dispatch. An O3 assembly check verifies
  contraction-disabled production contains no FMA.
- Engine negatives cover dimensions, RGB length/overflow, missing/non-RGB/
  matrixless/non-finite/singular profiles, every non-finite input class,
  non-finite geometry/output, wrong ID/schema, mask state, invalid canonical
  ROI scale for positive smoothing, pre-cancel, and row-deadline cancellation.
  Success owns RGB and deep profile storage, retains the shared immutable
  analysis snapshot, and leaves the source unchanged; failure publishes
  nothing. Vertical-slice tests cover
  strict v1 singleton import (including reversed XML, reused-position
  conflict, and whole-0176 remaining unsupported), explicit Develop
  presence versus absence, CLI `--set` accept/reject, Catalog
  preview/save/reopen/export of explicit positive smoothing, and Studio
  presenter/QML numeric intent bounds. Private S2.2 tests compare an
  independent scalar source-order oracle and fixed constant/impulse/extended
  vectors, including the frozen ±1e9 per-read clamp, dimensions/overflow/sigma,
  and deterministic vertical/horizontal cancellation. Color Harmonizer tests
  cover exact sigma, pull-width floor,
  full/downscaled canonical ROI scale, propagation, two-pass oracle,
  source/profile/analysis ownership, map/Gaussian/apply/prepublication
  cancellation, and saturated RAW memory estimates. Canonical mask tests cover
  attachment alpha 0/all/spatial/path/brush behavior, Studio-owned leaf and
  group creation, overlay composite of exact-zero versus positive alpha, and
  read-only detach of external/shared attachments. They do not relax the frozen
  legacy importer or claim historic blend modes, leftover GTK mask-manager
  deletion, GPU, or C15.
- `colorreconstruct` census finds one actual record, the enabled-v3
  priority-zero unnamed default-unmasked singleton in 0052. Its exact 20-byte
  payload and v10 blend import after the document's exact built-in RAW tuples;
  disabled, duplicate, mask, custom-blend, multi/name/priority, malformed,
  non-finite, and other-version state rejects structurally. The complete 0052
  document becomes positive evidence without making any other history
  compatible.
- Color Reconstruction CPU tests freeze all none/chroma/hue precedence modes,
  the D50 bridge, threshold exclusion and 95% blend ramp, bounded grid shape,
  x/y/lightness five-tap blur, trilinear slice, and canonical full/downscaled
  spatial scale. Dimensions/buffer/profile/scale, parameters, every non-finite
  input/output, grid overflow/allocation, operation mask/schema, and controlled
  splat/blur/slice/prepublication cancellation publish nothing and preserve the
  borrowed source/profile/analysis state. The 0052 payload on `mire1.cr2` pins
  a 42x64 channel-sum reference and unchanged source hash/size/mtime. Catalog
  covers explicit save, preview cache identity, PNG export, close/reopen pixel
  equality, and source hash; Studio presenter/QML, compiled translations, and
  offscreen smoke cover the complete five-parameter surface. These tests do
  not claim tile-local processing, historic blend modes, GTK/OpenCL, or R2
  demosaic completion.
- `sharpen` census finds exactly three enabled-v1 priority-zero unnamed records,
  all with radius 2, amount 0.5, and threshold 0.5. Two exact v9 and one exact
  v11 default-unmasked blends map to schema v2; disabled, duplicate, mask,
  custom-blend, multi/name/priority, malformed, non-finite, and other-version
  state rejects. The complete 0029 document remains negative because its
  Filmic RGB operation is not mapped. Fixture 0171 is separate demosaic capture
  sharpening evidence and does not satisfy F1.
- Sharpen CPU tests compare every Lab component against an independent frozen
  scalar oracle for full/downscaled canonical scale, source-order Gaussian
  normalization and separable convolution, radius cap with retained sigma,
  strict threshold, unchanged borders/chroma, and small-frame pass-through.
  Schema-v1 upgrade, dimensions/buffer/profile/scale, non-finite input/output,
  mask/schema, allocation, RAW memory accounting, and controlled input/
  vertical/horizontal/output/prepublication cancellation are atomic. A
  340x512 `mire1.cr2` default reference proves observable source behavior and
  unchanged source hash/size/mtime. Catalog covers edited preview/cache/save/
  PNG export/close/reopen pixel equality; Studio covers amount/radius/threshold,
  translations, and offscreen QML smoke. These tests do not claim demosaic
  capture sharpening, GTK presets, historic blend modes, or OpenCL.
- `hazeremoval` census finds exactly two enabled priority-zero unnamed
  singleton records: v1 strength/distance 0.2 with the exact v9 default blend,
  and v2 strength 0.9, distance 0.8, adaptive false with the exact v13 default
  blend. Strict tests reject disabled, duplicate, masks (including the full
  0026 mask history), custom blend, multi/name/priority, malformed,
  non-finite, and other-version state.
- Dehaze CPU tests compare ambient quantiles, distance, adaptive full/downscale
  windows, max/min transition and every RGB guided-filter result against an
  independent direct-window covariance oracle. They cover Kahan separable
  means, tiled overlap, Cramer's-rule/singular solve, negative/positive
  strength, minimum transmission, source-linear stage, raster/already-working
  rejection, dimensions/buffer/profile/scale/ambient/non-finite/allocation,
  RAW memory accounting, and controlled dark/selection/transition/tile/
  statistics/solve/output/prepublication cancellation with no mutation. The
  v1 payload on `mire1.cr2` pins an 85x128 reference and source hash/size/mtime;
  Catalog covers cache/save/PNG export/close/reopen equality and Studio covers
  strength/distance/adaptive, compiled translations and offscreen smoke. The
  generic old guided-filter owner remains unclaimed for its other consumers.
- Retouch tests validate the strict nested region schema, drawable-leaf and
  unique-mask references, Develop/recipe serialization, ordered clone/heal/
  Gaussian/bilateral blur/fill execution, original/detail/residual wavelet
  reconstruction, untouched pixels, source/profile ownership, entry
  cancellation, and saturating workspace accounting. The four frozen fixture
  families supply five exact v1 parameter revisions plus v6 circle/ellipse/
  path/brush/group/source payloads; focused import selects the greatest history
  revision, while their unrelated unaccepted `rawprepare`/`basecurve` state is
  not absorbed. Catalog covers preview/cache/save/PNG export/close/reopen pixel
  equality and original hash; Studio presenter/QML/command registration,
  translations, and offscreen smoke cover all four modes and add/remove
  intents. These tests do not claim shared S2/S3 owners, historic whole-document
  replay, GTK/OpenCL, or GPU execution.
- Canonical mask graph tests cover schema-v1 `all` upgrade and deterministic
  v2 serialization; unknown fields/kinds/versions, non-finite/bounded values,
  duplicate/dangling/cyclic/deep/shared DAGs, node/child/expanded-work limits,
  invalid enum state, and v1 non-identity rejection. Private evaluator tests
  freeze pixel-centre exact grids, zero/positive gradient/circle/ellipse
  transitions, path/brush tessellation, frozen parametric branches for
  input/output sources, group
  replace/union/intersection/difference/exclusion ordering with node/edge
  inversion and opacity, whole-
  frame versus vertical and inset-stride tiled ROI, invalid ROI/stride/sample/
  finiteness, pre/node/row cancellation, and owned alpha scratch bounds. Engine
  tests cover exact normal alpha 0/1 selection, spatial mix, overlay composite,
  unsupported-operation failure, and RAW
  saturating masked memory estimates. CLI direct render parity and Catalog
  preview/cache/save/close/reopen tests retain graph/attachments through an
  ordinary Develop edit; reset_recipe alone clears the stored graph. Failed
  strict mutations leave Develop/saved/undo/cache state untouched before the
  existing preview/commit lifecycle is reached.
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
- JPEG export freezes one typed options value from `ExportRequest` through
  CatalogService and the raster port to the pinned private encoder. Domain tests
  cover default quality 95, the 5–100 range, stable enum values and canonical
  names, and fail-closed quality/subsampling errors. Adapter tests parse JPEG
  headers to verify automatic 90/91/92/93 thresholds and explicit
  4:4:4/4:4:0/4:2:2/4:2:0 factors without changing quality-owned quantization,
  DCT, smoothing, or optimized Huffman behavior. Catalog tests prove default
  and explicit propagation, unrelated-format isolation, no partial output on
  validation failure, and source immutability. Existing ICC APP2, 300 dpi,
  65,500-dimension, resource, and entry/scanline cancellation gates remain.
  Final bytes use the shared encoded-publication contract. Catalog/CLI JPEG
  exports independently parse one Exif APP1, one standard XMP APP1, ICC APP2,
  and optional Photoshop APP13, plus unchanged quantization/subsampling.
  Dedicated JPEG CLI tests independently parse SOF sampling factors and the first
  luminance quantizer for defaults and every `auto|444|440|422|420` mode, prove
  last-value-wins, and reject JPEG flags on PNG/TIFF/original/list before the
  Catalog opens. Desktop conversion tests freeze the strict Studio payload keys,
  path/extension policy, and immutable typed `ExportRequest` snapshot; command and
  QML contracts cover the two-step options dialog without localized-filter parsing.
- PNG export freezes one typed options value from `ExportRequest` through
  CatalogService and the raster port to the private libpng encoder. Domain
  tests cover stable 8/16-bit values and names, the 8-bit/compression-5 default,
  compression 0–9, and fail-closed option errors. Adapter tests assert the
  exact zlib level/memory/strategy/window/method/buffer and adaptive-filter
  configuration, then parse IHDR/chunks and inflate rows to prove opaque,
  non-interlaced RGB8 pixels. They verify exact sRGB/Display P3/file ICC, known
  built-in cICP, and that a file ICC whose identifier collides with `srgb` does
  not acquire false cICP. A separate RGB16 encoder path accepts host-endian
  16-bit RGB samples that are not 8-to-16 expansions, writes bit-depth-16 IHDR,
  applies the frozen `png_set_swap` transform where host byte order requires
  it, and proves exact reconstructed samples plus ICC/cICP. Product PNG16
  export renders engine-owned RGB16 and independently parses IHDR/chunks/rows;
  an RGB8 source still returns `reason=unsupported_png_16bit_source` and
  never expands 8-bit samples.
  Dimension/source/output/ICC bounds, invalid
  cICP, mismatched 8/16-bit source/request pairs, entry/row cancellation,
  deterministic libpng/allocation failure, and source/profile immutability
  return no encoded result. Catalog tests prove default and explicit
  propagation, non-PNG isolation, and no file publication for
  unsupported/invalid options; PNG input and shared encoded-publication
  regressions keep I6 decode and atomic destination ownership separate.
  Dedicated CLI tests invoke real Catalog import/export and independently parse
  PNG chunks. They cover implicit and explicit `png` format, both PNG-qualified
  flags, canonical values and defaults, argument order, last-value-wins value
  flags, PNG-only scope, JPEG-option isolation, complete JSON errors, source hashes, and zero
  publication for invalid options, and successful 16-bit product renders that
  are not 8-bit expansions.
  Rendered PNG independently parses one `eXIf` TIFF profile without an
  `Exif\0\0` prefix and one uncompressed XMP `iTXt`; pHYs remains absent.
- Shared export-metadata tests cover XML 1.0 character rejection and
  carriage-return preservation, IPTC-IIM RecordVersion 4 plus dataset-specific
  byte limits, canonical tag count/order, conservative pre-render packet
  estimates, word-aligned Exif TIFF offsets, and deterministic owned packets.
- TIFF export freezes one typed options value from `ExportRequest` through
  CatalogService and the raster port to a private static LibTIFF built from the
  pinned source root with only ZLIB Deflate enabled. Domain tests cover stable
  uint8/uint16/float16/float32 and none/Deflate/predictor values and names,
  uint8/predictor/level-6/RGB defaults, levels 1–9, conditional grayscale, and
  fail-closed option errors. Baseline-directory tests additionally freeze
  resolution default 300 and inclusive 72–9600 bounds, the 16 KiB NUL-free
  well-formed-UTF-8 document-name boundary, bounded writable UTF-8 values, and
  fail-closed legacy raster doubles. Adapter tests independently parse classic
  little-endian IFDs, inflate strips, reverse horizontal prediction, and assert
  exact top-left opaque RGB8 or conditional-grayscale pixels, per-sample bits
  and sample-format tags, compression/predictor, requested inch resolution, and
  exact ICC bytes. The same parser proves exact NUL-terminated DocumentName 269,
  ImageDescription 270, Artist 315, and Copyright 33432 values; absent fields
  omit tags, present-empty emits one NUL, and title remains unmapped in Exif.
  The same parser follows EXIFIFD 34665 and reconstructs XMP 700 plus optional
  IPTC 33723.
  The grayscale matrix freezes the greater-than-four dimension gate, ignored
  border, uint8/uint16 channel-delta thresholds of two/165, and the float 1.01
  ratio with a 0.001 floor. Source/output/ICC bounds, non-finite float sources,
  mismatched RGB8/high-precision pairs, entry/scanline/pre-finalize
  cancellation, real memory-client write/seek/grow/close/finalize failures,
  and source/profile immutability return no encoded result. Catalog tests prove
  default and explicit propagation, JPEG/PNG isolation, no file for invalid or
  invalid options, successful high-precision product renders, atomic conflict
  behavior, cancellation, and source-hash
  preservation. Metadata Catalog tests prove one public snapshot after lookup
  for every rendered JPEG/PNG/TIFF export, TIFF DocumentName only, exact
  baseline plus packet tags, metadata-stage cancellation and injected tag
  failure, and unchanged source plus sidecar hashes, sizes, and modification
  times with no generated sidecar.
  Dedicated CLI tests invoke real Catalog import/export and
  independently parse TIFF tags, Deflate strips, horizontal prediction, and
  exact RGB/grayscale pixels. They cover the `tiff`/`tif` format spellings, all
  TIFF-qualified flags including `--tiff-resolution-dpi` 72/300/9600, canonical values and defaults, argument order,
  last-value-wins value flags, duplicate-grayscale rejection, TIFF-only and JPEG-option scope,
  complete JSON errors, source hashes, successful uint16/float16/float32
  product renders, and zero publication for invalid requests. I7 input and
  ADR-0032
  publication remain separate owners. Domain tests freeze `CaptureDateTime`,
  `CaptureLocation`, and `CaptureMetadata` validation plus exact export DMS
  conversion. Engine tests read `mire1.cr2` as local
  `2007:09:11 13:53:33.18` without inventing an offset or GPS, and independently
  exercise unsigned-rational bounds and ties, tag precedence/conflicts, hostile
  PNG chunks, and bounded errors. Catalog tests cover schema v5 additive
  columns, v4-row NULL preservation, every injected v5 migration stage, v5→v6
  recovery-state migration, atomic
  import rollback, strict new-column storage classes, zero/reference reopen,
  real RAW plus independently built JPEG/PNG/TIFF Exif import, and
  duplicate-import metadata/revision freeze. Encoder and TIFF tests prove one
  `PreparedExportMetadata` derivation, conservative packet estimates,
  JPEG/PNG/TIFF embed, exact TIFF field values, and injected LibTIFF GPS IFD
  lifecycle failures. Sidecar tests and source snapshots prove directory import
  skips `.xmp`, explicit legacy conversion never mutates inputs, Catalog edits
  generate no adjacent file, rendered XMP stays embedded, original-copy remains
  exact, and source/adjacent-sidecar hash/size/mtime are unchanged (ADR-0063).
  Separate schema-v6 recovery tests cover catalog-owned `.ravo.json`
  publication, exact-generation retry/acknowledgement, Develop publication
  after UI preview queueing, corruption rejection, restart drain, and verified
  preview-free backups (ADR-0097). Metadata refresh and privacy stripping
  remain unclaimed S9 work.
- Capture refresh tests modify committed Exif source bytes after import, then
  prove Make/Model/numeric/date/GPS re-read, identity refresh, close/reopen, and
  one revision increment. A forced SQLite revision trigger and entry
  cancellation preserve the old capture row and revision. Export privacy tests
  cover domain mode parsing, disabled-snapshot payload rejection, Studio typed
  conversion/QML/translation, CLI `--metadata`, and JPEG/PNG/TIFF semantic
  parsing: no-location retains time but no GPS; none contains no public
  Exif/XMP/IPTC/DocumentName/ExifIFD/GPSIFD while ICC remains. Original-copy
  rejects stripping and publishes nothing.
- RecipeStyle tests cover deterministic complete schema-v1 serialization and
  selective schema-v2 serialization, bounded name/description/file size, exact
  placeholder identity, complete operation/mask/Retouch/bypass round-trip,
  target-only identity replacement, and selected-field overlay that preserves
  every omitted target value. Empty, duplicate, unsorted, unknown, wrong-type,
  newer, malformed, and legacy-dtstyle state reject structurally. CLI
  create/validate/apply uses real files, conflict behavior, and explicit target
  Recipe validation for schema v2. Studio tests pin baseline-relative candidate
  inventory, an initially empty checkbox selection, **Save…** to the right of
  **Import…**, managed-folder pre-existing conflict plus atomic complete output,
  stale-field rejection, and apply through the existing recipe/history/undo
  transaction rather than a parallel preset store. The same selection dialog
  and Recipe merge owner cover session Copy/Paste Parameters. Translation
  catalogs and offscreen smoke cover the dialog. ADR-0072 removes all bundled
  `.dtstyle` examples and their exclusive translation generator; the parser's
  whole-format rejection remains the test truth rather than per-example
  conversion tests (ADR-0065/0098).
- Studio settings tests isolate QSettings storage and prove corrupt persisted
  language removal, English fallback, alias normalization, synchronous
  persistence, unsupported-language rejection, and preservation of the prior
  durable value. Translation package smoke separately proves every manifest
  catalog compiles with no active unfinished strings (ADR-0066/0093).
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

The opt-in `PresetPerformanceProbe` exercises the real CatalogService path
without touching the source catalog: copy a catalog to a private temporary
location, then set `RAVO_PRESET_PERF_CATALOG`,
`RAVO_PRESET_PERF_ASSET_ID`, and `RAVO_PRESET_PERF_XMP` before running the
probe. It reports parse, save, first-preview, settled-preview, and total
milliseconds. Optional `RAVO_PRESET_PERF_FIRST_PREVIEW_BUDGET_MS` and
`RAVO_PRESET_PERF_BUDGET_MS` turn those measurements into explicit local
gates; the test skips when its fixture variables are absent, so host-specific
timings do not make the normal contract suite flaky.

The opt-in `InteractivePreviewPerformanceProbe` warms both live and settled
working slots, then measures a non-persistent Develop parameter sweep through
CatalogService and includes the RGB ownership copies plus histogram work used
by its service-level result. The corresponding
`StudioInteractivePreviewPerformanceProbe` measures from the Presenter numeric
intent through publication of the owned live `QImage`; SHA-256 and scopes are
intentionally a separate latest-only stage and their exact final identity is a
functional contract. Its rapid-intent case injects one-millisecond slider
updates and reports first-frame latency, latest-intent latency, published-frame
count, and maximum frame gap. Run the probes from a Release build against a
private catalog copy with
`RAVO_INTERACTIVE_PERF_CATALOG` and `RAVO_INTERACTIVE_PERF_ASSET_ID`; optional
`RAVO_INTERACTIVE_PERF_MAX_EDGE`, `RAVO_INTERACTIVE_PERF_RUNS`, and
`RAVO_INTERACTIVE_PERF_P90_BUDGET_MS` select the size, sample count, and visible
P90 gate. `RAVO_INTERACTIVE_BURST_RUNS` and
`RAVO_INTERACTIVE_BURST_BUDGET_MS` select the burst length and cap both its
first and latest owned-image response. `InteractivePreviewQualityProbe` uses the
same fixture variables to compare the 960px and former 640px complete pipelines
with the 1600px settled display pixels;
`RAVO_INTERACTIVE_QUALITY_MIN_PSNR_DB` adds a local PSNR gate. Both probes skip
without explicit fixture variables and leave recipe and preview-record state
unchanged.

`RAVO_TRACE_PREVIEW_PRESENTATION=1` enables a read-only Studio trace that joins
the presenter's interactive intent timestamp to the next native
`QQuickWindow::frameSwapped` after owned-image publication. It records both
intent-to-image and intent-to-frame-swap microseconds. Use it only with a fixed
display refresh rate and power state: swap time is quantized by the display and
is reported separately from the 5/10 ms CPU/presentation-publication gate. The
trace neither changes scheduling nor supplies a fallback renderer.

`PerspectiveInteractivePerformanceProbe` uses the same Release-only catalog
and asset variables, warms the normal CatalogService working buffer, sweeps
manual vertical correction without saving, and includes owned RGB8 plus
histogram publication work. `RAVO_PERSPECTIVE_PERF_MAX_EDGE`,
`RAVO_PERSPECTIVE_PERF_RUNS`, and
`RAVO_PERSPECTIVE_PERF_P90_BUDGET_MS` select its workload and visible P90 gate.
It skips without explicit fixture variables and verifies that recipe and
preview-record state remain unchanged.

`LocalDetailResearchProbe` is the reproducible selection record for bounded
Texture versus the rejected Local Laplacian prototype and the already accepted
Sharpen/Tone Equalizer semantics. Run a Release contract binary with
`RAVO_LOCAL_DETAIL_RESEARCH=1`. It reports texture gain, mean movement, step
halo, prototype memory and timing on both committed RAW fixtures, then applies
the production Texture owner. The production operation must remain below
30 ms for each 960×640 working buffer; this is its complete algorithm budget,
not a claim that service and display latency are zero.

`FilmDevelopmentResearchProbe` is intentionally test-only. With
`RAVO_FILM_DEVELOPMENT_RESEARCH=1`, it runs the Ravo-owned reaction, diffusion,
reservoir and agitation prototype on the same committed RAW buffers and reports
stage medians, extra peak bytes, spatial-context response and agitation
difference. Its current result rejects product integration; the probe must not
be linked into Engine or used as a hidden slow fallback.

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
