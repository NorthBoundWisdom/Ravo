# Ravo Testing Strategy

Video contracts use the generated CC0 corpus in `Ravo/tests/fixtures/video`;
`Ravo/tools/generate_video_fixtures.py` owns its media and hash manifest.
`VideoDecoderTest` covers H.264/AAC, HEVC HLG/PQ, display matrices, timestamps,
cancellation, corrupt/missing input and plane bounds. `VideoColorTest` checks
finite monotonic SDR mapping. `VideoCatalogTest` covers atomic metadata/import,
schema 17→18 migration, transaction injection, Move/source hashes, duplicate
import, recipe rejection, source change, original copy, reopen and backup/restore.

Synthetic auxiliary-audio MOV fixtures cover AAC alongside an unknown `apac`
entry, type-1 cover metadata, and excess stereo channel descriptions. They are
not APAC-encoded camera samples. Decoder tests require stable diagnostics,
unchanged originals, working frame decode, no repeated known-warning output,
and explicit rejection when no supported audio track exists. Corrupt-container
errors must still reach both stderr and structured failures, including concurrent
probes. The full-range gray fixture checks midtone values and avoids deprecated
YUVJ format warnings by supplying an explicit range to swscale.
`ImportCandidateListModel.MixedMediaTotalsTrackBatchesChecksFailuresAndReset`
checks complete versus checked media counts/bytes across scan batches,
duplicates, unsupported candidates, MIME updates, selection and source reset.
The production QML smoke checks complete versus checked summaries and capacity
units while changing checks, including every manifest locale.

`CatalogServiceTest.ImportWork*` checks hash observations before publication,
unchanged completion callback semantics, and cancellation during a partial
hash with no catalog/output publication. `StudioImportRoundtrip.ProgressIsObservableBeforeItemsCompleteAndThroughRealCli`
checks preflight visibility and reads versioned progress/candidate state through
the real CLI and local control server. Existing import conflict, cancellation,
transaction, duplicate and destruction contracts remain required.

P3 D65/BT.601 fixtures exercise full and limited range, SMPTE 170M and BT.470BG
aliases, and a rejected YCgCo matrix. Tests check recovered nonlinear RGB code
values, an independent Qt P3-to-sRGB transform, catalog metadata/preview reopen,
strict unknown-matrix rejection and old v1 metadata without a matrix. Muted Qt
playback must match owned poster pixels for both ranges and resolve old metadata
from the source before playback. ProRes fixtures require frame-header colour
resolution, zero-time decode, seek and owned Qt playback matching the poster.
Native 4:2:2 checks require full-height chroma and reject short buffers. P016
checks require complete interleaved 16-bit UV and preserve expected RGB values.
Original fixtures are owned by the generator.

`ravo_video_software_playback` repeats the unchanged poster/playback pixel
contract with Qt's hardware decoder list explicitly empty, so hardware
availability cannot conceal software YUVJ range-normalization errors. The
ordinary test remains enabled too. `test_video_dependencies.py` exercises the
production Windows CMake resolver with isolated DLL/tool fixtures, including
missing runtime, empty exports and failed tools. Native MSVC CI validates real
DLL export inspection, generated import linkage and execution. Linux CI and
clean-package hosts install the declared PipeWire/VA-API dependencies; CLI
stdout/stderr assertions are not loosened or filtered.

`StudioVideoTest` uses the production C++ controller and real CLI subprocesses
for play/pause/seek/volume/mute, owned-pixel comparison with the shared HDR
renderer, view release and cancellation during close. No UI automation or
screenshots are an oracle. `--smoke --video-smoke-input <file>` exercises the
shipped Qt backend and frame path with bounded time and muted output, returning
`ravo.video_playback_smoke/v1`. The packaged-runtime checker requires video
import, verified immutable frame and playback stages; audible output and
real-corpus quality remain separate native acceptance.

`StudioLibraryPaging` allows 60 seconds for opening its 205-file test catalog
under parallel Windows CI load. This is a setup budget, not a performance claim;
page-resolution waits and unloaded-row/selection assertions retain their original
contracts. Open failures report catalog-open, busy, visible-count and error state.

Lightroom import contracts construct SQLite catalogs with the vendor table
relationships and verify source hashes, metadata after reopen, missing files,
active journals, invalid references, cancellation and destination conflict in
`catalog_lightroom_test.cpp`. This is synthetic structural coverage, not vendor
release certification. Real private catalogs, large-catalog memory/disk pressure
and platform-specific volume remapping require separate evidence (ADR-0164).
ADR-0166 adds database-only Develop, independent virtual-copy recipes,
catalog XMP, named history/snapshots, collection membership, source archive
hash/export/reopen and cancellation/rollback contracts. Source preservation is
not evidence that an unsupported control renders correctly. Schema 19 and
ordinary backup/restore must retain verified archive bytes.
Catalog XMP contracts cover plain and declared-length zlib packets, while empty
current settings remain unedited. Read-only CLI inventory must leave source
hashes unchanged, require no destination and bound diagnostic/sample payloads.
Virtual-copy contracts must cover empty edits, tags, labels and writable text
independently of a modified master. History-only conversion must publish a
verified recovery artifact and restore through the ordinary snapshot owner;
prepared snapshots reject a mismatched asset/source before writes.
ADR-0167 contracts cover per-record checkpoint rollback and backup, reopen
resume without duplicate history/versions/sets, ambiguous partial records,
destination revision conflicts and real CLI journal inspection. Final source
audit cancellation, disappearance and observed changes must retain committed
receipts, with no false `originals_unchanged` success.
Schema 21 contracts additionally exit a real subprocess after import, rating,
metadata, history snapshot, Develop and collection commits, before receipt publication; reopened
journals must retain the proven target and allow untouched work to continue.
Second-connection writes must not be adopted as task progress. Proof insertion
failure rolls back the business data/revision, and exceptional stage callbacks
close their connection without leaking provenance into later writes. Collection
tests restore a missing original, reuse the same set, add no duplicate members,
retain explicit subset boundaries and reject manual destination edits. Schema
20 upgrades retain wide 64-bit revisions without inventing old commit proofs.
`SingleReviewPatchesRollbackDataAndRevisionTogether` covers all four single-photo
commands, revision-failure rollback, unrelated-field preservation and stale guards.

`StudioPipelinePriority.RecipeLoadRejectsOldAAfterABAAndSessionReplacement`
drains an old worker read while delaying GUI publication, then uses a second
worker barrier to prove old A results cannot complete a replacement load.
History-only read failure and a stopped executor must leave Develop unavailable.
The test's private SQLite fault is confined to its temporary catalog.

ICC resource measurement uses scoped caller-owned `DisplayConversionObserver`
sinks; concurrent callbacks are noexcept and all workers join before return.
No process-global collector, worker pool or rendering policy is introduced.
Run the measure-only `DisplayPresentationResourceProbe` and
`StudioDisplayResourceProbe` in `mac_clang_release` with
`RAVO_DISPLAY_RESOURCE_PROBE=1` and optionally
`RAVO_INTERACTIVE_PERF_REPORT_PATH=<new.jsonl>`. Reports include P95/P99,
created ICC threads, aggregate active participants for concurrent transforms,
and intent-to-publication samples with/without real import work. The Studio
case requires a system monitor ICC different from sRGB. Offscreen publication
is an owned-pixel handoff measurement, not physical display scanout latency.
Resource measurements alone do not admit a global scheduler replacement.
Observer peaks count ICC participants in the measured workload. Sampled native
process thread counts also include idle Qt/Engine workers and must be reported
separately. Repeat tail-latency measurements under comparable load and power
conditions before attributing a change to thread creation.

Release artifacts also run on fresh CI runners without the build bootstrap or
Qt SDK. `ravo_studio --startup-smoke -platform cocoa|windows|xcb` loads the
production root, presents a native frame within 15 seconds, emits
`ravo.native_startup` v1 JSON, and exits through ordinary owner destruction.
It rejects offscreen; the existing offscreen `--smoke` interaction suite remains
separate and required. [Packaging.md](Packaging.md) owns the runner matrix,
package install/launch lifecycle, evidence and remaining host qualifications.
The Python checker tests cover loader environment isolation, first-frame
evidence, timeout/nonzero failure, direct AppImage execution and DEB cleanup.

`StudioControls.*` scans all production Studio QML for unowned Qt visual
widgets and attached default tooltips. It loads the production Import page,
delivers keyboard and pointer input through a real offscreen QQuickWindow, and
requires live thumbnail sizing, disabled-state rejection and destruction.
Shared-component contracts require scroll position propagation, live theme
colors, bounded busy visibility, keyboard menu activation, one-press Escape
cancellation of text dialogs and popup destruction. Test engines register the
same static GeoControls resources, qrc import root and Basic input foundation
as Studio.
The complete production-QML smoke also covers the shared controls in every
supported locale; no desktop automation or application screenshot is used.

Studio's production-QML smoke checks that the Develop tool strip remains fixed
while the inspector scrolls, exposes mask creation in the local workspace, and
draws gradient geometry and completes mask editing before entering Crop. It checks Crop and Edit selection
against presenter state and verifies the pinned crop controls remain available.
Expanding mask settings must overflow the bounded local-tools viewport; scrolling
it and the adjustment stack independently must preserve both viewport positions
and their separate scroll offsets, as well as the fixed toolbar position.
The same smoke requires toolbar labels to fit their standard buttons and opens
the shared mask-creation menu with all five actions. Locale smoke repeats these
checks with every packaged translation.
`StudioQmlContract.ExposureAndColorBalanceInstanceChrome` requires the global
instance controls and rejects the retired Advanced menu and visibility flag
in the Develop stack.

`LocalAdjustmentWorkspaceTest` checks geometric guides for gradient, circle,
ellipse, path and brush masks after photo rotation, and retains those guides when
coverage is disabled. Parameter-preview assertions require uncolored published
pixels, unchanged guide coordinates and overlay preference, timed restoration,
and the existing save/reopen invariants.

Studio's curve gesture smoke sends pointer and wheel events to the production
ToneCurveEditor inside a real Flickable. It checks changed point coordinates with
unchanged scroll position, one commit on release outside the plot, resumed wheel
scrolling, cancellation without commit, preservation of an initially disabled
scroll owner, and restoration when the editor is destroyed mid-press.
The resumed NoScrollPhase wheel assertion observes `contentYChanged`, with a
bounded failure deadline, rather than assuming one fixed event pump contains an
animation tick. Coordinate, commit, cancellation and restored-scroll assertions
and the executable's overall smoke deadline remain unchanged.

`StudioCommands.GalleryExposureRefreshesGridThumbnailPixels` exercises the
Gallery exposure commands against real thumbnails with an injected monitor
profile. It requires model notifications, darker pixels after −1 EV, restored
pixels after +1 EV, bounded thumbnail dimensions and the latest pixels after
rapid repeated edits, without leaving Grid or manually requesting another thumbnail.
The same test rejects transient empty model URLs during refresh. Production QML
smoke reloads a real ThumbnailCell asynchronously and requires uninterrupted chrome
visibility and retained gutter geometry, then verifies clearing the source removes
the retained frame.
`StudioInspectFrame.NavigatorExtentIgnoresPreviewRoundingAndResetsForGeometry`
checks thumbnail/settled size rounding, real aspect changes, reselection and clear.
`StudioInspectFrame.PendingThumbnailCorrectsBothViewportsWithoutReplacingPublishedFrame`
requires pending portrait/landscape/square thumbnails to correct both viewport
aspects without replacing a published preview. The real-catalog
`StudioPipelinePriority.ReselectCroppedPhotoUsesThumbnailAspectBeforeFullPreview`
blocks full-preview work during rapid reselection and requires the cached cropped
thumbnail's aspect immediately, followed by the final cropped frame.
Production QML smoke checks decoded `paintedWidth`/`paintedHeight` on both
navigator Images and the main loading placeholder against the selected photo
while the destination geometry changes between landscape, portrait and square.
Production smoke also watches both navigator and viewport-border scene geometry
through four Gallery exposure commands and requires no movement.
At every preview publication in that loop it also requires the navigator to
use the selected Grid thumbnail URL and keep its GPU presentation disabled.
The separately decoded navigator hold image must never become visible in Grid,
including during asynchronous replacement.

`StudioCommands.WritableMetadataAcceptsEveryDomainFieldAndRejectsInvalidInput`
checks all fourteen writable fields through command dispatch, including clearing,
multi-field patches, unchanged-field preservation and whole-patch rejection for
invalid input. Catalog IPTC contracts cover transactions and reopen.
Localization contracts check the native Settings-role prefix and command dispatch
after each supported language switch. Production Studio smoke checks live Light
slider values and the 30% long-edge viewport ratio in landscape and portrait
windows, plus return from 1:1. Native trackpad gestures require host evidence.

`StudioDisplayPresentationTest` covers thumbnail cache loss before catalog open
and deterministic eviction between listing and display dispatch. Recovery must
publish monitor-corrected pixels without a transient error, preserve original
bytes, classify an unavailable original as photo-level missing state, and stop
after one repair if its output is evicted again. Existing presentation contracts
also cover listing replacement, owner destruction, cache corruption, and output
publication conflicts. The 200-row folder/cache fixture drains the full
monitor-change batch before taking the publication lock, so it tests an
intentional competing publisher rather than racing its own remaining rows.
It also requires visible demand to finish within the four dispatched background
tasks, repairs a failed decoded URL, rejects stale URL failures and bounds repeat
repair. `GalleryCacheReusesIndexAndObservesOtherPublishers` checks index reuse,
cross-owner eviction under a shared byte budget, cancellation and invalid epoch
errors. Release measurements record cold-page and warm-reopen timings on the same
200-image fixture; they are synthetic cache/scheduling evidence, not a private
RAW corpus or removable-drive throughput claim.
`PagedThumbnailUsesPresentationAndEvictionRecovery` loads a cached thumbnail
past the initial 200-row page, evicts its input at display dispatch, and requires
recovered monitor-corrected pixels with unchanged originals. The production QML
smoke imports a synthetic photo and requires the Gallery cell's filename, image
URL, and `Image.Ready` state, rather than treating root creation as image readiness.

`StudioLibraryPaging.*` contracts cover the 199→200 sparse-page boundary,
repeated navigation, placeholder row selection through the command controller,
and rejection of queued selection after a new selection or query. Filmstrip
uses the shared metadata-page owner before requesting thumbnail pixels.

## Multi-photo merge contracts

ADR-0163 uses `PhotoMerge.*` for numerical HDR clipped-highlight/chromaticity
truth, exposure and mild-projective registration, moving-pixel rejection,
panorama exposure/seam/crop coordinates, unordered connected inputs, and
malformed/featureless/cancel/memory failures. `CatalogServiceTest.PhotoMerge*`
covers immutable TIFF/provenance, input hashes, output conflicts, missing EVs,
cancel after file publication, import-transaction failure, concurrent revision,
reopen/preview/re-export and backup verification. CLI and Studio merge tests
cover versioned artifact JSON, revision requirement, stale dialog selection,
option validation, availability and owned-window destruction.
Production `ravo_studio --smoke` also opens the 16-photo dialog at 960×640 and
1280×800, checks bounded scroll/body/footer geometry and modal command isolation,
then closes it without issuing a merge mutation.

```text
ctest --preset mac_clang_debug -R 'PhotoMerge' --output-on-failure
cmake --build --preset mac_clang_debug --target RavoCodeQuality
```

Run the full Ravo unit/contract suite for the extended public import transaction.
Synthetic fixtures do not establish real bracket/panorama corpus quality,
native-resolution performance or installed Windows/Linux/macOS release gates.
Tests use generated artifacts and original hashes, never Studio screenshots.

`StudioPresenterTest.VersionsStacksAndSurveyUseSerialBrowsePreviews` covers
unstacked Burst Compare rejection without changing selection/error state,
shortcut availability, collapsed-stack stepping, ordinary Survey, leaving
compare mode, and stack dissolution. Service burst tests retain structured
failures for missing/singleton/dissolved stacks.

`CatalogServiceTest.ExportJpegPngOriginalCopyConflictAndCancel` also verifies
companion JPEG exact-byte export, missing companions, output conflicts,
cancellation and unchanged RAW/JPEG source hashes.

The same service case checks bounded JPEG file size, unchanged pixel dimensions,
unreachable limits, cancellation and conflicts. `OriginalCopyCliTest` covers
`--jpeg-max-bytes` JSON success/failure and companion preflight type errors.
`ExportWorkflow` checks desktop companion preflight signal routing; Studio's
QML smoke checks the size-limit form and missing-companion confirmation wiring.

`StudioPresenterTest.ColdCatalogBuildsOnlyDemandedThumbnails` admits requests
synchronously from `modelReset` and requires every demanded cold thumbnail to
finish without another viewport event. The sparse-page model contract checks
that cancelled `presenting` states return to pending while ready states survive.

## Current evidence baseline

Frozen 0.9 assets are used only for static evidence:

- `Ravo/tests/fixtures/frozen/` contains 158 XMP + `expected.png` fixture sets and five
  source images.
- Fixtures cover 68 operation names; that number is an asset inventory, not
  Ravo coverage.
- Those fixtures are read-only static evidence. They are not a leftover-faithful
  algorithm port queue (ADR-0106).

Boundary checks:

```text
python3 Ravo/tools/freeze_legacy_manifest.py --check
python3 Ravo/tools/check_ravo_dependency_boundary.py
```

The dependency-boundary checker covers the product target graph:
`Qt6::Sql` is adapters-only, `Qt6::Gui` is raster adapters, desktop, and the
Engine QRhi GPU adapter,
`Qt6::Network` is control/CLI-local-socket or desktop-only, `Qt6::Qml`/`Qt6::Quick`,
`QtQuick.Controls`/`QtQuick.Dialogs`/`QtQuick.Layouts`,
`GeoControls`/`GeoControls.AppShell` imports, and production `.qml` are
desktop-only; every Ravo target rejects Qt Widgets. Do not remove a check to
admit a new dependency.

The current Ravo Debug graph covers foundation/recipe/engine/CLI and catalog
integration, including first-frame Bayer RAW/DNG errors and preview-cache miss
rebuild. Review contracts include schema v2→v11 migration, review
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
explicit preview rebuild. Windows atomic publication uses `\\?\` extended-length
paths so nested derived-tree restore temps can exceed `MAX_PATH`. Schema v7 page tests compare every accepted filter
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
workspace selection, highlight versus import-check, named placeholder publication before thumbnail decode,
named Gallery placeholders as soon as Import starts, and cancellable
background preview policies (ADR-0102/
0104). Import-scan tests cover catalog preview/support-tree exclusion through
scan and execution, explicit descendant inputs, POSIX symlink aliases,
similarly named ordinary photo folders and source preservation. They also cover
same-path, renamed/copied content, same-batch
duplicates, equal-size/different-byte inputs, source mutation, revision conflict,
cancellation, and v16-to-v17 index migration/backfill. The derived hash table
must preserve catalog revision, source bytes, and edit history. CLI subprocess
tests exercise `import-scan` and `import --skip-existing` against real files.
`DomainUriTest.FileModificationIdentityIsStableAtMillisecondBoundaries` proves
that repeated reads cannot manufacture a source-change conflict from clock
sampling. HEIC adapter/catalog tests (ADR-0159) generate non-private oriented
and Display-P3 images on macOS, compare owned path/memory pixels, preserve
source bytes and dimensions, and reject truncated/oversized/cancelled input.
Other hosts assert an explicit unavailable decoder. HEIC stays a primary-photo
SDR input with the same catalog and preview/publication owners as other rasters.
`ImportScanVerifiesKnownContentAcrossTimestampOnlyChanges` covers verified
SHA-256 identity after metadata-only changes and rejection of actual byte
changes; neither branch revises catalog or recipe state.
Desktop tests isolate QSettings and cover Copy defaults, opt-in filename
composition, ordered parts/separators, extension preservation, unchecked/Add
original-name behavior, invalid choice rejection and preflight locking. Production
offscreen smoke checks preview/rename/second-copy/destination ordering, the
non-collapsible destination section, Add hiding its tree, and the read-only
filename example; Copy/second-copy integration verifies renamed output bytes.
Second-copy checkbox contracts cover missing-path rejection, retained disabled
paths, Add omission, preflight locking and unchecked Copy publishing only the
primary while preserving source bytes.
They also cover streaming check intent,
visible disabled duplicates, single/range/all selection exclusion, duplicate
thumbnail completion without rescan loops, and source-byte preservation,
source persistence before import, source ancestor selection/reveal after page
reopen and cross-catalog restart, one-shot expansion of the restored source,
manual source changes while storage discovery is blocked, tree reset clearing
selection and rejecting old listings, unavailable saved-source removal and explicit
errors with collapsed roots and no automatic scanning of another folder, invalid source
preferences and failed-write preservation, destination and organization persistence
without importing, organization value validation and failed-write preservation,
restart/cross-catalog restore, unavailable
destinations, settings-write failures that preserve committed photos and the
prior destination, conflict-before-import with Gallery error feedback while
retaining the chosen valid destination, asynchronous ancestor reveal
after all planned-branch model resets,
superseded reveal cancellation, and visible directory-listing errors. Folder
contracts cover Home and mounted-volume roots in both panels, volume refresh
without losing expanded children or selection, removal/reinsertion with late
listing rejection, unique paths across overlapping roots, accessible absolute
storage discovery, activation/expansion, pending-list
deduplication, external picker paths, and repeated destination selection without
losing loaded children; QML contracts check independent disclosure/selection areas. The
mounted-root discovery test accepts `RAVO_TEST_EXPECT_MOUNTED_ROOT` for an
independently identified mounted card path; it verifies discovery without scanning
or importing the card's photos.
The import contracts also cover the checkbox's explicit click intent, non-recursive
selection across folder changes/page reentry, late recursive-result rejection,
and the Home recursion guard for normalized and symlink paths. The
destination-preview tests check single/date/month/hierarchy and second-copy
counts against actual import, no early directory/media/catalog publication,
selection/organization replacement, close cancellation, and stale metadata/directory
blocker errors. Path-only projections defer corrupt-image validation to preflight;
date-based plans reject corrupt metadata. Metadata-only preview accepts an unverified
content hash and existing output file while formal preflight rejects them; CLI subprocess tests
verify provisional v2 counts followed by actual content-deduplicated import.
A blocked import-worker test requires destination planning to finish and close
cleanly on its independent session. CLI subprocess tests verify the versioned
`catalog import-plan` JSON.
Partial-projection contracts check the first photo's counts, primary/second-copy
folders, cancellation after a partial result, stale catalog rejection and no
destination writes. RAW date/month projections are checked against actual import.
Set `RAVO_IMPORT_PREVIEW_SOURCE` to an explicit read-only directory when running
`CatalogServiceTest.DestinationPreviewPerformanceProbe` in a Release build to
record first-result latency, complete-plan latency and update counts.
Blocked scan and destination-planner tests require Cancel/Escape to abandon the
current source while keeping Import open, reject late publications, and allow a
child folder to be selected without reopening the page. They verify unchanged
source bytes and no destination/catalog publication. The production offscreen
smoke invokes the modal Cancel button through its QML command binding and checks
that the source is cleared and Import remains open.
The 1025-photo workspace regression blocks destination planning and thumbnail
decoding independently: enumeration must release the window, pending plans must
reject import, published trees remain visible without an immediate model reset,
and partial/replacement counts must publish while thumbnails are still blocked. The
production smoke also invokes source-tree context intent and Copy Path, checking
the clipboard and that no scan or source selection occurs. File-manager launch
tests validate platform arguments without launching a user's file manager.
Filesystem model tests cover ordered month overlays, existing-year/month deduplication,
collapse/expand, virtual-folder selection rejection, late listings, plan replacement,
second-copy exclusion and explicit listing errors without retry loops.
Service cache coverage distinguishes provisional same-size/time metadata reuse
from formal byte admission, invalidates changed timestamps, refreshes current
catalog membership, and rejects cancelled/closed requests. The opt-in
`CatalogServiceTest.DestinationPreviewPerformanceProbe` takes an explicit
`RAVO_IMPORT_PREVIEW_SOURCE` and records cold month, warm month and warm date
planning times without importing or creating planned directories.
Desktop worker-gate coverage holds the destination planner, verifies immediate
interaction blocking and rejected early import/commands, exercises command
cancellation, and reopens without accepting late results. Production QML loads
the modal planning/cancel controls alongside the real destination tree.
Deferred
Home/ancestor listing coverage requires the planned month to be revealed without
selecting it, with correct counts on both real and future directories; clearing
the plan removes the counts and virtual rows. Presenter
coverage checks that the month plan appears in the destination tree and disappears
on page close while the directory remains absent on disk. Production QML smoke
checks the gray, italic month label alongside the normally styled existing year,
their inline planned counts, and the month's position inside the tree viewport.
`StudioGpuPreviewTest` verifies that delayed delivery retains the original pixels
after surface reuse at 639×960 and 1066×1600, and checks cancellation, invalid
surface and dimension failures. With `RAVO_TEST_GPU_RAW` set to an explicit RAW
path, it also compares native portrait publication with CPU gold, verifies the
source hash, and exercises the real asynchronous Loupe presenter in a temporary
catalog. `RAVO_TEST_GPU_OUTPUT_DIRECTORY` optionally receives new CPU/GPU/Loupe
PNG artifacts for inspection.
`StudioLibraryProgress` covers small batches, batches completed before the reveal
delay, sustained work, the low-count tail and rapid replacements. Production QML
smoke also measures the Library header height and navigator position across
hidden, visible and completed thumbnail-progress states.
`StudioBackupSettingsTest` covers command-owned Settings drafts, save failure,
invalid inputs, late folder selection, matching-policy conflict recovery,
real backup execution, disable/reopen and
CLI policy readback. `WindowGeometryTest` covers panel reset persistence.
Production Settings smoke loads all four categories at narrow and wide sizes;
it checks native folder URL conversion with Unicode, spaces and escaped path
characters. Localization smoke repeats the production load for every manifest locale.
The ordinary Studio QML smoke logs after presenter destruction has cancelled
and joined its workers, so an early logging shutdown fails the real executable
smoke. It checks a temporary directory's real disclosure handler,
expand/collapse result, and hit-area dimensions. It also activates a
loaded collapsed directory row and verifies
that the folder choice reaches the presenter despite synchronous delegate replacement.
Import grid/action/panel bounds are exercised at
1440×900, 1440×1100, 1024×640, and 640×480 without screenshots or a user catalog.
The smoke also transitions the menu bar between zero and visible client-area
height and checks that Import starts immediately below it, with no stale
automatic top inset. Native system-menu objects and commands remain present.
The smoke checks equal-width destination mode segments and C++-owned Move
availability, matching source/destination tree surfaces, inline month/count styling and final scroll
visibility with dozens of Home siblings and deferred ancestor listings, absence
of a separate preview list, adaptive tree growth and collapse/Add sizing,
and a real checkbox indicator of at least 24 pixels within a 32-pixel hit area.
`StudioImportRoundtrip.FolderMovePreservesDestinationBytesAndReopensCatalog`
covers ordinary folder Move through the workspace, spaces in paths, exact
primary/second-copy media and XMP bytes, source removal and catalog reopen.
`StudioImportWorkspace.MoveCancelAndConflictPreserveSourceAndCatalog` gates
the worker to check cancelled preflight and destination conflict without source
deletion or catalog publication. `MoveCleanupFailureSurfacesWarningAndKeepsCommittedAsset`
changes XMP at publication and requires retained source bytes, a committed asset
and visible cleanup error. `StudioControls.DisabledImportMoveShowsReasonOnHoverAndCannotBeClicked`
loads the production destination panel and sends window mouse events to verify
the disabled Move tooltip, click rejection, hover exit, re-enabling for folder
import and tooltip destruction. The ingest-policy roundtrip covers rejection
and return to ordinary folder import for every accepted transport.
The preview fixture waits for the model's final leaf reveal with a bounded
deadline before inspecting delegates; a fixed delay is not listing completion.
Enumeration callbacks are tested before classification and for cancellation
without publication. Catalog-independent PNG/RAW thumbnail decode must match
Catalog pixels and preserve source hashes and structured corrupt/missing/cancel
errors. Desktop worker gates prove that all named placeholders precede
classification, 96 reverse-demanded rows drain in ascending order even with the
catalog worker blocked, and real import completes while workspace thumbnails
are blocked. Cache tests cap retained images at 256 and preserve scan-owned
eligibility across thumbnail completion/failure. Select All tests freeze one
command shortcut, import-vs-Gallery routing, duplicate exclusion, and text-input
isolation. `StudioImportWorkspace.RealSourceProgressProbe` is opt-in via
`RAVO_IMPORT_SCAN_SOURCE`: it uses an isolated temporary catalog, reads only the
explicit source folder, requests the first 32 thumbnails, and reports candidate
count plus enumeration/first-image times and classification progress as GoogleTest
properties. It cancels remaining classification after the first viewport finishes;
`scan_ms=-1` means classification was still active, not a failed image result.
Run this probe from a Release desktop test binary with `--gtest_output=xml:<file>`;
it never executes import or uses a user's existing catalog.
LibraryQuery tests cover every
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
(ADR-0101). Studio folder-tree tests cover right-click import, reveal, missing
relink, expand/collapse, and catalog-only folder removal that keeps originals;
`catalog folder-remove --folder-uri` shares that service contract.
Schema v10 tests create manual membership and smart `LibraryQuery` sets,
reject stale catalog revisions and invalid/unknown IDs, keep empty sets, page
listing without materializing the catalog, reopen and restore the rows through
verified backup, and pin Studio/CLI/QML wiring (ADR-0103).
Schema v11 tests create a virtual copy that shares the original URI, isolate
recipe/export of the version, refuse disk deletion of a non-primary, cascade
catalog deletion of versions with a primary, collapse stacked non-picks from
the default page total, expand on request, migrate v10 catalogs, reopen and
restore stack identity, and pin Survey as a browse mode that does not start N
Develop pipelines (ADR-0105).
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
Actual-size inspect ROI follows live Develop parameters without requiring a pan.
Its published URL must resolve to a non-null owned image, and leaving actual-size
mode clears both URL and image. QML contracts require the hidden CPU ROI item to
clear its source when the GPU display item owns presentation.
The CFA-window linear working is reused across RGB-only edits and rebuilt on
pan or preprocess change.
`InteractiveGpuSourceTracksSameSizeCacheReplacement` checks same-size source
replacement, alternating caches, cache recreation and moves against CPU gold
with and without a cached prefix. `SameSizeInspectPansMatchTheirCpuExportRegions`
compares successive real RAW viewport pixels to their full CPU export regions,
returns to the initial viewport, and preserves source hash/catalog/recipe state.
Catalog tests cover Bayer viewport-ROI 1:1 windows, full-frame ROI rejection,
and geometry rejection (ADR-0132).
Engine GPU adapter tests require `create_phase1` to succeed whether or not a
device exists, honor cancellation before dispatch, and reject size-mismatched
opt-in copies. When QRhi reports a compute backend, identity copies are
bit-exact and Exposure affine RGB, unmasked light controls, Lab USM Sharpen,
RapidRAW global tone controls, and the active display mapper (Sigmoid for
synthesized RAW baselines, RapidRAW Basic for explicit stored recipes) are RMSE-gated against the CPU gold.
`render_interactive_linear_working` reports `gpu_backend` when those GPU RGB
passes ran, including the default RAW baseline that keeps Sharpen and Sigmoid on the GPU. `render_linear_working` and export stay on CPU even when a
compute backend exists. Recipes without those ops stay on CPU. A later smaller
upload must not over-read a grow-only SSBO. Retained-source RGB apply matches
the uploaded path. Interactive skip-download on Metal publishes a non-zero
display generation and native surface with empty CPU RGB, including odd widths
whose IOSurface `bytesPerRow` is 16-byte aligned. Studio pure-interactive
Develop skips the synchronous float readback, then copies owned RGB8 from that
same completed IOSurface for exact live identity, scopes, and headless presenter
tests. Invalid surface identity/layout fails structurally. Comparison, overlay,
persisted preview, and non-native paths still request ordinary CPU RGB. Hosts
without compute or buffer readback, and hosts whose QRhi device cannot build the
preview pipelines, return `gpu_unavailable` or `gpu_pipeline_failed`. Bayer
window RCD is RMSE-gated against the CPU gold when the adapter is present; PPG
and hosts without a working compute backend stay on CPU. Export stays on the
CPU path.
Progressive-preview coverage uses a source larger than the display preview and
requires live drag pixels and viewport to retain the settled 1600px resolution.
`DisplaySizeLiveEditKeepsResolutionAndPreparedRawSource` verifies source reuse,
native GPU display dimensions, CPU-gold pixels and unchanged catalog/recipe
state. Explicit low-edge requests remain separately bounded.
`CroppedPreviewUsesFinalPhotoDensityAndReopensExactCache` verifies crop
density against CPU rendering, RGB-only source reuse, exact cache dimensions
after reopening, and unchanged source/recipe bytes. Perspective contracts
check the same integer geometry sizing, native limits, bypass, and cancellation.
`StudioCropTest.AutoLevelChangesOnlyRotationAndRejectsInvalidModes` uses tilted
line pixels to verify rotation and preserved perspective/source bytes. The
production QML smoke enters crop, checks the expanded controls above the Develop
scroller, half-surround layout, independent scrolling, and hiding on exit.
Engine guide-fit contracts verify rotation-only level fitting, cancellation and
missing guides; CLI structured analysis accepts `--mode level` without writing
the source. Desktop exposes the same mode through the shared command owner.
A saved-edit reopen
case enters Develop before the asynchronous Recipe callback and requires the
first live frame to match the loaded Recipe, preventing an identity warm-up
generation from charging a complete rebuild to the first slider intent.
Library-resume contracts cover primary identity after insertion changes its row
beyond the first page, restored query/sort/loupe before startup handoff,
independent per-library grid/selection, explicit-path precedence, deleted
bookmarks and empty libraries. Corrupt bookmarks fail without replacing their
bytes or opening a default library. Last Import scope restores its selector and
count, and All Photographs clears its saved time window. Paging contracts locate every anchor under
tested sort/filter combinations with bounded materialization and reject mixed
cursor/anchor requests. A real `catalog locate` subprocess verifies the versioned
row/page result, missing identity, and unchanged catalog revision. Existing startup
error and owner-destruction contracts still apply. Tests isolate QSettings.
Catalog-open setup failures terminate the resume case before later commands.
The 400-file resume fixture uses a bounded 30-second open/startup deadline on
shared runners; its identity, ordering and sparse-page assertions are unchanged.
Desktop tests use the shared Qt bootstrap to set an organization and isolate
both user and system settings scopes before constructing settings owners. Display
tests use that bootstrap as well; an unset organization is a real QSettings error
on Linux/Windows. Preflight cancellation/source-loss tests gate the import worker
which owns preflight, keeping source mutation/cancellation ahead of its execution;
gating the foreground catalog executor does not establish that ordering.
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
from the repository root. Ordinary GitHub Actions branch/PR runs use
`mac_clang_debug`, `linux_clang_debug`, and `win_msvc_release`, after static
fixture-manifest and dependency-boundary checks. Tags and package rehearsals run
the full Release suite inside each of the five native package jobs, before
packaging the same binaries; no separate Debug or Release smoke matrix runs.
Both flows use `.github/actions/ci-test`, reject empty test discovery and retain
Windows runtime-path setup/failure diagnostics. Windows development CI uses Release because
MSVC Debug `/Zi` shared PDBs are uncacheable; Release objects are cacheable and
share the tag-package ccache key. Debug CRT/assert coverage remains on macOS
and Linux. CI runs `--init`, updates Qt/PATH in the
active lock, then runs `--update`. Builds use `cmake --build build/<preset>` so
they do not depend on the Linux template's `ClangDebug` build-preset name and
Windows gtest discovery can see Qt on runner `Path`.
Compiler cache setup/restore belongs to `ci-bootstrap`; its native absolute
directory is also used by `actions/cache/save` to preserve cache-version matching
on Windows as well as Unix. Every successful build saves before CTest, so later
test failure or cancellation does not discard an already uploaded cache.
Build failure skips saving. Keys retain the preset/OS/architecture restore prefix
and add job/run/attempt identity so reruns can publish updated immutable entries.
The bootstrap action's job-end save is disabled; its statistics remain available.

Unit/contract coverage includes foundation/recipe/executor, CLI JSON/exit
codes, bounded strict XMP mappings, and real `mire1.cr2` inspect/render. Catalog tests
cover schema create/reopen/newer-version reject, idempotent PNG/JPEG/RAW/DNG
import, same-stem RAW+JPEG pairing, directory sidecar skipping, unchanged source hashes, preview cache
including corrupt-PNG miss, missing/unsupported/cancelled input, and
recipe/review independence. They do not replace
local manual Studio Fit/100%/Develop acceptance. Each successful link of
`ravo_studio` runs `--smoke` on the `offscreen` software backend to instantiate
the root QML and exit before `QGuiApplication::exec()` so Windows CI cannot
block in the Qt Quick scene-graph loop; a QML or command-registry failure
fails the target build. Smoke mode installs a fatal-message handler that writes
the Qt diagnostic and exits immediately, including on Windows where an
interactive crash dialog would otherwise look like a hang. The full Windows
Debug root-QML instantiation has a 300-second subprocess bound and a 330-second
CTest bound; macOS and Linux retain 90 seconds. Windows offscreen smoke binds
`QT_QPA_FONTDIR` to an existing explicit or `%WINDIR%/Fonts` directory and
fails before launch when neither is available; it does not use Qt's absent
`lib/fonts` default. The same check is
`ravo_studio_qml_smoke` in CTest so GitHub Actions exercises it after
configure/build. Manual check:
`$<TARGET_FILE:ravo_studio> --smoke`.
The same smoke checks a compact independent startup splash with a loaded,
nonzero-size logo and verifies that showing it leaves the main window hidden.
`StudioStartupTest` covers first-run create intent, one-shot handoff after the
complete initial catalog state, explicit corrupt-catalog failure without default
retry, and destruction during an in-flight open. Startup never attaches the
splash to the geometry settings owner; `WindowGeometryTest` retains the stored
windowed/maximized restoration contract.
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

## Direct-main development loop

Ravo is a single-developer repository. The normal loop is:

```text
targeted local tests
→ push main
→ remote matrix
→ stop-and-fix on red
```

Rules:

- no force push to `main`
- no amend of published `main`
- no claiming green until the exact pushed SHA passes
- a red or incomplete ordinary `main` verification blocks further planned
  product work; repair forward with a new commit. Tag release qualification
  requires the same-SHA Release/package gates described in `Packaging.md`

`main` history safety is ruleset `22562825` (`Ravo main history safety`):
deletion and non-fast-forward protection only. CI contexts
(`Static checks`, `mac_clang_debug`, `linux_clang_debug`, `win_msvc_release`)
run on ordinary pushes as post-push verification. An atomic tagged `main` push
keeps static checks but skips duplicate development builds; the tag workflow
performs all five full Release suites and package checks. PRs retain development
coverage even if their source SHA has a tag. Same-SHA release qualification remains tag-gated; see
`DevDocs/Packaging.md` for the ruleset payload and read-back command.

Local Static parity before push:

```text
python3 Ravo/tools/freeze_legacy_manifest.py --check
python3 Ravo/tools/check_ravo_dependency_boundary.py
python3 Ravo/tools/check_vertical_slice_plan.py
python3 Ravo/tools/check_fixture_classification_ledger.py
```

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
variables for the first slider frame; ADR-0087/0089 remain the settled 1600
pixel identity, interactive 960-from-settled derivation, and 30 ms Release P90
authority. Queue depth is structurally one
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

`RAVO_PHOTO_CORPUS` is the only private photo corpus entry point. An availability
probe (directory exists / enumeration can begin) is not REL-01 evidence. Do not
treat a non-empty directory check as mixed-corpus recovery qualification or C3.

`MixedCorpusReopenRecoveryTest.PhotoCorpusOfflineRestoreReopenVertical` is a
bounded opt-in vertical (max 8 selected assets, staged copies only). When
`RAVO_PHOTO_CORPUS` is unset it skips as UNTESTED. Passing it contracts offline
restore/reopen for the selected subset; it is not REL-01 C3 complete and must
not invent missing PNG/TIFF/X-Trans coverage.



## Test framework and target boundaries

### Source-size and split integrity

Service capability refactors retain the existing integration/CLI test identities
and target labels. `CreateReopenAndRejectNewerSchema` also retains capability
references across close and verifies structured failures from Library, Metadata,
Develop, Import, Preview and Recovery before destroying the composition owner.
The ordinary recipe/history, source-hash, stale-guard, publication, cancellation,
backup/restore and bounded preview-cache tests remain the business-level oracle.

`CancellationGenerationTest` verifies borrowed-token cancellation, late-result
identity rejection, explicit token renewal and first-cancellation reason retention.
Preview/Import controller tests separately verify their scheduling and shutdown
use of that mechanism. The Library facet command test reads the child object's
scoped counts; the QML contract checks its direct bindings and canonical commands.

Develop/Inspect/Export migrations retain original test identities, failure windows
and numeric/image assertions. QML and tests address their specific child owner;
changing a property path does not remove its assertion. Production QML smoke
checks composed objects and signal receivers. Frame hash/scopes retain the
existing cancellation/identity contracts; recipe/history, source hashes and frozen
render fixtures remain the behavior oracle.

`StudioInspectFrame` exercises failed image preparation, corrupt monitor-profile
conversion, rejected ROI dispatch and publication after shutdown. Prior pixels,
viewport, URL, native resource identity, frame revision, hash and displayed Recipe
must survive failed publication;
restoring valid presentation permits a later frame. The monitor fault uses the
existing display owner's publication boundary, not an alternate converter. Shared
frame-test helpers construct only matching owner slots and immutable input data.
Live-control and progressive-preview tests retain independent current/saved/displayed
Recipe guards and the original rapid-intent publication timing.

Comparison coverage checks the accepted source/profile pair across display changes
with an independent channel-swap pixel oracle. The composed command test sends
Before then Comparison without an event-loop turn between them, and requires
unedited Before pixels alongside the edited After frame. These tests retain the
original preview queues and deadlines; closed-owner coverage also rejects adoption.
Base-restoration checks retain monitor-presented pixels and verify that a failed
monitor conversion does not replace the accepted frame with output-space pixels.

The burst performance probe starts only after loading finishes, pixels exist and
live control reports that the displayed Recipe matches current edits. Cached
settled pixels may use the live image provider, so a file URI is not a readiness
contract. The probe retains its original 1 ms intent cadence, 5 s deadline,
minimum published-frame count and exact latest-Recipe publication guard.

First-party Ravo `.cpp` and production `.qml` files have a 2,000-line limit.
`configs/translation_unit_size_budget.jsonc` and
`configs/qml_file_size_budget.jsonc` have empty debt lists. The checks reject
any oversized first-party source; adding a debt entry is not an alternative to
decomposition. New split files target at most 1,500 lines; QML section
components target at most 1,000 lines.

`configs/test_split_inventory.jsonc` freezes the ordered GoogleTest case
inventory and target membership of test files being decomposed. Splitting may
move a case between listed source files, but cannot rename, disable, reorder, or
move it to another target. Reusable fixtures and independent numeric oracles
belong in target-owned `*_test_support.cpp` units; split test-case files do not
carry private copies of the complete helper prelude. Run all three repository
checks and their self-tests through:

```text
cmake --build --preset mac_clang_debug --target RavoCodeQuality
```

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

Mask-scoped Develop (ADR-0158) is covered by `LocalAdjustmentScopeTest`,
`LocalAdjustmentEngineTest`, and `LocalAdjustmentWorkspaceTest`: independent
global/local parameters, recipe v4 round trips, whole-group alpha, original
mask pixel parity, transformed coordinates, graph ownership/copying,
incomplete gesture cancellation, Done during an in-flight preview, persistence,
and explicit CLI revision conflicts. `StudioQmlContract` requires exactly one
mask editor in the unified workspace and no per-control mask editors.
Canonical brushes accept up to 1,024 points; paths remain bounded to 32.
Use the full Ravo suite for these public recipe/Engine changes, with
`RavoCodeQuality`, Studio QML smoke and localization smoke. User-operated visual
UI acceptance is distinct from these machine contracts; rendered evidence
comes from CLI PNG artifacts, never application screenshots.

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

Mutation regression coverage preserves monotonic operation-instance IDs,
revision-bound recipe saves, atomic mask-graph clones and exclusive-node
cleanup. Review changes and the catalog revision publish in one transaction;
pre-publication cancellation changes nothing, while recovery-mirror failure
reports committed catalog state and retryability. Offline-proxy tests bound
manifest parsing, reject path escapes and source identity changes, and inject
publication failure without losing the prior verified proxy. Near-duplicate
tests enforce bounded, non-authoritative pairwise suggestions. These contracts
remain separate from mixed-corpus reopen/backup/restore qualification.

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
`Ravo/tests/fixtures/frozen/images/mire1.cr2`. JPEG/PNG/TIFF catalog import now fully decodes
before publication: truncated JPEG/PNG/TIFF publish no asset or preview;
recognized but unimplemented TIFF layouts stay `unsupported` and do not become
RAW; a camera TIFF without a RAW suffix may still import through
`unsupported_tiff_raw_container`. First-frame RAW/DNG coverage includes
structured LibRaw reasons, `.dng` suffix import of the Bayer fixture, X-Trans
6×6 decode and Engine-rendered preview publication, unpack-before-publish when no embedded JPEG exists,
RAW import cancellation, corrupt PNG cache miss, and close/reopen preview.
`DngOpcodeTest` freezes the big-endian OpcodeList2/3 envelope, checked bounds,
four-parity and partial GainMaps, declared operation order, repeated List3
operations, parsed WarpRectilinear that default decode skips, radial vignette correction, logical-range
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
independently. RAW highlight coverage additionally pins a synthetic neutral
highlight whose green photosites reach a declared linear-response limit before
red/blue: the default opposed baseline evaluates in as-shot white-balance scale,
restores neutral channel agreement, accounts for its RAW/float/mask peak, leaves
the source immutable, and rejects an out-of-range nonzero limit. Real X-Trans
import proves that the shared opposed baseline renders without selecting a
Bayer-only reconstruction fallback. A stored pre-baseline RAW recipe is
augmented only for effective load/preview/export and retains explicit highlight state.
Leftover
flip v2 orientation bits 1–7 map to canonical rotate-then-flip; `NULL`/`NONE`
stay identity because camera EXIF is applied at decode. Leftover crop v1–v3
left/top/right/bottom maps to canonical x/y/width/height; full-frame 0,0,1,1
is identity. Leftover ashift v4/v5 generic rotation, vertical/horizontal lens
shift and shear map to canonical Perspective. Specific-lens mode, unsupported
crop modes, manual crop boxes, masks, custom blends, and ambiguous UI state
fail structurally. Export
`max_edge` is the G7 output-size contract. Studio long-edge is the same field
(ADR-0113). Leftover rgblevels v1 maps to
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
covered. These contracts do not claim I14 batch/storage policy; ADR-0068 tests that higher owner separately.

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

`PerspectiveTest.CropWorkspacePreservesSourceScaleAndCanonicalCropCoordinates`
checks the preview-only recipe and canonical geometry mapping without altering
the saved recipe. `StudioPipelinePriority.CropFrameChangesKeepCompletePhotoAndViewport`
checks crop edits, rotation scale, full source extent and exit/reentry through
the presenter. QML contracts bind the overlay to the published crop region.
`PerspectiveTest.CropSurroundIsGrayWithoutReplacingBlackPhotoPixels` checks
geometry-derived gray fill, preservation of black image content, cancellation
and mismatched pixel-buffer rejection.

Foreground/import scheduling contracts are covered by `StudioPipelinePriority`
in `ravo_desktop_command_tests`: a deterministic import gate must not prevent
an uncached selected-image preview or successive live rotation renders. Live
gestures must leave the stored recipe unchanged until commit. Catalog tests
cover unrelated imports, stale same-photo edits, preview generation rejection,
and derived dimension updates that preserve concurrent review changes. Run the
full Ravo suite for changes to these worker or repository boundaries
([ADR-0160](adr/0160-foreground-preview-and-import-isolation.md)).

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
- RAW preview contract v12 validates complete decode, explicit input/output
  profiles, default opposed highlight reconstruction, and default Sigmoid tone; the raster baseline must not receive a second display transform.
  Synthetic DNG black-level tests cover uniform and unequal 2×2 pedestals,
  exact zero-signal and illuminated CFA values, and source-byte preservation.
  RapidRAW tone requires strict schema validation, fixed normalization and
  directional samples for every global control, CPU/GPU comparison, bounded
  blur memory, all-control Debug/Release latency, and catalog reset/reopen/export.
  Stored Sigmoid compatibility
  retains its synthetic colour patches, `mire1.cr2` channel-sum reference, and
  catalog reopen coverage.
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
  live memory preview before its persisted settled preview. An exact settled-cache
  hit may instead be republished as the initial owned live frame, without RAW
  unpack. Embedded JPEG must not become editable data. CLI/Studio share the
  `request_preview` contract; late results are dropped by request revision.
  Entering Develop while the selected Recipe is still loading defers that live
  request; the first frame
  must use the loaded parameters and its display-RGB snapshot must remain
  available to identity and scope analysis.
- Scopes collect from the current declared display-referred RGB preview: RGB
  histogram skips bin 0 for its peak, and parade uses 8/9 mapping with 160 tone
  bins. The Gallery grid computes scopes from browse thumbnails to avoid full
  RAW decode on selection; loupe/develop still use processed preview and never
  treat embedded JPEG as editable data.
- Quickly switching assets drops late results from old request revisions.
- Warm RAW settled previews and processed browse thumbnails must hit their
  exact disk key after catalog reopen without populating a decoded RAW slot.
  Initial Develop may opt into the matching settled frame; changed live
  parameters must miss, cancelled selection must fail, and corrupt PNG cache
  must rebuild. Source hashes remain unchanged. Import candidate thumbnails
  extract camera JPEGs without full RAW crop/opcode inspection; decoded JPEG
  pixels, orientation and declared profile are verified independently.
- Rapid cached Develop revisits must publish the selected asset's owned pixels,
  including an A/B/A burst with distinguishable source colours. Recent decoded
  PNG caching must retain ICC presentation and the existing stale-result and
  presenter-destruction contracts.
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
- The production QML smoke imports a profiled raster and delivers two real
  mouse clicks to the photo surface: Actual must be immediate and the second
  click must restore Fit while staying in Loupe. Dynamic QML tests separately
  cover native-GPU readiness with the CPU Image empty, comparison, grid and crop
  exclusions; presenter tests retain 30% and custom-factor restoration.
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
  `HarmonyGeometryTest.FullTablesMatchIndependentOracleAndReferenceInvariants`
  also checks that extended negative swatch channels and full table construction
  raise no `FE_INVALID`. The production inverse dt-UCS transforms and independent
  oracle preserve scalar exception semantics under optimized Clang, including
  unused SIMD lanes on macOS x86_64 Release.
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
  unsupported-operation failure, Color Balance RGB masked mix (ADR-0108),
  Exposure masked mix (ADR-0109), RGB Curve masked mix (ADR-0110), Tone Curve
  masked mix (ADR-0111), Highlights/Shadows/Whites/Blacks masked mix and
  fusion-break (ADR-0112), and RAW
  saturating masked memory estimates. Studio click placement maps preview
  coordinates through crop into attached-frame circle/ellipse/gradient fields
  and rejects Canvas/Perspective/straighten/rotate/flip (ADR-0114). CLI direct render parity and Catalog
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
- Studio's startup QML smoke invokes the production export form for JPEG, PNG
  and TIFF in original-size, long-edge and width/height modes. It parses each
  emitted options map with the C++ converter, proving that prefilled inactive
  dimensions remain zero in requests. Presenter and QML contracts pin suggested
  sizes, canonical codec defaults, advanced controls and accessible mode labels.
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
  prove Make/Model/numeric/date/GPS re-read. Missing GPSAltitudeRef is tested
  against Exif's default 0, including import/reopen and source-byte preservation;
  a reference without an altitude and malformed explicit references still fail.
  Refresh tests also prove identity refresh, close/reopen, and
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
  and Recipe merge owner cover session Copy/Paste Parameters. CatalogService
  `apply_develop_selection` tests cover multi-asset overlay, fail-closed
  preflight without writes, stale catalog revision, cancellation that keeps
  completed photos, and partial item failure. CLI `catalog develop-apply` and
  Studio **Paste Parameters to Selection** share that contract (ADR-0107).
  Translation catalogs and offscreen smoke cover the dialog. ADR-0072 removes all bundled
  `.dtstyle` examples and their exclusive translation generator; the parser's
  whole-format rejection remains the test truth rather than per-example
  conversion tests (ADR-0065/0098).
- Studio settings tests isolate QSettings storage and prove corrupt persisted
  language removal, English fallback, alias normalization, synchronous
  persistence, unsupported-language rejection, and preservation of the prior
  durable value. Window-geometry tests prove default 1440×900, windowed
  persist/reload, maximized keeping the last windowed rectangle, invalid-size
  rejection without writes, and incomplete/malformed key repair (ADR-0066/0115).
  Panel-layout contracts cover default sizes, destruction/reload, bounded
  input rejection, malformed settings and failed-write retry. Studio's isolated
  offscreen smoke invokes the production filmstrip resize intent,
  checks side-width bindings and preference reload, and verifies that temporary
  height constraints retain the preferred size.
  Translation package smoke separately proves every manifest
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
`StudioGalleryViewerDevelopPerformanceProbe` extends PERF-01 coverage with a
shared `ravo.perf01.report/v1` JSONL schema (`interactive_perf_report.h`) for
Gallery grid→Loupe select, adjacent-photo revisit, and Loupe→Develop first-frame
latency. Each case records warmups, P50/P90/max, cache state, source kind, and
optional host/storage/workers/peak-owned-bytes/display-refresh fields from env.
Import keyboard focus observations (`StudioImportKeyboardPerf.*`) record
per-key `focus_down_<N>` / `focus_right_<N>` samples at 1k/10k/100k synthetic
candidates plus a legacy combined `import_candidate_keyboard_focus_move` series
that sums Down+Right for continuity. These measure model+window input handling
after `processEvents`, not frame presentation; they are not PERF-02/C3 admits.
CTest discovers these two keyboard timing cases separately with `RUN_SERIAL`:
their 50 ms focus / 100 ms page-scroll ceilings and samples stay unchanged,
while other render/import tests cannot run concurrently on the same worker.

Set `RAVO_INTERACTIVE_PERF_REPORT_PATH` to append JSONL rows; optional
`RAVO_INTERACTIVE_PERF_WARMUPS` (default 2) and
`RAVO_INTERACTIVE_PERF_RECORDED_SAMPLES` (default 8) select the protocol.
`RAVO_GALLERY_VIEWER_DEVELOP_P90_BUDGET_MS` is an optional local gate. This is
measurement-only and does not admit browse optimizations.

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

`MeasuresExposureIntentThroughImagePublication` binds the system monitor
presentation owner and reports `develop_intent_to_publish_with_display`, with
the selected asset's actual media type. It includes ICC conversion and owned
surface publication, but not QML binding cost or native frame swap. It must not
be compared as equivalent to older unbound `develop_intent_to_publish` reports.
`DisplayPresentationColorTest` compares parallel ICC output byte-for-byte with
the original serial, unoptimized LittleCMS evaluator over sRGB, Display P3,
Adobe RGB and ProPhoto RGB, plus a real BToA LUT fixture, odd rows, cancellation
and repeated calls after cancellation. Small images exercise the serial branch.
Run these through `ravo_contract_tests`; display/history/export preservation
remains covered by `DisplayPresentationTest` in `ravo_catalog_tests`.

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

## Import workspace ownership contracts

Desktop Import configuration lives in `ImportDraft` inside `StudioImportWorkspace`
(page/batch/progress owner with scan / thumbnail / destination-preview controllers).
QML, command and live-control tests read its `imports` child directly. Candidate
selection remains on its `ImportCandidateListModel`; Gallery selection and sparse
listing publication retain one root owner. Functional coverage: `StudioImportWorkspace.*`,
`StudioImportKeyboard.*`, `StudioImportRoundtrip.*`, destination-preview and
thumbnail scheduler tests. Keyboard focus observations
(`StudioImportKeyboardPerf.*`) measure `processEvents` input handling only — not
frame presentation / PERF-02 / C3.

Import ownership checks retain blocked-worker preflight cancellation, source loss,
destination conflict, catalog replacement, committed-photo preference failures,
one-item dispatch, foreground editing, destroy/reopen, text-focus isolation and
bounded thumbnail shutdown. The Gallery preflight regression also blocks final
listing publication after execution failure, using an import-worker-to-GUI
fence. Work must remain active until placeholders are replaced, and terminal
notifications must expose the reconciled catalog total. Original cancellation/
source-loss assertions and timeouts remain unchanged.
Source-structure checks follow the owning controller
wiring and keep revision-based destination keys separate from debounced path
snapshots. Test identities, pixel assertions and failure-injection windows remain
unchanged. Library paging contracts still validate the shared listing/selection
generation across unloaded rows and query replacement.
Closed-filesystem tests exercise rejected destination and folder dispatch through
the real workspace/model boundary, retaining an explicit error and clearing
pending folder state.

Focused validation uses:

```text
cmake --preset mac_clang_debug -DBUILD_TESTING=ON
cmake --build build/mac_clang_debug --target ravo_desktop_command_tests
ctest --test-dir build/mac_clang_debug --output-on-failure -R 'StudioImport|ImportDraft|DestinationPreview|ImportCandidate'
```

Run discovery and execution serially. Import destroy/reopen membership is covered by
`StudioImportRoundtrip.DestroyAndReopenSameCatalogPreservesMembership` with
synthetic temporary media; it does not qualify private corpus or C3 behavior.
`StudioImportWorkspace.RealSourceProgressProbe` accepts an explicit read-only
directory in `RAVO_IMPORT_SCAN_SOURCE` and checks placeholder/thumbnail progress
without importing. The committed `Ravo/tests/fixtures/frozen/images` directory
can exercise that path locally; representative corpus performance still needs
its own workload and evidence.
`check_packaged_runtime.py` owns packaged catalog create/import/probe/reopen
checks. In-tree CLI and script unit results do not qualify deployed package
runtimes, artifact digests or release acceptance; those gates remain in
[Packaging.md](Packaging.md).

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

The ROI-versus-export contract runs its CPU pixel and persistence assertions on
all hosts. Only macOS requires Metal/IOSurface publication; Windows and Linux
must return owned CPU RGB and no native surface when that transport is absent.
Do not skip the complete test or accept arbitrary backends to satisfy CI.
QML size expressions must use members of the pinned GeoControls API. In
particular, 48-unit assistant/proposal geometry uses `Fonts.scaledUiSize(48)`;
there is no `Fonts.size48` token. Undefined arithmetic can become NaN and abort
Windows Debug Qt during layout even if a release Qt build tolerates it.
Filesystem browser fixtures explicitly distinguish in-root and out-of-root
paths: Windows temporary directories may be inside Home. Tests wait for
asynchronous ancestor discovery before activating the destination row.


## Import lifecycle and package contract gates

Thumbnail checks distinguish session reset, residency and demand generation,
including CopyDefault / ThumbnailCache restoration and Select All routing when
no item is highlighted. Packaged-runtime script tests validate CLI envelopes,
catalog membership, probe IHDR integrity and DMG top-level symlink handling.
CI publishes `PACKAGED_EVIDENCE_DIR` before validation and retains fixed failure
evidence paths. Real DMG/AppImage/DEB host unpack and package-rehearsal digest
evidence require their deployed environments; unit checks do not qualify REL-02
or C3. Desktop TSan has the separate instrumented-Qt admission gate below.

Relevant contracts include:

- `StudioImportThumbnailScheduler.OverBudgetDemandReachesFiniteTerminalWithoutThrash`
- `StudioImportThumbnailScheduler.OverBudgetComparesDispatchCompleteAndDeferredSets`
- `ImportCandidateListModel.SelectionRevisionUsesExactDeltaWithoutFullScan`
- `StudioImportKeyboard.ProductionWindowKeysUseQTestWindowEntry`
- `StudioImportRoundtrip.RepeatedIntentsRemainQuiescentWithBoundedDiagnostics`
- `Ravo/tools/test_check_packaged_runtime.py` identity + catalog + isolation suites

Sanitizer qualification runs the existing Import gate, cancellation, destruction
and post-rejection cases without suppressing reports or changing their deadlines.
Instrument both C and C++ compilation and executable/shared-library links in a
separate build directory; record which Qt/vendor/system libraries remain
uninstrumented. ASan lifetime evidence does not qualify thread synchronization.
For TSan, qualify the joined executor/service cases separately from desktop Qt
handoffs. Uninstrumented synchronization can produce false reports and hide real
races ([sanitizer manual](https://github.com/google/sanitizers/wiki/ThreadSanitizerCppManual));
Qt queued-callback reports require a matching instrumented Qt SDK before desktop
thread-safety admission. A Qt-only reproducer can classify a report mechanism,
but cannot dismiss every application report or replace the ownership contracts.

Gallery folder-presentation tests populate 200 full-size cached previews and
require list publication while thumbnail presentation is pending, 320-pixel
display output, cache URL/mtime reuse across owner destruction/reopen, monitor
and source-preview invalidation, corrupt-header rebuilding, bounded publication
lock conflicts, and rejection of rapid obsolete folder results. They record cold
all-thumbnail and warm first/all-thumbnail latencies for synthetic fixtures;
polling-harness timings are not native UI or real-RAW performance claims.
`StudioDisplayPresentationPerformanceProbe.MeasuresPrivateCatalogFolderSwitch`
accepts `RAVO_FOLDER_PERF_CATALOG` (a private catalog backup/restore with a copied
preview cache) and `RAVO_FOLDER_PERF_URI`; it records three listing latencies
separately from thumbnail completion. It skips without explicit inputs and
must never be pointed at the user's live catalog.

## Studio command workspace (Import)

Import readiness is tested with duplicate classification active, all thumbnail
work blocked and unknown candidate hashes. Copy destination validation must
complete while the catalog executor is blocked; final import still validates the
source. With the catalog executor blocked, tests require Gallery visibility and cancellable import state
immediately after the click. Cancellation, source disappearance and destination
conflict must release busy state without importing or remembering a failed draft.
The 300-candidate thumbnail scheduler regression asserts viewport-first dispatch,
exactly-once completion of every row, finite one-pass offscreen work, cache limits
and visible-pixel residency. It waits in a real Qt event loop with a 30-second
watchdog and reports completion/pending/in-flight counts on failure. The test
must not add a polling sleep to each worker/UI handoff; this is a scheduling
contract, not a host-dependent decode latency benchmark.
Tests requiring final duplicate classification explicitly
wait for scan completion rather than using button readiness as a scan oracle.

Live Studio interactive commands share one C++ workspace policy
(`active_command_workspace` / `command_workspace_support`). While Import is
open, Gallery selection mutations (rating/flag/nav/remove/recipe edit) stay
blocked; Select All, cancel, and listed window globals remain available.
Copy Info and Reveal in File Manager accept a generation-bound Import context
candidate, never the Gallery selection. The context-command regression covers
duplicates, missing files, candidate replacement, invalid rows and page close;
the production smoke verifies Import menu bindings and sends a real right-click
event to a duplicate candidate without enabling its import checkbox.
Gallery selection is preserved for restore after Import closes. Validation:
`StudioImportKeyboard.ImportWorkspaceBlocksGallerySelectionCommands` and the
production Import layout smoke path inside `ravo_studio --smoke`.

## Probe image artifact contract

`catalog probe --output` emits a versioned nested `artifact` object
(`ravo.image_artifact` v1) after verifying immutable encoded PNG bytes through
the shared RasterDecoder owner. Packaged runtime checks consume that contract
and stream the file under a hard byte cap; they do not own a second PNG/ICC
decoder. Build-tree create/import/probe/reopen evidence is not packaged PASS.
