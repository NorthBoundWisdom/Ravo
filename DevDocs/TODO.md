# Ravo product execution TODO

> **Status:** ordered residual queue
>
> **Updated:** 2026-09-04
>
> **Review basis:** `main` at
> `b9bbb766dc3e91204fe01fb2fc911b7c680f1539`; CI run `33876363596`
> was still incomplete at review. The next free ADR number is **0157**, but new
> product ADRs are frozen by the work-in-progress rule below.

This file contains only unfinished product work, dependencies, risks,
verification, and acceptance gates. Current behavior belongs in
`Ravo/README.md`, [ARCHITECTURE.md](ARCHITECTURE.md),
[TESTING.md](TESTING.md), accepted ADRs, code, and tests. Package evidence
belongs in [Packaging.md](Packaging.md). Product themes without an executable
contract belong in [ProductRoadmap.md](ProductRoadmap.md).

Ravo's near-term objective is not feature count. It is a professional photo
manager and non-destructive grading application that remains safe,
colour-correct, responsive, and recoverable with real catalogs, removable
storage, repeated delivery work, and long editing sessions.

Leftover-faithful algorithm ports remain closed by ADR-0106. Nothing below
reopens the deleted GTK application, dynamic IOP ABI, Lua surface, old catalog,
or OpenCL path. GPU work remains an Engine QRhi adapter with CPU as the
correctness reference.

## Queue discipline

### Maturity labels

Use these labels consistently in ADRs, README status, release notes, and this
queue:

- **C0 — Contract:** ownership, lifecycle, persistence, failure, cancellation,
  resource, privacy, and package behavior are decided.
- **C1 — Headless ready:** the shared C++ owner and CLI/service tests implement
  the bounded contract.
- **C2 — Studio workflow ready:** a photographer can complete the workflow in
  Studio without CLI assembly, and history/reopen/recovery behavior is covered.
- **C3 — Release proven:** real corpus or hardware evidence, end-to-end
  performance budgets, installed-package smoke, and supported-host results are
  recorded for one release candidate SHA.

A stub or fixture can establish C0/C1 only. A capability below C2 is not a
completed user workflow. A capability below C3 is not a release-ready claim.

### Work-in-progress limit

A green `main` may carry at most:

1. one active P0 evidence/correctness stream; and
2. one active P1 workflow-completion stream.

Until `COR-01` is closed and `LOCAL-01` reaches C2:

- do not add another product capability ID, model/provider stub, placeholder
  pane, unsupported-probe feature, or specialization ADR;
- do not add more AI Stub chrome; the existing proposal pane is a developer
  contract surface, not a product milestone;
- do not expand the ADR-0153 tethered-studio stub; SPECIALIZE remains deferred;
- finish correctness, Studio workflow, persistence, backup/restore,
  cross-platform, and performance residuals on accepted contracts;
- do not grow `Main.qml`; a P1 change that touches it must move orchestration
  into a dedicated component or C++ presenter and produce no net line growth.

A data-loss, security, catalog-corruption, colour-integrity, or unbounded
resource defect preempts both streams.

## Current assessment and execution order

Recent work on LOCAL-01 Studio chrome/history/copy, CULL-01 keyboard review,
EDITIN-01 Studio round-trip, PERF-01 instrumentation, and IQ-00 CPU-gold policy
moves in the intended direction. OFFLINE-01 and DISPLAY-01 remain partial
contracts rather than complete workflows. AI proposal chrome, display/native
transport stubs, and the tethered-studio probe do not count as product progress.

### Release and correctness lane

| Order | ID | Priority | Outcome |
| ---: | --- | --- | --- |
| 1 | CI-01 | P0 | Keep the latest main SHA green and make the gate enforceable |
| 2 | COR-01 | P0 | Fix reviewed mutation, rollback, and support-file safety defects |
| 3 | REL-01 | P0 | Prove source safety, catalog durability, interruption recovery, and upgrade |
| 4 | PERF-01 | P0 | Freeze Gallery, viewer, Develop, analysis, and large-library budgets |
| 5 | IQ-00 | P0 | Complete preview/reopen/export, ICC, ROI, and CPU/GPU consistency evidence |
| 6 | REL-02 | P0 | Prove installed packages and backup/restore on supported hosts |

### Product-completion lane

Do not execute these concurrently. Finish the current row to C2 before starting
the next one.

| Order | ID | Priority | Outcome |
| ---: | --- | --- | --- |
| 1 | LOCAL-01 | P1 | Finish safe multi-instance local adjustments and mask composition |
| 2 | EDITIN-01 | P1 | Close the nearly complete Studio external-editor round trip |
| 3 | CULL-01 | P1 | Finish transactional keyboard review and evaluated assistance |
| 4 | OFFLINE-01 | P1 | Make verified proxies usable for actual Loupe/Develop work |
| 5 | DISPLAY-01 | P1 | Finish per-display ICC presentation on supported hosts and views |
| 6 | INGEST-01 | P1 | Replace transport stubs with packaged native adapters and hardware evidence |
| 7 | IQ-01 | P1 | Establish camera/profile and denoise quality admission |

# P0 — Repository, correctness, and release gates

## CI-01 — Green and enforceable main

**Status:** Open. A prior same-SHA three-platform Debug run passed, but the
reviewed head's run was incomplete, and `main` remains unprotected.

**Remaining work:**

- require Static checks and macOS/Windows/Linux jobs through a repository
  ruleset or equivalent branch policy;
- do not accept another product-state commit or tag while the latest main run is
  queued, in progress, failed, or cancelled;
- add a Release or RelWithDebInfo compile plus minimal CLI/Studio smoke job;
- make tag packaging and release publication depend on the same successful
  source SHA, with no red-commit or skipped-job path;
- document required checks and a narrow, auditable emergency override policy;
- keep flaky tests visible and owned rather than converting them to permanent
  skips.

**Acceptance gate:**

- the latest main SHA has one successful Static/macOS/Windows/Linux matrix;
- ordinary changes cannot bypass required checks;
- release configuration compiles and launches outside the Debug-only path;
- tag publication cannot use a failed, cancelled, incomplete, or superseded
  gate;
- no required job is `continue-on-error`, silently narrowed, or replaced by a
  documentation-only run.

## COR-01 — Reviewed correctness defects before further feature breadth

**Status:** Active blocker. This is a correctness gate, not a new product
capability.

### LOCAL-01 instance and mask mutation safety

`Ravo/recipe/src/develop_instances.cpp` needs strong failure guarantees:

- `delete_exposure_instance` and
  `delete_color_balance_rgb_instance` must validate `instance_id` before the
  singleton-collapse path. A stale or unknown ID must not clear the only
  instance.
- Define last-instance delete semantics explicitly. Deleting a disabled or
  bypassed sole instance must not silently reactivate its parameters through
  the legacy singleton mirror; either retain one identity instance or reset the
  operation to a documented identity state.
- Instance IDs must not be silently reused after deletion when a stale
  command/history entry could address a newly created instance. Mutations must
  bind the observed recipe/catalog revision.
- `clone_mask_subgraph` must build a complete staged clone and append it
  atomically. A missing child, cycle, allocation failure, cancellation, or
  malformed group must leave `DevelopParams` byte-for-byte unchanged.
- Deleting or replacing an instance must remove only its exclusively owned mask
  subgraph and must retain shared/external masks or fail closed. No orphan mask
  accumulation.

### CULL-01 catalog mutation atomicity

`Ravo/services/src/catalog_cull_review.cpp` currently performs review update,
revision bump, recovery synchronization, and optional auto-advance discovery as
separate fallible steps.

- Move review state plus revision into one repository transaction.
- Check cancellation before publication. After publication, return an explicit
  committed result rather than a cancellation/error that implies no mutation.
- A recovery-sidecar failure must either roll back the catalog transaction or
  report exact committed state and retryable recovery state.
- Compute/validate auto-advance order before mutation, or treat navigation
  failure separately so a committed review edit is never returned as a failed
  mutation.
- Apply the same contract to `set_picked` and other direct review helpers.
- Add failure-injection tests for revision failure, recovery failure,
  cancellation before/after publication, stale revision, and reopen.

### OFFLINE-01 manifest and publication safety

`Ravo/services/src/catalog_offline_edit_proxy.cpp` needs a fail-closed retained
resource lifecycle:

- replace throwing `std::stoll`/`std::stoull` support-file parsing with bounded
  structured parsing; malformed or out-of-range manifests must never escape an
  exception;
- validate schema/version, asset identity, hash syntax, dimensions, profile,
  maximum edge, and that every manifest path is contained in the expected
  catalog support root;
- surface corrupt manifests in list/status results; do not silently omit them;
- generate a replacement proxy in a unique staging tree, verify bytes and
  manifest, then publish atomically. Failure must retain the prior valid proxy
  and leave no partial final tree;
- do not report an original as export-usable from file presence alone; verify
  the catalog/source identity or require an explicit reconnect result;
- before routing Develop to a proxy, define whether proxy pixels are baseline
  or recipe-baked. The render path must not apply the same recipe twice after a
  proxy was produced from an edited export.

**Acceptance gate:**

- every listed error path either changes nothing or returns exact committed
  state;
- wrong IDs, cycles, missing children, corrupt manifests, out-of-range values,
  path escape, disk-full, cancellation, and injected publication failures have
  regression tests;
- reopen and backup/restore preserve the same result;
- no source original is modified;
- no fallback or partial support tree is silently accepted.

## REL-01 — Real mixed-photo corpus, source safety, and recovery

**Status:** Active evidence work. A host-local path alone is not reviewable
evidence.

Use an explicit read-only mixed RAW/raster corpus in `RAVO_PHOTO_CORPUS`, a
Release build, and a unique temporary root for every generated catalog,
preview, recovery mirror, proxy, derived file, backup, and report.

The corpus must cover Bayer and X-Trans cameras, DNG, JPEG, PNG, TIFF, adjacent
XMP, same-stem RAW+JPEG, malformed/truncated/unsupported inputs, large and
Unicode paths, duplicate content, removable storage, and source disappearance.

**Required scenarios:**

- Add/Copy/Move, filesystem-card ingest, verified second copy, rename,
  disconnect, cancellation, resume, and destination collision;
- ratings, pick/reject, labels, keywords, metadata, collections, virtual copies,
  stacks, history, snapshots, multi-instance recipes, and batch edits;
- preview rebuild, folder relink, XMP conflict handling, external-editor
  working copies, offline proxies, cull reports, backup/restore, and reopen;
- disk-full, process termination at publication boundaries, corrupt support
  files, stale revisions, and upgrade from the latest published catalog.

**Evidence artifact:** keep photographs private, but publish or attach a
sanitized report for the exact candidate SHA containing:

- host, OS, toolchain, package/build identity, storage type, and power state;
- hashed corpus manifest, format/camera-family counts, and omitted scenarios;
- command/test versions, sample counts, pass/fail/skip, structured failures, and
  original before/after hashes;
- backup/restore/upgrade results and known limitations.

Do not commit absolute developer-machine paths as evidence.

**Acceptance gate:**

- source SHA-256, size, and mtime remain exact unless an explicitly successful
  destructive command owns the mutation;
- unresolved conflicts publish no row or artifact;
- interruption leaves the previous complete state or the new complete state;
- catalog, recipes, metadata, stacks, proposals, proxies, and provenance remain
  coherent through reopen/upgrade/backup/restore;
- missing dependencies and unsupported inputs fail structurally;
- an unset corpus, skipped test, or unshared local report is not evidence.

## PERF-01 — End-to-end interaction and large-library budgets

**Status:** Measurement harness exists; budgets and representative evidence are
open. Do not start PERF-02 optimization work yet.

Measure complete user-visible paths with two warmups and at least eight recorded
samples:

- import publication, first placeholder/thumbnail, viewport completion, and
  exact browse preview;
- adjacent select/revisit, Fit/Fill/Actual Size, and 1:1 ROI;
- first interactive Develop frame, rapid-burst latest frame, settled save, and
  reopen;
- presentation transform and native frame swap separately from owned-image
  publication;
- page/facet queries, exact hash, burst grouping, and heuristic similarity at
  100,000 photos;
- offline proxy creation/verification/selection/reconnect;
- external-editor preparation and returned-file registration.

Record P50/P90/max, cold/warm state, source kind, catalog size, storage, workers,
peak owned bytes, display refresh, GPU backend, and power state.

The ADR-0149 implementation currently materializes the library, decodes each
candidate, and compares fingerprints pairwise. Before C3 it needs an explicitly
bounded dataset limit or an indexed/incremental design; O(N²) work may not run
unbounded on a professional catalog.

**Acceptance gate:**

- candidate-specific budgets are frozen from reproducible reports;
- reports separate catalog, decode/demosaic, processing, encoding,
  presentation, ownership, QML publication, and frame swap;
- rapid intents never publish an older frame after a newer one;
- background analysis, ingest, proxy generation, or export cannot starve
  foreground Develop;
- memory, I/O, cancellation, and publication remain bounded;
- a same-corpus rerun with frozen budgets passes.

### PERF-02 — Admit only measured optimizations

Start only after PERF-01 identifies a dominant owner. Evaluate one change at a
time and retain it only when same-corpus end-to-end P90 improves without
foreground, determinism, memory, source-safety, colour/profile, HDD-seek,
package-size, or CPU-reference regression.

## IQ-00 — Rendering and colour consistency gate

**Status:** ADR-0151 establishes CPU-gold policy; full corpus coverage remains
open.

Complete a matrix across raster, Bayer, and X-Trans inputs; representative
global and multi-instance edits; masks and geometry; built-in/file ICC input and
output; proof/monitor transforms; 1:1 ROI; settled preview; reopen; and
JPEG/PNG/TIFF export. Exercise CPU-only, supported GPU, GPU-unavailable,
resource-exhausted, cancelled, and corrupt-profile paths.

**Acceptance gate:**

- preview, save, reopen, and export use one canonical recipe with documented
  tolerances;
- CPU/GPU and monitor comparisons use owned pixel data, never screenshots;
- unsupported GPU stages remain explicit;
- ICC identity and embedded profile bytes match declarations;
- moving between displays changes presentation only;
- overlapping 1:1 and export regions agree after the same geometry;
- regressions retain input, recipe, backend, profiles, hashes, and metrics.

## REL-02 — Installed package, upgrade, and restore evidence

**Status:** Open after CI enforcement and candidate artifacts exist.

Validate the final DMG, Windows ZIP, Linux AppImage, and Linux DEB after copying
each away from the build tree. Smoke Studio and bundled CLI through catalog
create/open, raster+RAW import, browse/review/edit/save/reopen/export,
multi-instance reopen, external editor, offline status, cull review,
preview rebuild, backup/verify/restore, upgrade, localization, ICC/GPU status,
long paths, and removal without touching user data.

**Acceptance gate:**

- every artifact launches on a clean supported host without build-tree paths or
  undeclared runtime dependencies;
- source-build and installed machine contracts match;
- failed upgrade leaves the prior catalog recoverable;
- backup/restore stages all retained support state and excludes originals and
  declared rebuildable caches;
- release notes, README, package version, supported formats, signing claims, and
  actual artifacts agree.

# P1 — Complete the professional baseline

## LOCAL-01 — Professional multi-instance local adjustments

**Maturity:** C2 candidate, blocked by COR-01 and geometry/composition evidence.

**Remaining work:**

- close all LOCAL findings in COR-01 before more chrome;
- define bounded instance counts, stable identity, last-instance behavior, and
  owned-mask garbage collection;
- finish Add/Subtract/Intersect/Invert, opacity, overlay visibility, feather,
  and accepted brush flow/density UX through one C++-owned mask DAG;
- prove one coordinate path through orientation, lens, Perspective, crop,
  Canvas, preview scaling, 1:1 ROI, and export;
- complete snapshot/reopen and failure-injection depth for instance vectors and
  masks; retain existing history/undo/style/copy/batch/virtual-copy contracts;
- add GPU evaluation only after CPU equality and PERF-01 evidence;
- extract any remaining Main.qml orchestration rather than growing it.

**Acceptance gate:**

- Studio can create, name, reorder, duplicate, mask, bypass, enable, and delete
  at least two Exposure and Color Balance RGB instances without CLI assistance;
- every failed mutation leaves exact prior state;
- preview/export/reopen/history/undo/snapshot/style/copy/batch agree;
- masks stay aligned through accepted geometry;
- QML owns no durable geometry or pixels;
- legacy singleton recipes remain bit-compatible.

## EDITIN-01 — Studio external-editor round trip

**Maturity:** Near C2; complete this after LOCAL-01.

**Remaining work:**

- prove prepare/open/return/register/auto-stack/reopen/abandon entirely from
  Studio across restart;
- include working-copy sessions and derived provenance in backup/restore and
  catalog relocation;
- validate TIFF profile, bit depth, dimensions, naming, and conflict behavior
  on installed macOS/Windows/Linux packages;
- finish derived-pair/stack presentation and clear stale/missing states;
- keep watch-folder auto-import and proprietary scripting out of scope.

**Acceptance gate:** originals remain byte-identical; unchanged, missing,
source-mutated, stale, and conflicting returns are visible; publication is
atomic; reopen/backup/restore/export work without CLI assembly.

## CULL-01 — Transactional keyboard review and evaluated assistance

**Maturity:** Manual keyboard workflow approaches C2; analysis remains C1.

**Remaining work:**

- close review atomicity findings in COR-01;
- verify Pick/Reject/Unflag/rating/colour, auto-advance, previous-review undo,
  filters, and Survey comparison under paging, collapsed stacks, and restart;
- persist fingerprints/proposals incrementally with source-identity invalidation,
  bounded storage, dismiss state, cancellation, and throttling;
- produce fingerprints through one accepted image resource path for supported
  RAW/raster orientation and colour;
- replace or explicitly bound pairwise O(N²) grouping;
- distinguish exact-byte groups from heuristic aHash groups in UI and reports;
- measure precision/recall and false-positive cost on approved corpora;
- never auto-delete or auto-reject. Auto-stack needs a separate accepted action
  threshold and explicit user initiation.

**Acceptance gate:** the keyboard loop needs no pointer; a committed mutation
cannot be reported as failed; review remains responsive during analysis;
heuristic thresholds have metrics and known failures; cancel/restart/missing
source/corrupt decode/stale fingerprint/resource exhaustion are deterministic.

## OFFLINE-01 — Offline-original editing

**Maturity:** C1. Status/reconnect exists, but Loupe/Develop still do not render
from the proxy.

**Remaining work:**

- close all manifest/publication findings in COR-01;
- define baseline-vs-recipe-baked proxy semantics before any proxy render path;
- route Loupe/Develop/Before-After/scopes to a verified proxy only under explicit
  media state, with no double application of recipe operations;
- re-render from the verified original after reconnect and compare
  resolution-dependent results;
- add Studio create/delete/pin/storage/corruption management;
- define explicit asset/collection generation, cancellation, byte quota,
  pinning, eviction, disk-full, cleanup, backup/restore, and relocation;
- retain fail-closed full-resolution export while the original is unavailable.

**Acceptance gate:** an offline original can be viewed, graded, saved, reopened,
and compared in Studio; edits remain canonical recipes; reconnect returns to
original authority; stale/corrupt proxies are explicit; pinned data survives
eviction; source bytes remain unchanged.

## DISPLAY-01 — Per-display ICC presentation

**Maturity:** C1 on macOS contract paths; Windows/Linux discovery remains a
fail-closed stub and does not count as platform completion.

**Remaining work:**

- implement and package real Windows/Linux monitor-profile discovery and change
  lifecycle or explicitly reduce the supported-host claim;
- bind window/screen changes to one C++ presentation owner;
- apply presentation consistently to Gallery, Loupe, Develop, Before/After,
  comparison, magnifier, and declared display-referred surfaces;
- explicitly define scope pixel kind;
- prove CPU/GPU matrix/LUT parity, invalidation, corrupt/missing profiles, and
  multi-display performance;
- define the SDR/HDR boundary.

**Acceptance gate:** display movement changes presentation only; recipe/history/
catalog/export remain unchanged; every view declares pixel kind; transformed
pixels are compared directly; unsupported states are visible.

## INGEST-01 — Packaged native camera transport

**Maturity:** C1 fixture contract only; no native adapter or hardware evidence.

**Remaining work:**

- decide/package ImageCaptureCore, WPD/WinRT, and libgphoto2/libmtp ownership,
  licence, and support matrix;
- implement device/session enumeration, permission, timeout, disconnect,
  reconnect, object identity, cancellation, and restart resume;
- reuse the verified Copy/second-copy planner; retain Move/camera-delete
  rejection;
- finish Studio source/progress/report/retry/permission UX;
- run real camera/card source-preservation and failure tests.

**Acceptance gate:** each claimed host has a packaged adapter; sessions recover
from disconnect/cancel/restart; reports are exact; device originals are
unchanged; filesystem mounts are not mislabeled native.

## IQ-01 — Camera/profile and denoise quality admission

**Maturity:** C1 fixture probe only.

Build a licensed redistributable corpus plus private extension across skin,
saturated fabrics, foliage, mixed light, underexposure, clipped highlights, high
ISO, moiré, hot pixels, and lens extremes. Record provenance and measure colour,
hue, highlights, detail/noise, halos, false colour, and scaling with blinded
human review. Define camera/profile/lens update workflows. Admit a real denoise
provider only after runtime, weights, licence, package, memory, cancellation,
updates, and no-model behavior are accepted.

**Acceptance gate:** camera support means image-quality evidence, not decode
alone; changes carry before/after results and known failures; preview and native
export are covered; deterministic defaults never change silently.

# P2 — Frozen important extensions

Do not implement these while the WIP freeze is active.

- **FORMAT-01 — HEIC/HEIF:** owned colour-managed decode remains blocked by
  dependency/licence/package evidence.
- **FORMAT-02 — DNG and browse Smart Preview:** choose owned converter/encoder;
  preserve Copy-only/browse-only semantics and keep this class distinct from
  offline-edit proxies.
- **META-01:** add only a target cohort's required IPTC Extension/contact/scene/
  subject fields, controlled vocabularies, privacy, facets, and XMP authority.
- **CONVERT-01:** real Lightroom/Capture One readers require read-only,
  versioned, reportable, resumable, packaged conversion into a new destination.
- **DELIVERY-01:** advanced overlays, fonts, templates, printing, and publishing
  need separate delivery ownership and must not mutate Develop.
- **SPECIALIZE-01:** ADR-0153's tethered probe is a deferred unsupported state,
  not a selected product direction. Do not expand it without an external cohort,
  hardware/corpus access, and a dated choice between tethering and HDR/Panorama.

# P2 — Real AI admission

The existing deterministic proposals and Studio pane are contract/developer
fixtures. Freeze their scope. Do not add another stub, provider, prompt surface,
or product claim.

Before any real model ships, AI-00 must record runtime/model identity, source,
licence/GPL compatibility, notices, size/update/cache policy, credentials,
local/remote payload, retention/logging/telemetry/training policy, memory,
concurrency, timeout/retry/cancellation, offline behavior, threat review, and a
licensed evaluation corpus.

- **AI-01 global edits:** blocked by AI-00 and IQ-01; apply only ordinary,
  reviewable recipe diffs.
- **AI-02 semantic masks:** blocked by AI-00 and LOCAL-01; results must use the
  canonical mask DAG.
- **AI-03 shoot consistency:** blocked by AI-00/AI-01 and a batch evaluation
  set; each destination keeps independent proposal/history state.
- **AI-04 metadata/culling/similarity:** blocked by AI-00/CULL-01; suggestions
  remain non-authoritative and never auto-delete/reject.
- **AI-05 retouch/generated pixels:** blocked by AI-00/LOCAL-01 and immutable
  generated-derived publication with provenance and backup/restore.

# P3 — Deferred product areas

Print layout, map/geocoding, slideshow, remote publishing/cloud collaboration,
shared multi-writer catalogs, face identity/biometrics, broad natural-language
search, proprietary editor automation, and arbitrary legacy compatibility must
not displace the queue above.

# Cross-cutting acceptance

Every Ready tranche adapts this minimum set rather than weakening it:

```text
python3 configs/source_roots.py verify
python3 Ravo/tools/freeze_legacy_manifest.py --check
python3 Ravo/tools/check_ravo_dependency_boundary.py
cmake --preset mac_clang_debug -DBUILD_TESTING=ON
cmake --build --preset mac_clang_debug --target \
  ravo_catalog_tests ravo_desktop_command_tests ravo_contract_tests ravo_studio
ctest --test-dir build/mac_clang_debug --output-on-failure
```

Matching Windows and Linux CI jobs must run for cross-platform changes. Release
candidates additionally require Release corpus/performance/IQ probes and
installed-package smoke.

For persisted state, test create/migrate/reopen/backup/restore/relocate. For
asynchronous or multi-step publication, test stale revision, conflict,
cancellation before and after commit, disconnect, source disappearance,
disk-full, allocation/resource exhaustion, restart, and exact partial state.
For images, test CPU reference, supported-GPU comparison, ICC identity,
preview/export equality, and source immutability. For dependencies, models,
profiles, hardware, or corpora, record pins, licences, notices, provenance, and
reproducibility.

Studio must preserve keyboard operation, focus order, localization, high-DPI,
accessibility, and explicit unavailable reasons. No QML owner may acquire SQL,
codec, image algorithm, durable mask, job, or conflict policy.

A skipped test is not a pass. An unsupported state is explicit. A fallback is
named, observable, quality-bounded, and accepted by contract; otherwise fail
closed.
