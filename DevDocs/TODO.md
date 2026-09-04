# Ravo product execution TODO

> **Status:** ordered execution queue
>
> **Updated:** 2026-09-04

This file contains only unfinished product work, dependencies, risks, validation,
and acceptance gates. Current behavior belongs in `Ravo/README.md`,
[ARCHITECTURE.md](ARCHITECTURE.md), [TESTING.md](TESTING.md), code, tests, and
accepted ADRs. Three-platform package evidence belongs in
[Packaging.md](Packaging.md). It blocks a release-ready claim, not independent
product implementation. Product themes that are not ready for execution belong
in [ProductRoadmap.md](ProductRoadmap.md).

Ravo's near-term objective is not to maximize feature count. It is to become a
photo manager and non-destructive grading application that a professional
photographer can trust with a large real catalog, removable storage, repeated
delivery work, and long editing sessions.

Leftover-faithful algorithm ports remain closed by ADR-0106. Nothing below
reopens the deleted GTK application, dynamic IOP ABI, Lua surface, old catalog,
or OpenCL path. New GPU work remains an Engine QRhi adapter with CPU as the
correctness reference.

## Priority and queue rules

Priority is dependency-based:

- **P0 — repository or release gate:** CI, source-safety, and catalog-integrity
  failures block product breadth. Evidence-only performance and package gates
  block a release-ready claim but may run in parallel with independent P1 work.
- **P1 — next professional workflow:** the highest-value daily workflow gaps.
  Contract-changing work requires a dated accepted ADR before implementation.
- **P2 — important extension:** valuable after the P0/P1 foundation is proven,
  or blocked by a dependency, licence, privacy, or product decision.
- **P3 — deferred specialization:** research or market-specific work that must
  not displace the current professional baseline.

Every implementation tranche must name its domain/service/engine/adapter/desktop
owner, lifecycle, persisted or machine contract, cancellation and failure
behavior, smallest validation set, and acceptance gate. QML presents state and
forwards intent; it does not own SQL, image algorithms, durable masks, codecs,
jobs, or conflict policy.

Completed slices must be removed from this file after durable facts move to
their authority. Stub or fixture implementations prove a contract only; they do
not prove product quality.

## Current execution order

Work should proceed in this order unless a newly discovered data-loss or
security issue preempts it:

| Order | ID | Priority | Outcome |
| ---: | --- | --- | --- |
| 1 | CI-01 | P0 | Restore a green, enforceable three-platform main branch |
| 2 | REL-01 | P0 | Prove original safety, catalog durability, and crash recovery on a real corpus |
| 3 | PERF-01 | P0 | Establish end-to-end Gallery, viewer, and Develop latency budgets |
| 4 | REL-02 | P0 | Prove installed packages, upgrades, backup, and restore on each supported host |
| 5 | IQ-00 | P0 | Gate preview/export/reopen and CPU/GPU rendering consistency |
| 6 | DISPLAY-01 | P1 | Add per-display ICC colour management without recipe mutation |
| 7 | LOCAL-01 | P1 | Ship a professional multi-instance local-adjustment and mask system |
| 8 | OFFLINE-01 | P1 | Ship Smart Preview / offline-original editing |
| 9 | CULL-01 | P1 | Ship high-throughput ingest review, burst grouping, and duplicate assistance |
| 10 | INGEST-01 | P1 | Add native PTP/MTP transport and resumable verified camera ingest |
| 11 | IQ-01 | P1 | Establish camera/profile quality and a real denoise evaluation path |
| 12 | EDITIN-01 | P1 | Complete the daily external-editor round trip in Studio |
| 13 | SPECIALIZE-01 | P2 | Choose one: HDR/Panorama or tethered-studio workflow |
| 14 | AI-00…AI-05 | P2 | Admit real AI only after packaging, evaluation, mask, and provenance gates |

# P0 — Repository and release blockers

## CI-01 — Restore green main and enforce the gate

**Status:** Active blocker (portable CLI float parser landed locally; three-platform
green and merge gate still open).

The pre-reorder `main` head `98026410f90bc99c116c3910abb5ccab63cb0976`
failed CI run `33843178641` (Static checks green). Triage from that run:

- **macOS (`mac_clang_debug`):** compile failure —
  floating `std::from_chars` for `--roi` in
  `Ravo/cli/src/application_catalog_args.cpp` (Apple libc++ deletes/omits the
  floating overload).
- **Windows / Linux:** build succeeded; product/test failures included GPU
  adapter tests returning `gpu_pipeline_failed` while `gpu_available` was true,
  many `CatalogServiceTest.*PersistsReopensAndExports*Pixels` exact-pixel
  mismatches, `XTransImportsAndPublishesAnEngineRenderedPreview`,
  `BackupRestorePreservesDerivedAndExternalEditorTrees`,
  `ravo_test_split_inventory` / inventory checker, and related catalog reopen
  pixel gates. These remain residual until fixed with regressions on those hosts
  (this tranche did not claim Windows/Linux green).

**Closed in this tranche (local `main`, mac verified):**

- one foundation owner `ravo::parse_ascii_double` (classic-locale, complete
  consumption, finite) used by CLI `parse_double_flag`, `--roi`, sharpen /
  watermark / frame export doubles; integer `from_chars` kept;
- unit + CLI coverage for boundary, malformed, locale (dot accepted / comma
  rejected under comma `LC_NUMERIC`), and duplicate `--exposure-ev` / `--roi`.

**Still open:**

- fix or correctly classify every Windows/Linux failure from run `33843178641`
  (and re-verify on current `main`) with regressions — do not skip/weaken tests;
- rerun static + macOS/Windows/Linux Debug on one pushed SHA;
- add a Release or RelWithDebInfo compile/smoke gate so Debug-only success cannot
  authorize a release;
- configure a repository ruleset or equivalent merge policy requiring the
  static and three platform jobs before ordinary changes reach `main`;
- forbid tags and release publication from a red commit.

**Acceptance gate:**

- static, macOS, Windows, and Linux jobs pass on the same `main` SHA;
- no required job is skipped, allowed to fail, or replaced by a narrower test;
- the portable parser has boundary, malformed, locale, duplicate-option, and
  cross-platform tests;
- the merge/release gate is visible in repository configuration and documented
  in the contributor path;
- the next ordinary commit cannot silently bypass the required checks.

**Risk:** a documentation-only green run does not close a code or test failure;
all required targets must execute on the same resolved source roots. Local mac
parser verification alone does not authorize a three-platform green claim.

## REL-01 — Real mixed-photo corpus, source safety, and recovery

**Status:** Active release evidence.

**Dependency:** an explicit read-only mixed RAW/raster tree in
`RAVO_PHOTO_CORPUS`. Generated catalogs, previews, recovery mirrors, backups,
derived files, and reports must live under a unique temporary root. Use a
Release build on the candidate host.

The corpus should include multiple Bayer and X-Trans cameras, DNG, JPEG, PNG,
TIFF, adjacent XMP, same-stem RAW+JPEG pairs, malformed/truncated files,
unsupported inputs, large files, Unicode paths, removable-storage paths, and
duplicate content. Private photos and reports remain outside the repository.

**Required scenarios:**

- Add, Copy, Move, filesystem-card ingest, second-copy verification, rename, and
  cancellation;
- ratings, labels, reject state, keywords, metadata, collections, virtual
  copies, stacks, history, snapshots, and batch edits;
- preview rebuild, missing-folder relink, external-editor derived assets, XMP
  status/import/export, backup/verify/restore, and catalog reopen;
- source disappearance, destination collision, disk-full simulation, process
  interruption at publication boundaries, and restart;
- upgrade from the latest published catalog/package to the current candidate.

Suggested evidence entry point:

```text
RAVO_PHOTO_CORPUS=/absolute/private/photos \
  build/<release-preset>/Ravo/tests/ravo_catalog_tests \
  --gtest_filter=CatalogServiceTest.PrivatePhotoManagementReleaseProbePreservesCorpus
```

**Acceptance gate:**

- every source file retains exact SHA-256, size, and modification time unless an
  explicit successful destructive command owns the mutation;
- unresolved conflicts publish no catalog row or destination file;
- interrupted work leaves either the previous complete state or the new complete
  state, never a half-published authority;
- catalog reopen, backup/restore, relink, virtual copies, stacks, history,
  metadata, XMP baselines, AI proposals, and derived provenance remain coherent;
- every unsupported input fails structurally without a lower-quality or
  incidental decoder fallback;
- reports record imported, skipped, unsupported, cancelled, and failed items
  without treating an unset corpus or skipped test as evidence.

**Risk:** one macOS corpus result is not evidence for Windows, Linux, another
storage device, or another GPU path.

## PERF-01 — Representative Gallery-to-viewer and Develop latency

**Status:** Active release evidence.

**Dependency:** `REL-01`, a Release build, declared storage type, stable power
state, and explicit cold/warm cache state.

Measure complete user-visible paths rather than isolated kernels:

- folder enumeration and import publication;
- first placeholder, first thumbnail, viewport-complete thumbnails, and exact
  1600-edge browse publication;
- adjacent-photo selection and revisit;
- Fit, Fill, and Actual Size / 1:1 ROI publication;
- first interactive Develop frame, latest frame after a rapid input burst, and
  settled persisted preview;
- active-query page/facet refresh at large-catalog scale;
- owned-image publication separately from native frame swap.

Record at least two warmups and eight samples per case with P50/P90/max, source
kind, file count, cache state, host, storage, worker count, peak owned bytes,
display refresh rate, GPU backend, and power state.

```text
RAVO_INTERACTIVE_PERF_CATALOG=/temporary/private-corpus/library.sqlite \
RAVO_INTERACTIVE_PERF_ASSET_ID=<imported-raw-asset-id> \
  build/<release-preset>/Ravo/tests/ravo_desktop_command_tests \
  --gtest_filter='StudioInteractivePreviewPerformanceProbe.*'
```

**Acceptance gate:**

- host-local budgets are frozen only after repeatable candidate measurements;
- the report distinguishes enumeration, catalog query, decode/demosaic,
  processing, encoding, image ownership, QML presentation, and frame swap;
- rapid intents never publish an older frame after a newer one;
- cancellation, memory bounds, profile state, and foreground-Develop priority
  remain correct under load;
- the same-corpus rerun with the frozen budgets passes.

### PERF-02 — Admit only measured optimizations

`PERF-02` starts only when `PERF-01` identifies a dominant owner. Evaluate one
candidate at a time:

- byte-bounded browse worker concurrency;
- profiled medium browse resources;
- byte-bounded adjacent-preview LRU;
- deferred optional metadata extraction;
- non-PNG rebuildable browse encoding;
- additional QRhi stages or transfer elimination.

Adopt a candidate only when same-corpus end-to-end P90 improves without a
foreground latency, deterministic publication, cancellation, memory,
source-safety, colour/profile, HDD-seek, package-size, or CPU-correctness
regression. Embedded JPEGs and placeholders remain presentation resources, not
RAW correctness references.

## REL-02 — Installed package, upgrade, and restore evidence

**Status:** P0 after CI-01 is green.

Use the existing `RavoPackage` path and validate the final DMG, Windows ZIP,
Linux AppImage, and Linux DEB after each artifact is copied away from the build
tree.

**Required installed smoke:**

1. launch Studio and the bundled CLI;
2. create a catalog and import one raster plus one supported RAW;
3. browse, rate, edit, save, reopen, and export JPEG plus TIFF or PNG;
4. rebuild a preview, create and verify a backup, restore to a new path, and
   reopen the restored catalog;
5. verify language catalogs, Qt image/platform plugins, SQLite, ICC handling,
   GPU availability reporting, and external-editor OS payload behavior;
6. open or upgrade a catalog created by the latest published release;
7. uninstall or remove the package without touching user catalogs or originals.

**Acceptance gate:**

- every platform artifact launches on a clean supported host without build-tree
  paths or undeclared runtime dependencies;
- package smoke succeeds with the same machine contracts as the source build;
- an upgrade failure is explicit and leaves the old catalog recoverable;
- backup/restore uses complete staged verification and no original is packaged;
- package version, release notes, README status, supported formats, and actual
  artifacts agree;
- signing/notarization status is recorded accurately and never implied when
  absent.

**Risk:** successful archive creation is not installed-product evidence.

## IQ-00 — Rendering consistency release gate

**Status:** P0 after representative corpus access exists.

The current CPU path remains the correctness reference. GPU preview may improve
latency but cannot silently change the recipe, colour pipeline, supported-input
boundary, or export result.

**Required matrix:**

- raster and supported Bayer/X-Trans RAW;
- default and edited white balance, exposure, highlight recovery, curves, colour
  grading, lens/perspective/crop/canvas, sharpening, denoise, masks, frame, and
  watermark;
- built-in and file ICC input/output profiles, rendering intents, proof state,
  and metadata privacy modes;
- interactive preview, settled preview, close/reopen, CLI probe/render, and
  JPEG/PNG/TIFF export;
- CPU-only, supported GPU path, GPU-unavailable, resource-exhausted, and
  cancelled execution.

**Acceptance gate:**

- preview, settled save, reopen, and export use one canonical recipe and
  documented tolerance;
- CPU/GPU comparisons use per-operation and composed colour/error thresholds
  derived from an approved corpus, not screenshot judgment;
- unsupported GPU stages remain explicit and never select an unreported
  lower-quality algorithm;
- ICC identity and embedded profile bytes match the declared output;
- 1:1 ROI and full export agree for the overlapping source region after the same
  accepted geometry;
- regressions retain inputs, recipe JSON, backend, profile identity, hashes, and
  error metrics.

# P1 — Next professional daily workflows

P1 work starts only after CI-01 is closed. A P1 item may be researched earlier,
but implementation that expands product state must not displace active P0
evidence.

## DISPLAY-01 — Per-display ICC colour management

**Status:** P1; ADR-0144 accepted. First Ready tranche on `main` (service /
presenter contract + tests + CLI status). Residual OS/Studio/GPU work remains.

Ravo already owns input/output ICC transforms and soft-proof state. On-screen
monitor conversion is a separate presentation contract: it must not mutate the
canonical recipe, settled preview authority, export profile, history, or
catalog revision.

**Accepted (ADR-0144):**

- Monitor ICC is presentation-only; never mutates recipe JSON, history, catalog
  revision, settled preview authority, or export profile.
- C++ display-presentation owner applies preview→monitor after soft-proof;
  QML only presents pixels.
- First Ready: macOS CoreGraphics discovery + change lifecycle; explicit
  **sRGB** fallback when missing/corrupt (`source=fallback_srgb`); injectable
  ICC path; synthetic matrix/LUT CPU reference paths; CLI
  `display-profile status`.
- Soft-proof remains recipe-owned and inspectable; display transform is
  on-screen only.
- Window/screen token refresh does not change recipe.
- Headless/tests inject profiles; never silent assumed transform without
  machine-visible state.

**Residual:**

- Windows/Linux monitor discovery and profile-change lifecycle;
- Studio window-move wiring to refresh presentation across Gallery/Loupe/
  Develop/Before/After/comparison/magnifier/scopes with explicit pixel-kind
  declarations;
- GPU presentation-path parity with CPU synthetic matrix/LUT references;
- SDR versus HDR display policy and first-version HDR boundary;
- validation corpus comparing owned transform output (not screenshots) across
  hosts.

**Acceptance gate:**

- moving Studio between displays changes presentation only and never changes
  recipe JSON, history, catalog revision, or exported bytes;
- synthetic matrix and LUT monitor profiles produce repeatable reference output
  on CPU and any supported GPU presentation path;
- soft-proof and gamut-warning behavior remains defined and inspectable;
- missing, corrupt, changed, or unsupported profiles are visible machine states;
- Gallery, Loupe, Develop, Before/After, comparison, magnifier, and scopes all
  declare whether they consume scene/output pixels or display-transformed
  pixels;
- validation compares owned transform output, not application screenshots.

## LOCAL-01 — Multi-instance local adjustments and professional masks

**Status:** P1; ADR-0145 accepted. Ready on `main`: multi-instance Exposure and
Color Balance RGB recipe/Develop serialization + CLI `recipe inspect` with
gradient/radial/parametric masks. Studio instance chrome remains residual.

Existing canonical masks and single-mask everyday consumers are foundations.
The remaining product gap is a coherent local-adjustment system rather than more
one-off masked fields.

**Accepted (ADR-0145):**

- Ordered multi-instance Develop ops; first consumers Exposure + Color Balance
  RGB (both Ready for Develop/CLI; Studio chrome residual).
- Instance `instance_id`, optional `name`, `enabled`, `bypass`, reorder via
  recipe order, serialize on `OperationInstance` /
  `exposure_instances` / `color_balance_rgb_instances`.
- C++-owned mask leaves reuse ADR-0043/0116 (brush/path/linear/radial +
  luminance/colour parametric); group Add/Subtract/Intersect/Invert + opacity.
- Legacy/CRS unsupported multi-instance forms stay fail-closed (no approximation).
- Empty `exposure_instances` / `color_balance_rgb_instances` preserve legacy
  single-field Develop behaviour.

**Residual:**

- Studio instance list chrome (name/bypass/reorder/duplicate/delete) beyond thin
  hooks;
- history/undo/style/selective copy completeness for instance vectors;
- GPU adapter boundary for masked multi-instance evaluation if needed.

**Acceptance gate:**

- no mask pixels or durable geometry are owned by QML;
- preview, export, reopen, history, undo, style apply, and copy/paste are equal;
- mask geometry remains correct through every accepted transform;
- unsupported legacy blend/mask shapes reject without approximation;
- stale revision, cancellation, malformed graph, excessive points, allocation
  failure, and partial batch behavior are tested;
- unmasked identity and existing single-instance recipes retain their exact
  behavior through a versioned migration.

**Blocks:** real AI semantic selections and replayable AI retouch proposals.

## OFFLINE-01 — Smart Preview and offline-original editing

**Status:** P1 first Ready landed (ADR-0146). Residuals remain.

**Closed in this tranche (local `main`):**

- ADR-0146 accepts an explicit **offline-edit proxy** class under
  `{catalog}.ravo/offline-edit-proxies/` (distinct from ADR-0141 browse-only
  Smart Preview).
- Service/CLI: `offline-proxy-create|list|verify|status|reconnect` with
  source hash + recipe cache key + max edge + `srgb` profile; machine states
  `original|proxy|placeholder|missing`.
- Offline Develop recipe apply while original is stashed; export fail-closed
  with `proxy_export_forbidden` (v1: no proxy export); reconnect verifies
  source SHA-256 then returns to original-backed export.

**Still open:**

- delete/pin/quota/eviction and background generation policy;
- Studio status/storage chrome;
- Develop/loupe render path consuming the offline-edit proxy when original is
  offline (recipe writes already work);
- backup/restore packaging of `offline-edit-proxies/` (ADR-0136 extension);
- packaged Smart Preview / DNG converter enablement (ADR-0141 residual).

**Acceptance gate:** unchanged for remaining residuals; first Ready proves
create/verify, offline develop apply, export rejection, and reconnect hash
check with originals byte-identical.

## CULL-01 — High-throughput review, burst grouping, and duplicate assistance

**Status:** P1 after `PERF-01` establishes baseline measurements.

Ship deterministic workflow improvements before model-dependent ranking.

**First bounded tranche:**

- exact duplicate identity by content hash with explicit same-file, same-bytes,
  and distinct-version outcomes;
- bounded perceptual fingerprinting for near-duplicate suggestions;
- capture-time/camera/sequence burst grouping and optional auto-stack proposal;
- keyboard-first Pick/Reject/rating/colour-label flow with auto-advance;
- fast selected-photo 1:1 focus inspection and previous/next synchronization;
- import-session and collection-level filtering for unreviewed, picked,
  rejected, duplicate, and burst groups;
- transactional accept/dismiss actions; no automatic delete or reject.

**Acceptance gate:**

- suggestions remain separate from catalog facts until explicit acceptance;
- exact duplicates do not conflate virtual copies, RAW+JPEG companions, derived
  assets, or files with different bytes;
- 100,000-photo query, grouping, and paging remain bounded;
- source disappearance, stale revision, cancellation, and partial batch state
  are explicit;
- the user can undo accepted stack/review mutations where the existing catalog
  contract permits;
- baseline review latency does not regress.

**Blocks:** AI-04 metadata/culling/similarity quality work.

## INGEST-01 — Native PTP/MTP and resumable verified camera ingest

**Status:** P1, dependent on the existing ADR-0125 ingest URI/lifecycle.

The filesystem-card/DCIM adapter remains the first transport. Add one native
device transport without creating a second import planner.

**Decision required:**

- PTP/MTP session enumeration, device identity, object identity, disconnect,
  reconnect, permission, timeout, and cancellation semantics;
- per-object resume checkpoints without treating an incomplete copy as
  published;
- idempotent repeated ingest and already-imported detection;
- read-only source policy, destination/second-copy preflight, and device-safe
  deletion policy;
- package dependencies and three-platform support matrix.

**First bounded tranche:**

- enumerate one selected device/session;
- Copy-only ingest through the existing rename/organization/second-copy planner;
- resume verified incomplete batches after reconnect;
- structured per-item imported/skipped/unsupported/cancelled/failed report;
- Studio source selector and progress surface backed by the same service.

**Acceptance gate:**

- no Move or camera deletion occurs in the first tranche;
- every published primary and second copy verifies against the device object;
- disconnect or cancellation leaves reusable transport/session state;
- destination conflicts are resolved before publication;
- repeated ingest does not create accidental duplicate catalog rows;
- absent platform support reports `unsupported` rather than pretending a
  filesystem mount is a native session.

## IQ-01 — Camera/profile quality and denoise evaluation

**Status:** P1 after IQ-00 establishes the regression harness.

**Required work:**

- define a licensed, redistributable quality corpus plus a private-camera
  extension set covering skin, saturated fabrics, foliage, tungsten/LED,
  underexposure, clipped highlights, high ISO, moiré, hot pixels, and lens
  extremes;
- record camera model, firmware, lens, illuminant, exposure, expected profile,
  and legal provenance;
- measure colour accuracy, hue stability, highlight recovery, noise/detail,
  edge halos, false colour, and scaling behavior with repeatable metrics plus
  blinded human review;
- define camera input/profile update and lens database validation workflows;
- separate deterministic algorithm regression from learned-model quality;
- evaluate a first real denoise provider only after runtime, weight, licence,
  package, memory, cancellation, and fallback decisions are accepted.

**Acceptance gate:**

- no camera support claim is based only on file decode;
- new profiles and model versions have before/after evidence and known-failure
  notes;
- quality tests cover preview and export at native resolution;
- model absence or unsupported hardware has explicit behavior and never changes
  the deterministic default silently;
- redistributed data, profiles, runtimes, and weights have licence and notice
  evidence.

## EDITIN-01 — Complete the Studio external-editor round trip

**Status:** P1, building on ADR-0122, ADR-0136, and ADR-0139.

**First bounded tranche:**

- a Studio `Edit in…` dialog with destination application, TIFF baseline,
  bit depth, output profile, optional resize, naming, and auto-stack choice;
- explicit create-working-copy/open/register states with visible provenance;
- an explicit `Check for returned file` or user-selected result path rather than
  an implicit watch-folder authority;
- refresh of derived thumbnail/preview after successful registration;
- conflict, stale source revision, missing application, cancelled open,
  unchanged result, and duplicate registration handling;
- reopen, backup/restore, catalog relocation, unstack, and removal behavior.

PSD/PSB, proprietary application scripting, and background watch-folder import
remain outside the first tranche and require separate dependency/product
decisions.

**Acceptance gate:**

- the original remains byte-identical;
- the exported working copy has the requested dimensions, sample type, profile,
  and metadata privacy;
- registration publishes one immutable derived asset and complete provenance or
  publishes nothing;
- a stack conflict retains the derived asset and reports the exact unresolved
  relationship;
- Studio and CLI expose the same machine contract and failure classes.

# P2 — Important extensions after the professional baseline

## FORMAT-01 — Owned HEIC/HEIF decode

**Status:** Blocked by the ADR-0123 dependency/licence/package gate.

Remaining scope includes pixels, orientation, colour/profile, alpha, HDR
transfer/metadata policy, multi-image selection, malformed-container limits, and
three-platform packaging. Recognition continues to fail closed until an owned
decoder ships.

**Acceptance gate:** one declared decoder owner produces bounded, colour-managed
pixels on all supported hosts; unsupported brands/features remain structural;
no incidental platform ImageIO or Qt plugin becomes a hidden product contract.

## SPECIALIZE-01 — Choose one vertical specialization

Do not start both tracks concurrently. Select one based on the first external
photographer cohort and record the decision in a dated ADR.

### Track A — HDR/Panorama

Define source grouping, alignment, projection, ghost handling, exposure merge,
working colour space, memory/cancellation, crop, provenance, recipe relationship,
rebuild, and derived-asset publication. The merged result must remain linked to
its sources and never overwrite them.

### Track B — Tethered studio

Define camera support, USB/network lifecycle, reconnect, live view, capture
destination, naming/metadata/style application, client display, session
organization, privacy, and package dependencies. This is independent Ravo work,
not a port of the removed tether module.

**Acceptance gate:** the selected track has one complete service/CLI contract,
one Studio workflow, real hardware/corpus evidence, cancellation/recovery, and
source-preservation tests before the other track enters implementation.

## META-01 — Remaining professional metadata depth

**Status:** P2 unless a journalism, sports, agency, or archive cohort makes it a
P1 requirement.

**Residual from ADR-0140 Studio chrome:** headline/credit/source/instructions/usage_terms/job_id (plus description) are already exposed in Studio PhotoInfo via selection metadata patches on `main`.

**Residual from ADR-0143:** CRS `ProcessVersion` matrix is accepted on `main`.
`catalog xmp-status` / import report `crs_version_class` /
`crs_process_version`; unsupported Process Versions fail closed with
`unsupported_crs_process_version` (no silent drop). Remaining META-01 work is
IPTC Extension depth beyond ADR-0140, not ProcessVersion admission.

Define the remaining IPTC Extension/contact/scene/subject-code subset, controlled
vocabularies, multi-select semantics, facets, export privacy, XMP conflict
fingerprints, and capture-refresh authority. Do not add empty columns or panes
before the contract and target users are known.

## CONVERT-01 — Real foreign-catalog readers

**Status:** P2.

Potential readers for Lightroom `.lrcat` and Capture One catalogs/sessions need
a dependency/licence/package decision, a **vendor catalog** supported-version
matrix (distinct from ADR-0143 CRS ProcessVersion admission), read-only
parsing, Copy-mode destination policy, persistent resume/report state, and a
Studio surface. Never open or migrate a vendor catalog in place. Unsupported
fields must be reported per item rather than silently dropped.

## DELIVERY-01 — Advanced export delivery

Soft-proof-as-export, SVG/logo overlays, arbitrary fonts, decorative templates,
and publishing services remain outside the accepted ADR-0129 tranche. Each
requires a new product/dependency decision and must remain ExportOptions or a
derived delivery job rather than mutating Develop recipes.

# P2 — Real AI admission

ADR-0121 and the deterministic AI-01/AI-02/AI-03 stubs establish proposal
lifecycle and failure contracts. They are test fixtures, not claims of model
quality. No additional stub provider or placeholder Studio surface is a
priority.

## AI-00 — Runtime, privacy, packaging, and evaluation gate

**Status:** Required before any non-stub provider ships.

Record the concrete provider/runtime, model or weight identity, source, licence,
GPL compatibility, notices, package/download size, update channel, cache root,
credential ownership, local/remote payload, retention, logging, telemetry,
training-use policy, concurrency, memory, timeout, retry, cancellation, and
offline behavior.

Establish a licensed evaluation corpus and blinded human-review protocol across
RAW/raster inputs, cameras, lighting, genres, skin tones, failure cases, and
provider versions.

**Acceptance gate:** privacy/threat review, dependency evidence, reproducible
model identity, bounded resources, no-model behavior, and quality thresholds are
all testable; originals never need to be writable.

## AI-01 — Real global edit proposals

**Blocked by:** AI-00 and IQ-01.

Admit white-balance, exposure, tone, colour, crop/straighten, and
reference-grade proposals only as ordinary validated recipe-field diffs. Show
the exact diff and preview before apply. Reject/cancel changes nothing; apply
creates ordinary history/undo. Model quality must pass the approved corpus and
human review.

## AI-02 — Real semantic selections

**Blocked by:** AI-00 and LOCAL-01.

Subject, sky, background, person/skin, clothing, and object results must be
bounded canonical masks in the accepted coordinate system. Uncertain and empty
masks remain visible. No provider-specific bitmap may become a hidden second
mask authority.

## AI-03 — Real shoot-consistency assistance

**Blocked by:** AI-00, AI-01, and the accepted batch/culling evaluation set.

Every destination remains explicitly selected and receives an independent
proposal/history entry. Partial completion and cancellation follow the existing
batch contract. Per-image exposure and white-balance exceptions must remain
inspectable.

## AI-04 — Metadata, culling, and similarity suggestions

**Blocked by:** AI-00 and CULL-01.

**Residual from ADR-0142:** stub keyword/caption/focus/duplicate suggestions ship
under `{catalog}.ai_suggestions/` via provider `ravo.local.stub` /
`deterministic-suggestion-v1`, with explicit accept/reject/cancel CLI
(`ai-suggest`, `ai-suggestion(s)`, `ai-suggestion-accept|reject|cancel`).
Accept maps keyword→`set_tags` merge and caption→writable description/headline;
focus/duplicate acknowledge only and never delete peers. Stub ship satisfies
contract/lifecycle only.

Keyword/caption, focus/exposure, duplicate, and near-duplicate suggestions remain
separate from catalog facts. They never auto-reject, delete, publish, or write
identity-sensitive metadata. Acceptance is transactional or reports exact
partial state. Real model quality, batch accept beyond single-id, people/face
policy, and Studio surfaces remain unfinished.

## AI-05 — Retouch and generated-pixel results

**Blocked by:** AI-00, LOCAL-01, and completion of immutable generated-derived
asset publication.

Replayable dust/blemish work may propose canonical Retouch regions.
Non-replayable generated pixels must publish a new immutable derived asset with
source revision, provider/model/version or weight hash, settings, output hash,
privacy provenance, backup/restore policy, and visible generated status.
Failure or cancellation publishes no partial asset; reopen/export cannot require
the original provider to be online.

# P3 — Deferred product areas

The following remain decision-only and must not displace the queue above:

- print layout and printer/profile ownership;
- map, geocoding, and location-service privacy;
- slideshow and presentation delivery;
- remote publishing and cloud collaboration;
- shared/multi-writer catalogs;
- face identity and biometric privacy;
- broad natural-language library search;
- proprietary editor automation;
- arbitrary legacy IOP, GTK, Lua, OpenCL, or foreign-catalog compatibility.

Each needs an independent Ravo contract, target cohort, privacy/security review,
offline/failure behavior, package plan, and measurable acceptance gate.

# Cross-cutting acceptance

Any new Ready tranche adapts this minimum set rather than weakening it:

```text
python3 configs/source_roots.py verify
python3 Ravo/tools/freeze_legacy_manifest.py --check
python3 Ravo/tools/check_ravo_dependency_boundary.py
cmake --preset mac_clang_debug -DBUILD_TESTING=ON
cmake --build --preset mac_clang_debug --target \
  ravo_catalog_tests ravo_desktop_command_tests ravo_contract_tests ravo_studio
ctest --test-dir build/mac_clang_debug --output-on-failure
```

The matching Linux and Windows CI jobs must run for cross-platform changes. A
release candidate additionally requires the Release corpus/performance probes
and installed-package smoke described above.

For persisted state, add create/migrate/reopen/backup/restore tests. For
asynchronous work, add stale revision, conflict, cancellation, source
disappearance, resource exhaustion, disk-full, restart, and partial-publication
tests. For image behavior, add CPU reference, GPU comparison where supported,
ICC/profile, preview/export equality, and source-immutability evidence. For new
dependencies, models, profiles, or corpora, add pin, licence, package, notice,
and reproducibility checks.

Studio work must preserve keyboard-only operation, focus order, localization,
high-DPI behavior, and explicit unavailable reasons. When a P1 workflow touches
registered oversized QML or C++ owners, split the workflow owner and reduce the
registered debt; do not grow `Main.qml` or stop product work for a detached UI
rewrite.

A skipped test is not a pass. An unsupported state must be explicit. A fallback
must be named, observable, quality-bounded, and accepted by contract; otherwise
fail closed.
