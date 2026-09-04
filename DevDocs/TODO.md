# Ravo product execution TODO

> **Status:** ordered residual queue
>
> **Updated:** 2026-09-04
>
> **Review basis:** `main` through ADR-0156 XMP fail-closed multi-instance + LOCAL-01 history/undo
> measurement harness and AI-01/02 Studio stub proposal inspect/apply/reject chrome;
> next free ADR **0156**.

This file contains only unfinished product work, dependencies, risks,
verification, and acceptance gates. Current behavior belongs in
`Ravo/README.md`, [ARCHITECTURE.md](ARCHITECTURE.md),
[TESTING.md](TESTING.md), code, tests, and accepted ADRs. Package evidence
belongs in [Packaging.md](Packaging.md). Product themes without an executable
contract belong in [ProductRoadmap.md](ProductRoadmap.md).

Ravo's near-term objective is not feature count. It is a professional photo
manager and non-destructive grading application that remains safe, predictable,
colour-correct, and responsive with real catalogs, removable storage, repeated
delivery work, and long editing sessions.

Leftover-faithful algorithm ports remain closed by ADR-0106. Nothing below
reopens the deleted GTK application, dynamic IOP ABI, Lua surface, old catalog,
or OpenCL path. GPU work remains an Engine QRhi adapter with CPU as the
correctness reference.

## Queue discipline

Priority is dependency-based:

- **P0 — repository or release gate:** a red main branch, source-safety defect,
  catalog-integrity defect, or unbounded resource path preempts product breadth.
  Corpus, performance, rendering-consistency, and installed-package evidence
  block a release-ready claim.
- **P1 — complete the professional baseline:** finish the current daily
  workflows end to end through domain/service/engine/adapter/CLI/Studio rather
  than starting more first-Ready contracts.
- **P2 — important extension:** valuable after the P0/P1 baseline, or blocked by
  dependency, licence, privacy, hardware, market, or product decisions.
- **P3 — deferred specialization:** research or cohort-specific work that must
  not displace the baseline.

Every implementation tranche must name its owner, lifecycle, persisted or
machine contract, failure/cancellation behavior, smallest validation set, and
acceptance gate. QML presents state and forwards intent; it does not own SQL,
image algorithms, durable masks, codecs, jobs, or conflict policy.

A service/CLI stub or a first-Ready contract is not a completed photographer
workflow. Completed facts must move to their authoritative ADR, architecture,
product documentation, code, and tests; do not retain `Closed in this tranche`
history in this queue.

### Vertical-completion freeze

ADR-0144 through ADR-0149 move in the intended directions: monitor ICC,
multi-instance local adjustments, offline proxies, deterministic culling,
native-ingest lifecycle, and external-editor working copies. The correction is
execution depth, not a reversal of those decisions.

Until the current P0 head is green and at least one P1 workflow reaches its
end-to-end Studio acceptance gate:

- do not introduce another P1/P2 capability ID, additional AI stub, or
  service-only placeholder surface;
- finish residual Studio, persistence, backup/restore, performance, and
  cross-platform behavior on the accepted contracts;
- do not label a first-Ready service/CLI slice as a finished product outcome;
- keep ADR-0149 average-hash output explicitly heuristic and non-authoritative;
- keep SPECIALIZE neutral until an external photographer cohort supplies
  evidence for HDR/Panorama or tethered-studio priority.

A newly discovered data-loss, security, catalog-corruption, or colour-integrity
defect preempts this freeze and all product work.

## Current execution lanes

The release-evidence lane and product-completion lane may run in parallel only
while `main` is green. A red head immediately returns all owners to CI-01.

### Release-evidence lane

| Order | ID | Priority | Outcome |
| ---: | --- | --- | --- |
| 1 | CI-01 | P0 | Keep the latest main SHA green and make the gate enforceable |
| 2 | REL-01 | P0 | Prove source safety, catalog durability, interruption recovery, and upgrade |
| 3 | PERF-01 | P0 | Freeze end-to-end Gallery, viewer, Develop, and large-library budgets |
| 4 | IQ-00 | P0 | Finish corpus matrix after ADR-0151 CPU-gold first Ready |
| 5 | REL-02 | P0 | Prove installed packages and backup/restore on supported hosts |

### Product-completion lane

| Order | ID | Priority | Outcome |
| ---: | --- | --- | --- |
| 1 | LOCAL-01 | P1 | Finish professional multi-instance local adjustments in Studio |
| 2 | DISPLAY-01 | P1 | Finish per-display ICC presentation on all supported hosts and views |
| 3 | OFFLINE-01 | P1 | Make offline proxies usable for actual Loupe/Develop work |
| 4 | CULL-01 | P1 | Finish Studio keyboard chrome and evaluated duplicate/burst assistance |
| 5 | EDITIN-01 | P1 | Finish the Studio external-editor round trip |
| 6 | INGEST-01 | P1 | Replace native-ingest stubs with packaged adapters and hardware evidence |
| 7 | IQ-01 | P1 | Establish camera/profile and denoise quality admission |

# P0 — Repository and release gates

## CI-01 — Green and enforceable main

**Status:** Partially closed.

The original CI-01 failures were repaired. CI run `33853463156` on
`e7eac8dc2472645178c70814a2ea199d2bd981ca` is the first recorded same-SHA
success for Static checks plus macOS, Windows, and Linux Debug jobs after the
portable number parser, GPU classification, pixel-reopen, X-Trans, backup,
Windows long-path, and test-inventory repairs.

The repository still allows direct pushes to an unprotected `main`, and the
Debug matrix alone is not a release gate.

**Remaining work:**

- require Static checks and the macOS/Windows/Linux jobs through a repository
  ruleset or equivalent branch policy;
- require the latest `main` SHA to finish green before more product-state
  commits or tags are accepted;
- add a Release or RelWithDebInfo compile plus minimal CLI/Studio smoke job;
- make tag packaging and release publication depend on the same green source
  SHA, with no red-commit or skipped-job path;
- document the required checks and emergency override policy;
- keep flaky tests visible and owned; do not convert them to permanent skips.

**Acceptance gate:**

- the latest `main` SHA has one successful Static/macOS/Windows/Linux matrix;
- ordinary pushes cannot bypass required checks;
- a release candidate compiles and launches outside the Debug-only path;
- tag publication cannot run from a failed, cancelled, or incomplete gate;
- no required job is `continue-on-error`, silently narrowed, or replaced by a
  documentation-only run.

## REL-01 — Real mixed-photo corpus, source safety, and recovery

**Status:** Active release evidence. Host-local report:
`/Users/ethan/Documents/RavoEvidence/reports/20260904_191808/` (not in git).

**Dependency:** an explicit read-only mixed RAW/raster tree in
`RAVO_PHOTO_CORPUS`; a Release build; all generated state under a unique
temporary root. Private photos and reports remain outside the repository.

The corpus must cover multiple Bayer and X-Trans cameras, DNG, JPEG, PNG, TIFF,
adjacent XMP, same-stem RAW+JPEG, malformed/truncated/unsupported inputs, large
files, Unicode paths, duplicate content, removable-storage paths, and source
disappearance.

**Required scenarios:**

- Add, Copy, Move, filesystem-card ingest, native-ingest stub lifecycle,
  verified second copy, rename, cancellation, disconnect, and resume;
- ratings, labels, reject state, keywords, metadata, collections, virtual
  copies, stacks, history, snapshots, multi-instance recipes, and batch edits;
- preview rebuild, folder relink, XMP import/export/conflict resolution,
  external-editor working copies and derived assets, offline proxies, cull
  reports, and catalog reopen;
- backup/verify/restore for every authoritative or retained support tree,
  including derived/external-editor state, DNG/Smart Preview state, and any
  accepted offline-proxy state;
- destination collision, disk-full simulation, process termination at
  publication boundaries, corrupt support files, stale revisions, and restart;
- upgrade from the latest published package/catalog to the candidate.

Suggested entry point:

```text
RAVO_PHOTO_CORPUS=/absolute/private/photos \
  build/<release-preset>/Ravo/tests/ravo_catalog_tests \
  --gtest_filter=CatalogServiceTest.PrivatePhotoManagementReleaseProbePreservesCorpus
```

**Acceptance gate:**

- every source retains exact SHA-256, size, and modification time unless an
  explicit successful destructive command owns the mutation;
- unresolved conflicts publish no catalog row or destination artifact;
- interruption leaves the previous complete state or the new complete state,
  never a half-published authority;
- reopen, relink, upgrade, backup/restore, virtual copies, stacks, recipes,
  metadata, XMP baselines, proposals, proxies, and derived provenance remain
  coherent;
- unsupported inputs and missing dependencies fail structurally without an
  incidental decoder, renderer, or lower-quality fallback;
- reports distinguish imported, duplicate, skipped, unsupported, cancelled, and
  failed items; an unset corpus or skipped test is not evidence.

**Risk:** one host, storage device, GPU, or camera family does not generalize to
another.

## PERF-01 — End-to-end interaction and large-library budgets

**Status:** Active release evidence. Measurement harness expanded for
Gallery→viewer→Develop latency fields (`ravo.perf01.report/v1` via
`interactive_perf_report.h` + `StudioGalleryViewerDevelopPerformanceProbe`);
budgets still unfrozen. Do not start PERF-02 admit optimizations yet.

Host-local Gallery→viewer→Develop JSONL evidence (not in git):
`~/Documents/RavoEvidence/reports/20260904_201715/`.

**Dependency:** REL-01, a Release build, declared storage and power state, and
explicit cold/warm cache state.

Measure complete user-visible paths:

- folder enumeration, ingest publication, first placeholder, first thumbnail,
  viewport-complete thumbnails, and exact browse publication;
- adjacent-photo select/revisit; Fit, Fill, Actual Size, and 1:1 ROI;
- first interactive Develop frame, latest frame after a rapid burst, settled
  preview, save, and reopen;
- monitor-presentation transform and native frame swap separately from owned
  image publication;
- active-query paging/facets, exact-duplicate scan, burst proposal, heuristic
  fingerprint scan, and their cancellation/memory behavior at 100,000 photos;
- offline-proxy creation, verification, offline selection, and reconnect;
- external-editor working-copy preparation and returned-file registration.

Record at least two warmups and eight samples per case with P50/P90/max, source
kind, file count, cache state, host, storage, workers, peak owned bytes, display
refresh rate, GPU backend, and power state.

```text
RAVO_INTERACTIVE_PERF_CATALOG=/temporary/private-corpus/library.sqlite \
RAVO_INTERACTIVE_PERF_ASSET_ID=<imported-raw-asset-id> \
  build/<release-preset>/Ravo/tests/ravo_desktop_command_tests \
  --gtest_filter='StudioInteractivePreviewPerformanceProbe.*:StudioGalleryViewerDevelopPerformanceProbe.*'
```

Optional structured report append (host-local):

```text
RAVO_INTERACTIVE_PERF_REPORT_PATH=~/Documents/RavoEvidence/reports/<ts>/perf01_cases.jsonl
RAVO_INTERACTIVE_PERF_WARMUPS=2
RAVO_INTERACTIVE_PERF_RECORDED_SAMPLES=8
```

**Acceptance gate:**

- host-local budgets are frozen only from repeatable candidate measurements;
- reports separate catalog query, decode/demosaic, processing, encoding,
  presentation transform, image ownership, QML publication, and frame swap;
- rapid intents never publish an older frame after a newer one;
- background ingest, fingerprinting, proxy generation, or export cannot starve
  foreground Develop;
- memory, cancellation, colour/profile state, and deterministic publication stay
  bounded;
- a same-corpus rerun with the frozen budgets passes.

### PERF-02 — Admit only measured optimizations

Start PERF-02 only after PERF-01 identifies a dominant owner. Evaluate one
candidate at a time: byte-bounded concurrency, profiled browse resources,
byte-bounded adjacent-preview LRU, deferred metadata, non-PNG rebuildable browse
encoding, or additional QRhi stages/transfer elimination.

Adopt a candidate only when same-corpus end-to-end P90 improves without
foreground, determinism, cancellation, memory, source-safety, colour/profile,
HDD-seek, package-size, or CPU-reference regression. Embedded JPEGs,
placeholders, Smart Previews, and offline proxies must retain their declared
roles and cannot become silent RAW correctness references.

## IQ-00 — Rendering and colour consistency gate

**Status:** First Ready on `main` (ADR-0151). Full corpus matrix remains open.

**Closed in ADR-0151:** CPU is gold for persist preview / settled save / export /
reopen / CLI PNG; interactive GPU may run only on the live develop path and
must match within `kIqGpuCpuWorkingAbsTolerance` or fail-closed; contract tests
+ `catalog probe --json` `iq_consistency` policy object. Aligns with existing
persist/export CPU-gold Engine split.

CPU remains the correctness reference. GPU and display presentation may improve
latency but cannot silently alter recipe state, supported-input boundaries,
colour transforms, or export bytes.

**Required matrix:**

- raster and supported Bayer/X-Trans RAW;
- default and edited white balance, exposure, highlight recovery, curves,
  multi-instance local adjustments, masks, colour grading, denoise,
  lens/perspective/crop/canvas, sharpening, frame, and watermark;
- built-in/file ICC input and output profiles, proof state, monitor matrix/LUT
  profiles, rendering intents, and privacy modes;
- interactive preview, settled preview, close/reopen, CLI probe/render, 1:1 ROI,
  and JPEG/PNG/TIFF export;
- CPU-only, supported GPU, GPU-unavailable, resource-exhausted, cancelled, and
  corrupt-profile execution.

**Acceptance gate:**

- preview, settled save, reopen, and export use one canonical recipe with
  documented tolerances;
- CPU/GPU and preview/monitor comparisons use owned pixel results and approved
  metrics, never screenshots;
- unsupported GPU stages are explicit and do not choose an unreported
  lower-quality algorithm;
- ICC identity and embedded bytes match the declared output;
- moving a window between displays changes presentation only, never recipe,
  history, catalog revision, or export;
- overlapping 1:1 ROI and native export regions agree after the same geometry;
- regressions retain input, recipe, backend, profile identity, hashes, and error
  metrics.

## REL-02 — Installed package, upgrade, and restore evidence

**Status:** P0 after CI-01 enforcement and candidate artifacts exist.

Validate the final DMG, Windows ZIP, Linux AppImage, and Linux DEB after each is
copied away from the build tree.

**Required installed smoke:**

1. launch Studio and bundled CLI;
2. create/open a catalog; import one raster and one supported RAW;
3. browse, rate, edit, save, reopen, and export JPEG plus TIFF or PNG;
4. exercise monitor-profile status, multi-instance recipe reopen, cull report,
   offline-proxy status, and external-editor working-copy preparation;
5. rebuild previews; create/verify a backup; restore to a new path; reopen;
6. upgrade a catalog created by the latest published release;
7. verify locales, platform/image plugins, SQLite, ICC, GPU reporting, long
   paths, and installed runtime closure;
8. remove the package without touching user catalogs or originals.

**Acceptance gate:**

- every artifact launches on a clean supported host without build-tree paths or
  undeclared runtime dependencies;
- package smoke uses the same machine contracts as source builds;
- upgrade failure is explicit and leaves the old catalog recoverable;
- backup/restore stages and verifies all retained support state and excludes
  originals/rebuildable caches according to contract;
- package version, release notes, README, formats, signing/notarization claims,
  and actual artifacts agree.

# P1 — Complete the professional baseline

Product work below may proceed only from a green `main`. Finish one vertical
workflow before admitting another product area.

## LOCAL-01 — Professional multi-instance local adjustments

**Status:** ADR-0145 serialization/inspection plus Studio Exposure/Color Balance
RGB instance chrome (list/select/name/enable/bypass/reorder/add/delete/duplicate
with independent Studio-owned mask clone) are on `main`; selected-instance leaf
authoring for brush/path/linear/radial/parametric (luminance/colour range) via
the existing mask pipeline is on `main` with fail-closed external/shared sibling
attachments. Session undo/redo and develop history now keep discrete entries for
add/duplicate/reorder/bypass/enable/delete and selected-instance mask edits
(instance vector + masks restore). ADR-0156 fail-closes XMP export/status/import
when CRS cannot represent multi-instance locals. Overlay/composition polish and
style/batch still residual.

**Remaining work:**

- deepen overlay visibility, feather, flow/density where accepted, opacity, and
  Add/Subtract/Intersect/Invert composition UX beyond the existing leaf editor;
- one canonical coordinate path through orientation, lens, Perspective, crop,
  Canvas, preview scaling, 1:1 ROI, and export;
- snapshots, style/preset, selective copy/paste, multi-selection batch,
  virtual-copy, and reopen completeness for instance vectors;
- GPU parity only after CPU equality and PERF-01 evidence;
- split oversized Studio owners while implementing the workflow; do not grow
  `Main.qml`.

**Acceptance gate:**

- a photographer can create and edit at least two named local instances in
  Studio without CLI assistance;
- preview, settled save, export, reopen, history, undo, style, and copy/paste
  agree;
- masks stay aligned through accepted geometry and 1:1 inspection;
- QML owns no durable geometry or mask pixels;
- stale revision, cancellation, malformed graph, excessive points, allocation
  failure, and partial batch behavior are tested;
- legacy single-instance recipes remain bit-compatible.

**Blocks:** real semantic-mask and replayable retouch providers.

## DISPLAY-01 — Per-display ICC presentation

**Status:** ADR-0144 macOS discovery, explicit sRGB fallback, synthetic CPU
transform references, presenter/service contract, and CLI status are on `main`;
Win/Linux discovery remains fail-closed to `fallback_srgb` with platform
reasons (`windows_monitor_discovery_unavailable` /
`linux_monitor_discovery_unavailable`). Cross-platform Studio presentation is
incomplete.

**Remaining work:**

- Real Windows and Linux monitor discovery and profile-change lifecycle (replace
  fail-closed stub reasons with system ICC bytes);
- bind Studio window/screen changes to one C++ presentation owner;
- apply the transform consistently to Gallery, Loupe, Develop, Before/After,
  comparison, magnifier, and any view declared display-referred;
- define whether scopes consume output-profile pixels or
  display-transformed pixels, and keep the choice explicit;
- GPU presentation parity against synthetic matrix/LUT CPU references;
- corrupt/missing/changed profile state and cache invalidation;
- SDR/HDR display boundary without silently treating an HDR screen as sRGB;
- multi-display corpus evidence and performance budgets.

**Acceptance gate:**

- moving Studio between displays changes presentation only;
- recipe, history, catalog revision, settled preview authority, and exported
  bytes remain unchanged;
- every relevant view declares its pixel kind;
- matrix and LUT profiles produce repeatable CPU and supported-GPU results;
- missing/corrupt/unsupported profiles are visible machine states;
- validation compares owned transformed pixels, not screenshots.

## OFFLINE-01 — Offline-original editing

**Status:** ADR-0146 manifest/service/CLI create/list/verify/status/reconnect and
offline recipe writes are on `main`. It is not yet an offline editing workflow:
Loupe/Develop do not consume the proxy while the original is absent.

**Remaining work:**

- route Loupe/Develop preview requests to a verified offline-edit proxy only
  under the explicit media-state contract;
- re-render from the verified original after reconnect and compare accepted
  resolution-dependent results;
- Studio media-state, create/delete/pin, storage, corruption, and reconnect UI;
- background generation policy by explicit asset/collection, with cancellation;
- byte quota, pinning, eviction, disk-full, corruption, and cleanup;
- backup/restore and catalog-relocation policy for
  `offline-edit-proxies/`;
- clarify proxy format, bit depth, profile, maximum edge, and regeneration when
  recipe/source identity changes;
- retain fail-closed full-resolution export while the original is unavailable.

**Acceptance gate:**

- an offline original can be selected, viewed, graded, saved, reopened, and
  compared in Studio through the declared proxy;
- edits remain canonical recipes and re-render from the original after verified
  reconnect;
- export never silently uses a proxy as full-resolution authority;
- stale/corrupt/missing proxies are explicit and recoverable;
- quotas are byte-bounded and pinned proxies are not auto-deleted;
- source bytes remain unchanged and backup/restore behavior is tested.

## CULL-01 — Keyboard-first review and evaluated assistance

**Status:** ADR-0147/0149/0150/0155 service/CLI foundations plus Studio Library
filter chips, keyboard cull chrome, and ADR-0155 burst/stack Survey compare pair
(open + previous/next step with optional 1:1 ROI sync) are on `main`. Evaluated
assistance/fingerprint lifecycle remain.

**Correction:** ADR-0149 aHash is a bounded visual heuristic. It is not a
quality-complete near-duplicate classifier, does not currently cover every RAW
path, and must not be presented as delete authority or as an evaluated
professional similarity model.

**Closed in ADR-0150:** transactional Pick/Reject/rating/colour-label with
optional auto-advance and previous_review undo payload; schema v16 `picked`;
no auto-delete.

**Closed in Studio chrome:** Pick/Reject/Unflag shortcuts and Gallery review
flag control dispatch through `apply_cull_review` with selection-order
auto-advance; `selectedPicked` + asset `picked` role.

**Remaining work:**

- deepen visible shortcut teaching / cull-mode HUD if still needed beyond
  command palette + review bar;
- deepen Survey burst-compare UX beyond ADR-0155 pair open/step if needed;
- Library filter chips for picked/rejected/unreviewed and optional
  exact/near-dup/burst suggestion groups landed; deepen persistent fingerprint
  lifecycle, dismiss state, and corpus precision/recall next;
- persistent/incremental fingerprint and proposal lifecycle with invalidation on
  source identity changes, bounded storage, dismiss state, and cancellation;
- produce fingerprints through an accepted Ravo image resource path, including
  supported RAW, orientation, and colour handling, rather than creating a
  second hidden decode authority;
- evaluate exact, burst, and heuristic-similarity precision/recall and
  false-positive cost on licensed/private corpora;
- 100,000-photo paging, hash/fingerprint scheduling, I/O throttling, and memory
  budgets;
- never auto-delete or auto-reject; auto-stack requires a separate accepted
  confidence/action contract.

**Acceptance gate:**

- the main keyboard review loop works without pointer interaction;
- foreground review and 1:1 inspection remain responsive while analysis runs;
- exact and heuristic groups are visibly distinct;
- heuristic thresholds have recorded corpus metrics and known failures;
- source files are never modified and accepting a burst uses ordinary stack
  history/conflict behavior;
- cancel/restart, missing source, corrupt decode, stale fingerprint, and
  resource exhaustion are deterministic.

**Blocks:** real AI culling/similarity quality claims.

## EDITIN-01 — Studio external-editor round trip

**Status:** ADR-0122/0136/0139/0154 service/CLI/Studio path covers prepare,
check-returned, durable abandon, reopen after restart, and conflict machine
states (`pending` / `modified` / `missing_working_copy` / `source_conflict` /
`stale_catalog`). Package-host matrix and Gallery polish remain open.

**Remaining work:**

- derived-pair Gallery/stack presentation polish;
- profile/bit-depth equality and package smoke on each host;
- naming templates beyond default working-copy paths;
- keep watch-folder auto-import and proprietary editor scripting out unless a
  later dated decision supplies deterministic ownership.

**Acceptance gate:**

- create, open, return, register, auto-stack, reopen, backup/restore, and export
  work from Studio without CLI assembly;
- originals remain byte-identical;
- unchanged/missing/unsupported/stale/conflicting returns fail visibly;
- partial derived publication is impossible;
- package behavior matches the service/CLI contract.

## INGEST-01 — Packaged native camera transport

**Status:** ADR-0148 session identity, support probe, resume checkpoints,
Copy-only planner path, structured reports, and `ptp-stub` are on `main`.
No real native adapter has shipped.

**Remaining work:**

- macOS ImageCaptureCore, Windows WinRT/WPD, and Linux libgphoto2/libmtp
  dependency/licence/package decisions;
- real device/session enumeration, permission, timeout, disconnect, reconnect,
  object identity, and cancellation;
- verified primary and optional second-copy publication through the existing
  planner;
- checkpoint cleanup/abandon policy and resume after process restart;
- Studio source selector, progress, per-item report, retry, and permission UX;
- real camera/card hardware matrix and source-preservation evidence.

**Acceptance gate:**

- at least one declared native adapter per supported host is packaged or the
  host is explicitly unsupported;
- disconnect/cancel/restart leaves reusable sessions and exact per-item state;
- Move and camera delete remain rejected unless separately accepted;
- originals on the device are unchanged;
- no filesystem mount is reported as native PTP/MTP;
- hardware tests cover duplicate names, unavailable destinations, second-copy
  failure, and resume.

## IQ-01 — Camera/profile and denoise quality admission

**Status:** ADR-0152 evaluation-corpus contract plus CPU denoise / camera-profile
probes (fail-closed without corpus; fixture-backed first Ready) are on `main`.
Full licensed corpus matrix and learned denoise admission remain open.

**Remaining work:**

- licensed redistributable quality corpus plus private-camera extension set for
  skin, saturated fabrics, foliage, tungsten/LED, underexposure, clipped
  highlights, high ISO, moiré, hot pixels, and lens extremes;
- camera model/firmware/lens/illuminant/exposure/profile provenance depth beyond
  the fixture probe;
- repeatable colour accuracy, hue stability, highlight recovery, noise/detail,
  halo, false-colour, and scaling metrics plus blinded human review;
- camera input/profile and lens-database update workflows;
- separate deterministic regression from learned-model quality;
- admit a first real denoise provider only after runtime, weights, licence,
  package, memory, cancellation, update, and no-model decisions.

**Acceptance gate:**

- camera support claims include image-quality evidence, not decode alone;
- profile/model changes carry before/after results and known failures;
- preview and native-resolution export are covered;
- model absence/unsupported hardware is explicit and does not change the
  deterministic default silently;
- redistributed data, profiles, runtimes, and weights have licence and notice
  evidence.

# P2 — Important extensions

## FORMAT-01 — Owned HEIC/HEIF decode

**Status:** Blocked by ADR-0123 dependency/licence/package evidence.

Define pixels, orientation, ICC/HDR transfer metadata, alpha, multi-image
selection, malformed-container limits, memory/cancellation, and three-platform
packaging. Recognition continues to fail closed until one owned decoder ships.

**Acceptance gate:** one declared decoder produces bounded colour-managed pixels
on supported hosts; unsupported brands/features are structural; incidental
ImageIO or Qt plugin behavior never becomes a hidden contract.

## FORMAT-02 — DNG side conversion and browse Smart Preview implementation

**Status:** ADR-0141 and backup-v3 contracts exist; packaged converter/encoder
implementation remains blocked.

Choose owned converter/encoder dependencies and package/licence policy. Preserve
Copy-only side conversion, browse-only Smart Preview semantics, explicit source
hashes, cancellation, quota, and no original replacement. Do not merge this
cache class with ADR-0146 offline-edit proxies.

## META-01 — Remaining professional metadata depth

**Status:** P2 unless a journalism, sports, agency, or archive cohort promotes
it.

Define only the required IPTC Extension/contact/scene/subject-code subset,
controlled vocabularies, multi-select semantics, facets, export privacy, XMP
fingerprints, and capture-refresh authority. Do not add empty columns or panes.

## CONVERT-01 — Real foreign-catalog readers

**Status:** P2.

Lightroom `.lrcat` and Capture One readers require a dependency/licence/package
decision, vendor-version matrix, read-only parsing, Copy-mode destination
policy, persistent resume/report state, and Studio migration surface. Never
open or migrate a vendor catalog in place; report unsupported fields per item.

## DELIVERY-01 — Advanced delivery

Soft-proof-as-export, SVG/logo overlays, arbitrary fonts, decorative templates,
printing, and publishing services remain outside ADR-0129. Each needs a new
product/dependency decision and must remain ExportOptions or a derived delivery
job rather than mutating Develop.

## SPECIALIZE-01 — Select one cohort-backed vertical

**Status:** ADR-0153 records tethered-studio as the deferred P2 candidate with a
fail-closed probe stub. HDR/Panorama stays unselected. No cohort evidence yet;
real adapter work remains blocked.

Do not promote tethered-studio from the stub. Choose implementation only after
an external photographer cohort, representative workflow, hardware/corpus
availability, and measurable product advantage are recorded in a later dated
ADR.

- **HDR/Panorama:** source grouping, alignment, projection, ghost handling,
  exposure merge, colour space, memory/cancellation, crop, provenance, rebuild,
  and derived-asset publication.
- **Tethered studio:** camera support, USB/network lifecycle, reconnect, live
  view, destination, naming/metadata/style, client display, session
  organization, privacy, and package dependencies.

Do not start both. The selected track needs one service/CLI contract, one Studio
workflow, real evidence, cancellation/recovery, and source-preservation tests
before the alternate track enters implementation.

# P2 — Real AI admission

ADR-0121 and deterministic AI-01/AI-02/AI-03/AI-04 fixtures establish lifecycle
and failure contracts. They do not establish model quality. Do not add more
stubs or placeholder Studio panes.

## AI-00 — Runtime, privacy, packaging, and evaluation gate

Record the concrete provider/runtime/model identity, source, licence/GPL
compatibility, notices, package/download size, update channel, cache root,
credential ownership, local/remote payload, retention, logging, telemetry,
training-use policy, concurrency, memory, timeout, retry, cancellation, and
offline behavior.

Establish a licensed evaluation corpus and blinded review protocol across
RAW/raster, cameras, lighting, genres, skin tones, failure cases, and model
versions.

**Acceptance gate:** privacy/threat review, reproducible model identity,
dependency evidence, bounded resources, no-model behavior, and quality
thresholds are testable; originals never need to be writable.

## AI-01 — Real global edit proposals

**Blocked by:** AI-00 and IQ-01.

**Studio chrome (stub only):** inspect/apply/reject/cancel surface for existing
deterministic stub proposals is on `main`. Real provider/weights remain blocked.

Proposals remain ordinary validated recipe diffs with exact preview and field
diff before apply. Reject/cancel changes nothing; apply creates normal
history/undo. Quality must pass the approved corpus and human review.

## AI-02 — Real semantic selections

**Blocked by:** AI-00 and LOCAL-01.

**Studio chrome (stub only):** semantic-mask stub create + inspect/apply/reject
shares the AI proposal dialog. Real segmentation quality remains blocked.

Results must become bounded canonical masks in the accepted coordinate system.
Uncertain/empty results stay visible. Provider-specific bitmaps cannot become a
second mask authority.

## AI-03 — Real shoot consistency

**Blocked by:** AI-00, AI-01, and an accepted batch evaluation set.

Every destination is explicit and gets independent proposal/history state.
Partial completion and cancellation use the existing batch contract; per-image
exceptions remain inspectable.

## AI-04 — Real metadata, culling, and similarity

**Blocked by:** AI-00 and CULL-01.

Suggestions remain distinct from catalog facts and never auto-reject, delete,
publish, or write identity-sensitive metadata. ADR-0142 stub suggestions prove
only lifecycle. Real quality, batch acceptance, people/face policy, and Studio
surfaces remain unfinished.

## AI-05 — Retouch and generated pixels

**Blocked by:** AI-00, LOCAL-01, and immutable generated-derived publication.

Replayable dust/blemish work may propose canonical Retouch regions.
Non-replayable pixels must publish an immutable derived asset with source
revision, model identity, settings, output hash, privacy provenance,
backup/restore policy, and visible generated status. Failure/cancellation
publishes nothing partial; reopen/export cannot require the provider online.

# P3 — Deferred product areas

The following must not displace the queue above:

- print layout and printer/profile ownership;
- map, geocoding, and location-service privacy;
- slideshow and presentation delivery;
- remote publishing and cloud collaboration;
- shared/multi-writer catalogs;
- face identity and biometric privacy;
- broad natural-language library search;
- proprietary editor automation;
- arbitrary legacy IOP, GTK, Lua, OpenCL, or in-place foreign compatibility.

Each needs a target cohort, independent Ravo contract, privacy/security review,
offline/failure behavior, package plan, and measurable acceptance gate.

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

For persisted state, add create/migrate/reopen/backup/restore/relocate tests.
For asynchronous work, add stale revision, conflict, cancellation, disconnect,
source disappearance, disk-full, resource exhaustion, restart, and partial
publication tests. For image behavior, add CPU reference, supported-GPU
comparison, ICC/profile identity, preview/export equality, and source
immutability. For new dependencies, models, profiles, hardware, or corpora, add
pin, licence, package, notice, provenance, and reproducibility evidence.

Studio work must preserve keyboard-only operation, focus order, localization,
high-DPI behavior, accessibility, and explicit unavailable reasons. When work
touches registered oversized QML/C++ owners, split the workflow owner and reduce
registered debt; do not grow `Main.qml` or stop product work for a detached UI
rewrite.

A skipped test is not a pass. An unsupported state is explicit. A fallback is
named, observable, quality-bounded, and accepted by contract; otherwise fail
closed.
