# Ravo product execution TODO

> **Status:** product execution queue
>
> **Updated:** 2026-09-03

This file contains unfinished product work, dependencies, risks, validation,
and acceptance gates. Three-platform Release package evidence lives in
[Packaging.md](Packaging.md) and does not block items here.
Current behavior belongs in `Ravo/README.md`,
[ARCHITECTURE.md](ARCHITECTURE.md), [TESTING.md](TESTING.md), code, tests, and
accepted ADRs. Cross-layer ideas without an accepted product contract remain in
[ProductRoadmap.md](ProductRoadmap.md).

Leftover-faithful algorithm ports are closed by ADR-0106. No item below
reopens the old GTK UI, dynamic IOP ABI, OpenCL path, or deleted application.

## Queue rules

Priority is dependency-based:

- **P0 — Active release gate:** evidence needed to claim the current product is
  release-ready.
- **P1 — Ready after an accepted ADR:** professional workflow slices whose owner
  and contract are decided.
- **P2 — Decision required:** valuable outcomes that remain blocked in the
  roadmap until a dated ADR exists.

Every implementation item must name its owner, lifecycle, persisted or machine
contract, error/cancellation behavior, smallest validation set, and acceptance
gate. Delete a completed item after moving durable facts to their authority.

# P0 — Release evidence

## REL-01 — Private mixed-photo corpus and interactive latency

**Dependency:** an explicit read-only mixed RAW/raster tree in
`RAVO_PHOTO_CORPUS`; generated catalogs, previews, recovery files, backups, and
reports remain under a unique temporary root. Use a Release build on the release
candidate host.

```text
RAVO_PHOTO_CORPUS=/absolute/private/photos \
  build/mac_clang_release/Ravo/tests/ravo_catalog_tests \
  --gtest_filter=CatalogServiceTest.PrivatePhotoManagementReleaseProbePreservesCorpus
RAVO_INTERACTIVE_PERF_CATALOG=/temporary/private-corpus/library.sqlite \
RAVO_INTERACTIVE_PERF_ASSET_ID=<imported-raw-asset-id> \
RAVO_INTERACTIVE_PERF_P90_BUDGET_MS=10 \
RAVO_INTERACTIVE_BURST_BUDGET_MS=10 \
  build/mac_clang_release/Ravo/tests/ravo_desktop_command_tests \
  --gtest_filter='StudioInteractivePreviewPerformanceProbe.*'
```

**Acceptance gate:**

- record RAW/raster import, cold/warm settled preview, page, sequential
  interactive P50/P90/max, and rapid-intent first/latest/frame-gap timings using
  the definitions in `TESTING.md`;
- record the opt-in native frame-swap trace with display refresh rate and power
  state separately from owned-image latency;
- freeze host-local budgets only from a repeatable candidate run, then rerun
  with those budget variables enabled;
- prove every corpus file retains exact SHA-256, size, and modification time;
- retain private reports outside the repository and do not generalize one host
  result to another OS, storage device, GPU, or toolchain.

**Risk:** an unset `RAVO_PHOTO_CORPUS` skips the probe and is not evidence.

A macOS corpus result is not evidence for Windows or Linux hosts.

# P0 — Gallery and Develop performance evidence

## PERF-01 — Representative Gallery-to-viewer measurement

**Dependency:** the same explicit read-only mixed corpus used by `REL-01`, a
Release build, declared storage type, and a stable power/performance state.

Measure cold and warm folder enumeration/import, first thumbnail, viewport
completion, placeholder publication, exact 1600-edge publication, adjacent-photo
revisit, page query, and interactive Develop publication over two warmups plus
at least eight recorded samples.

Record P50/P90/max, cache state, source kind, file count, host, storage kind,
worker count, peak owned bytes, display refresh rate where applicable, and source
SHA-256/size/mtime preservation.

Suggested focused validation:

```text
cmake --preset mac_clang_debug -DBUILD_TESTING=ON
cmake --build --preset mac_clang_debug --target ravo_desktop_command_tests ravo_studio
ctest --test-dir build/mac_clang_debug --output-on-failure -R 'StudioPresenterTest|StudioQmlContract|ravo_studio_qml_smoke'
cmake --preset mac_clang_release -DBUILD_TESTING=ON
cmake --build --preset mac_clang_release --target ravo_desktop_command_tests ravo_studio
```

**Acceptance gate:** the report distinguishes enumeration, catalog publication,
thumbnail demand, exact preview, image ownership, native frame swap, and cache
state; source preservation passes; observed stalls have an owner and trace
rather than a guessed optimization.

## PERF-02 — Admit only measured browse optimizations

**Dependency:** `PERF-01` identifies a dominant bottleneck on the same corpus.
Evaluate each candidate independently:

- a byte-bounded browse worker pool;
- a profiled medium browse resource;
- a byte-bounded adjacent-preview LRU;
- deferred optional capture metadata;
- a non-PNG browse encoding.

**Acceptance gate:** adopt a candidate only when same-corpus end-to-end P90
improves without foreground-Develop latency, deterministic publication,
cancellation, memory, source-safety, colour/profile, HDD-seek, or package
regression. Item-count-only memory limits are insufficient. Browse placeholders
and embedded JPEGs remain presentation resources, never RAW correctness
references or silent Develop fallbacks. GPU work remains under
[GPU_Baseline.md](GPU_Baseline.md).

# P1/P2 — Professional photographer workflows

Each section below is **P2 / Decision required** until the linked theme in
`ProductRoadmap.md` becomes a dated ADR. After acceptance, move only the bounded
implementation tranche to **P1 / Ready** and add its exact tests.

## PRO-INGEST — Camera/card and modern-format ingest

**Status:** P1 / Partial — ADR-0118 accepts HEIC/HEIF fail-closed recognition
(ftyp brands, structured `unsupported_heic_input`, no pretend-JPEG / incidental
ImageIO decode, enumeration of `.heic`/`.heif`/`.heics`/`.heifs`/`.hif`). Owned
HEIC decode, PTP/MTP, DNG conversion, and Smart Previews remain unfinished.

**Outcome:** ingest from supported card/camera sources with explicit destination,
rename, verified second copy, and resumable failure handling; define HEIC/HEIF,
DNG conversion, and Smart Preview scope.

**Dependencies:** existing Add/Copy/Move planner and ADR-0104 publication rules;
ADR-0118 for HEIC/HEIF recognition; a later PTP/MTP and owned-decoder /
licence/package ADR for remaining format work.

**Remaining unfinished work:**
- Owned HEIC/HEIF decode (pixels, colour, orientation, multi-image selection)
  after dependency/licence/package gates.
- PTP/MTP transport and device lifecycle.
- DNG conversion and Smart Preview policy.

**Risks:** destination collisions, device disappearance, source mutation,
pretending HEIC is JPEG, background DNG replacement of originals, and partially
cataloged batches.

**Acceptance gate:** preflight every primary/second-copy path, publish no catalog
row on unresolved conflict, preserve source bytes, report partial completion by
item, and leave the next ingest reusable after cancel, disconnect, or error.
HEIC/HEIF recognition fails closed with zero publication until an owned decoder
ships.

## PRO-METADATA — Hierarchical keywords and delivery metadata

**Status:** P1 / Partial — ADR-0119 accepted and implemented for hierarchical
keywords (schema v12 vocabulary + membership, CLI/service APIs, Studio path
entry, revision-checked multi-select). IPTC subset, location, and facets remain
undecided.

**Outcome:** hierarchical keywords, a defined IPTC subset, catalog-owned location,
and camera/lens/date facets with transactional multi-selection editing.

**Done in this tranche:**
- catalog-owned single-parent keyword tree with stable IDs and `|` display paths;
- `asset_keyword` membership plus denormalized `asset_tag` paths for LibraryQuery /
  recovery / export;
- CLI `keywords` / `keyword-create|rename|move|delete` and path-capable `tag`;
- Studio Tags field accepts hierarchical paths; multi-select uses one revision-
  checked CatalogService transaction;
- create/reopen/migrate v12 and rename/membership survive reopen/backup checks.

**Remaining unfinished work:**
- IPTC Core/Ext field subset beyond keyword paths;
- catalog-owned location tables and camera/lens/date facets;
- adjacent XMP/source keyword merge matrix (PRO-INTERCHANGE).

**Risks:** two live metadata authorities if interchange lands without a matrix,
QML-built SQL, and privacy stripping that does not match export for future IPTC /
location fields.

**Acceptance gate:** hierarchy edits survive reopen/backup/restore, source refresh
preserves catalog-only fields, bulk failure rolls back or reports exact partial
state according to the accepted contract, queries remain bounded, and export
privacy is exact.

## PRO-LOCAL — Everyday masked grading

**Status:** P2 / Partial — ADR-0116 accepted and implemented for every
authorized everyday consumer (Exposure, Color Balance RGB, RGB Curve, Tone
Curve, Highlights/Shadows/Whites/Blacks). Click placement of
circle/ellipse/gradient geometry remains accepted (ADR-0114).

**Outcome:** C++-owned histogram-assisted parametric authoring for every
accepted masked everyday consumer through the same recipe CLI/Studio path.

**Remaining unfinished work:**
- Multi-instance policy, group-child parametric assist, path/brush stroking,
  and Canvas/Perspective inverse mapping remain undecided/out of scope.
- Importer behavior for unsupported legacy blend/mask forms stays fail-closed.

**Risks:** QML-owned mask pixels, display-histogram vs linear parametric source
mismatch under heavy grading, coordinate drift after Canvas/Perspective, and
altered unmasked defaults.

**Acceptance gate:** C++-owned histogram assistance authors a parametric
canonical mask for each authorized everyday consumer through the same recipe
CLI/Studio path; preview/export/reopen equality; fail-closed unsupported
geometry; cancellation/resource bounds; and no change to unmasked
identity/default behavior.

## PRO-EXPORT — Repeatable delivery jobs

**Status:** P1 / Partial — ADR-0117 box fit, post-resize output sharpen,
reusable ExportOptions presets, and restartable batch jobs are implemented.
Long-edge remains ADR-0113. Watermark and colour-conversion frame stay
undecided.

**Outcome:** box resize, output sharpening, reusable export presets, and
restartable background batch delivery.

**Remaining unfinished work:**
- Watermark and colour-conversion/frame overlays remain undecided.

**Risks:** resizing in QML, a second encoder/job owner, overwrite/skip guessing,
recipe mutation, and ambiguous partial delivery after restart.

**Acceptance gate:** exact JPEG/PNG/TIFF dimensions and colour, explicit conflict
preflight, cancel/restart semantics, completed-file retention, no source/recipe
rewrite, and exact preset apply after reopen.

## PRO-INTERCHANGE — Explicit XMP, catalog conversion, and external editors

**Status:** P1 / Ready — ADR-0120 accepted. First Ready slice landed:
`catalog xmp-status|xmp-import|xmp-export` with CRS recipe-field conflict
preflight, catalog-owned exchange baseline, fail-closed unsupported CRS, and
original-byte preservation. Remaining work stays P2 until its own ADR.

**Done (this slice):** conflict classes
`missing|identical|catalog-newer|sidecar-newer|both-changed`; explicit
`--resolve abort|catalog|sidecar`; structured JSON errors; CLI + service tests.

**Still open (P2 / Decision required):**
- external-editor derived assets/versions (deferred — needs a later ADR;
  too large for ADR-0120)
- foreign Lightroom/Capture One catalog conversion (no in-place open)
- keyword/IPTC/location adjacent merge matrix (ADR-0119 stays catalog-owned)
- supported source-version matrix beyond CRS PV2012 recipe fields

**Outcome:** user-initiated interchange without adjacent XMP becoming a second
live authority, plus external-editor output as a new derived asset/version.

**Risks:** in-place foreign-catalog migration, hidden external renderer, original
RAW mutation, watcher races, and unsupported fields silently dropped.

**Acceptance gate (remaining):** external results enter as new assets/versions;
foreign conversion is read-only; keyword merge matrix is explicit.

## PRO-PRESENT — Tether, print, map, slideshow, and publishing

**Status:** unauthorized while `MIGRATION.md` records these as removed leftovers.

A new dated product decision must independently define each outcome, owner,
hardware/service lifecycle, offline/cancel behavior, package dependencies,
privacy, and validation. Do not port GTK modules or add empty panes to claim
coverage.

# P2 — AI-assisted photo work

ADR-0121 accepts AI architecture/privacy/provenance. AI-01 may become Ready
against the proposal contract; AI-02…AI-05 remain blocked on their stated
dependencies. The existing Studio chat assistant still does not authorize
model inference, image upload, automatic catalog mutation, or generated-pixel
publication until a provider package is recorded.

## AI-00 — AI architecture, privacy, provenance, and licence ADR

**Status:** Accepted — ADR-0121. Residual before shipping a provider: record the
concrete runtime/weight package, notices, and evaluation corpus in Packaging /
Dependency Workflow. AI-01 may proceed against the proposal contract.

**Decision work (completed in ADR-0121; residual packaging below):**

- define conversational assistant, inference provider, proposal, apply, and
  derived-asset owners without moving business rules into QML;
- define local and remote provider ports, explicit user initiation, payload
  preview/minimization, cancellation, timeout, retry, concurrency, and memory
  bounds;
- define credential storage and redaction; no key, prompt payload, original path,
  EXIF, catalog state, or provider response enters logs unintentionally;
- define a versioned `proposal` contract for normal recipe fields and canonical
  masks, including source asset/revision, provider/model/version or weight hash,
  parameters, confidence/alternatives where meaningful, and deterministic
  validation before apply;
- define a separate immutable derived-asset contract for non-replayable generated
  pixels, including source revision, model identity, settings, output hash,
  retention, backup/restore, and missing-model behavior;
- decide model-runtime and weight distribution, GPL compatibility, third-party
  notices, package size, update policy, local cache, telemetry, retention, and
  whether providers may train on user photos; the default must not be implicit;
- define a licensed evaluation corpus and human review protocol across RAW/raster,
  cameras, lighting, genres, skin tones, edge cases, and known failures.

**Acceptance gate:** one or more dated ADRs name all owners and contracts; threat
and privacy review is recorded; dependency/licence/package implications are
known; failure and no-model/offline behavior are testable; no implementation
requires originals to be writable.

## AI-01 — Reviewable global edit proposals

**Status:** P1 / Ready after ADR-0121 (proposal contract).

**Outcome:** propose white balance, exposure, tone, colour, crop/straighten, and
reference-grade adjustments as ordinary validated recipe fields. Show the exact
field diff, preview before apply, and alternatives where the model supports
them.

**Acceptance gate:** reject/cancel changes nothing; apply creates normal
history/undo and survives reopen/export; unknown fields, stale asset/revision,
invalid bounds, provider failure, or missing model fail without partial state;
quality regressions are caught by the evaluation corpus and human review.

## AI-02 — Semantic selections as canonical masks

**Status:** blocked by `AI-00` and the applicable local-adjustment contract.

**Outcome:** propose subject, sky, background, person/skin, clothing, and named
object selections as bounded canonical masks that users can inspect and edit.

**Acceptance gate:** mask coordinates remain correct through orientation,
Perspective, crop, Canvas, preview, export, and reopen; uncertain/empty masks are
visible; CPU/GPU/model absence has explicit behavior; generated mask data is
bounded, cancellable, versioned, and never silently approximated.

## AI-03 — Shoot-level consistency and batch assistance

**Status:** blocked by `AI-01` and `AI-02` as applicable.

**Outcome:** use an explicit reference image or accepted style to propose a
consistent grade across a selected shoot while preserving per-image exposure,
white balance, and exceptions.

**Acceptance gate:** the user selects every destination; preflight binds the
observed catalog and asset revisions; partial completion and cancellation follow
the existing multi-selection contract; each image records its own proposal/apply
history and can be independently reverted.

## AI-04 — Metadata, culling, and similarity suggestions

**Status:** blocked by `AI-00` and the relevant metadata/privacy ADR.

**Outcome:** optional keyword/caption suggestions, focus/exposure/duplicate cues,
and near-duplicate grouping that never auto-rejects, deletes, publishes, or
writes identity-sensitive metadata without explicit acceptance.

**Acceptance gate:** suggestions are separated from catalog facts, confidence and
model identity remain inspectable, private/people/location policy is explicit,
and accepting a batch is transactional or reports exact partial state.

## AI-05 — Retouch and generated-pixel results

**Status:** blocked by `AI-00`; schedule after proposal/mask workflows are proven.

**Outcome:** accelerate dust, blemish, distraction, and object cleanup. Use
canonical Retouch regions when the result is replayable; otherwise publish a
new immutable derived asset/version rather than hiding pixels in a recipe field.

**Acceptance gate:** originals remain byte-identical; generated output is atomic,
content-addressed, attributable, cancel-safe, backup/restore aware, and visible
as generated; failure leaves no partial asset; reopen and export do not require
the original provider to be online.

# Cross-cutting acceptance

Any newly Ready `PRO-*` or `AI-*` tranche adapts this minimum set rather than
weakening it:

```text
python3 configs/source_roots.py verify
python3 Ravo/tools/freeze_legacy_manifest.py --check
python3 Ravo/tools/check_ravo_dependency_boundary.py
cmake --preset mac_clang_debug -DBUILD_TESTING=ON
cmake --build --preset mac_clang_debug --target ravo_catalog_tests ravo_desktop_command_tests ravo_contract_tests
ctest --test-dir build/mac_clang_debug --output-on-failure -R 'CatalogServiceTest|StudioPresenterTest|StudioQmlContract'
```

Add domain/service/CLI contracts first, then desktop presentation and QML smoke.
Add schema migration/reopen/backup/restore tests for persisted state; stale
revision, conflict, cancellation, source disappearance, resource exhaustion,
and partial publication tests for asynchronous work; package and generated
third-party notice checks for new dependencies/models. Windows and Linux remain
untested until those hosts run. A skip is not a pass.
