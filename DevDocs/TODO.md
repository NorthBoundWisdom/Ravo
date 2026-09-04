# Ravo product execution TODO

> **Status:** ordered residual queue
>
> **Updated:** 2026-09-05
>
> **Review basis:** `main` through LOCAL-01 C2, EDITIN-01 C2 (+ macOS installed-
> package TIFF matrix residual closed on report `20260905_024633`; Win/Linux
> package matrix still open), CULL-01 keyboard C2 + analysis decode-path residual,
> OFFLINE-01 C2, DISPLAY-01 C2, INGEST-01 C2 Studio filesystem-card + ptp-stub
> ingest (native adapters residual), IQ-01 C2 fixture evaluation, and IQ-00 macOS
> contract expansions (CPU gold + GPU live residual documented; Win/Linux not
> claimed). REL-01 has macOS desensitized evidence on `07dbb9ef` (report
> `20260905_020736`), packaged-CLI Add/Copy + XMP deepen on `e1a68eeb` (report
> `20260905_023651`), plus contract/packaged deepen for disconnect/reconnect,
> ENOSPC injection harness, Move, missing-volume fail-closed on report
> `20260905_024633` (PNG/TIFF-in-corpus, rating-via-XMP, X-Trans, Win/Linux still
> residual; not C3). REL-02 has a macOS Release DMG package-smoke tranche on
> `e1a68eeb` (report `20260905_023651`); Windows ZIP / Linux AppImage+DEB and full
> REL matrices remain open. The next free ADR number is **0157**, but new product
> ADRs are frozen by the work-in-progress rule below.

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

Until GPU/PERF follow-ons and REL-01 corpus reopen evidence are closed (LOCAL-01 and EDITIN-01 Studio C2 evidence are on `main`):

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

LOCAL-01 and EDITIN-01 Studio C2 evidence are on `main`. Recent work on
CULL-01 keyboard review, PERF-01 instrumentation, and IQ-00 CPU-gold policy
moves in the intended direction. DISPLAY-01 is C2 with GPU/Linux discovery
residuals; OFFLINE-01 C2 Studio proxy manage/consume landed on `main`;
INGEST-01 is C2 for Studio filesystem-card + ptp-stub Copy ingest with fail-
closed native probe (packaged ImageCapture/WinRT/libmtp residual is C3). AI
proposal chrome and the tethered-studio probe do not count as product progress.

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
| 1 | LOCAL-01 | P1 | C2 Studio multi-instance local adjustments/masks; GPU residual after PERF-01 |
| 2 | EDITIN-01 | P1 | C2 Studio external-editor round trip; Win/Linux package TIFF residual |
| 3 | CULL-01 | P1 | Keyboard C2; analysis C1+ decode-path; corpora/index residual |
| 4 | OFFLINE-01 | P1 | C2 Studio offline-edit proxies; C3 quota/corpus residual |
| 5 | DISPLAY-01 | P1 | Finish per-display ICC presentation on supported hosts and views |
| 6 | INGEST-01 | P1 | C2 Studio filesystem-card + stub ingest; C3 packaged native adapters/hardware |
| 7 | IQ-01 | P1 | C2 fixture evaluation workflow; C3 licensed corpus/human review/learned denoise |

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

**Status:** Closed for LOCAL/CULL/OFFLINE/aHash reviewed defects and the four
follow-on residuals (instance-id high-water + recipe save revision binding;
owned-mask GC; export-usable catalog identity; offline proxy publish inject).
Remaining evidence that does **not** block LOCAL-01 C2 chrome start:
reopen/backup/restore corpus scenarios under REL-01.

### LOCAL-01 instance and mask mutation safety

- [x] `delete_exposure_instance` /
  `delete_color_balance_rgb_instance` validate `instance_id` before sole-
  instance collapse; stale/wrong IDs reject.
- [x] Sole disabled/bypassed collapse resets legacy fields to documented
  identity (no silent re-activation). Studio disables Delete when only one
  instance remains.
- [x] Instance IDs never reuse deleted numeric suffixes (`*_instance_id_high_water`);
  recipe/develop saves bind `RecipeSaveOptions.expected_revision` (Studio passes
  observed catalog revision). Stale id after delete cannot address a new instance.
- [x] `clone_mask_subgraph` stages the full clone and appends atomically;
  cycle/missing child leave `DevelopParams.masks` unchanged.
- [x] Deleting an instance removes only its exclusively owned Studio mask
  subgraph; shared/external masks are retained. No owned-mask orphan accumulation.

### CULL-01 catalog mutation atomicity

- [x] `commit_review`: review state + revision in one repository transaction
  (`apply_cull_review`, `set_picked`).
- [x] Cancellation checked before publication; post-publication returns
  committed result.
- [x] Recovery-sidecar failure reports `catalog_committed=true` +
  `recovery_retryable` (does not pretend the review never happened).
- [x] Auto-advance order computed before mutation.
- [x] Failure-injection tests: revision bump, commit abort, cancel before
  publish, stale revision (existing).

### OFFLINE-01 manifest and publication safety

- [x] Bounded `from_chars` parsing (no throwing `stoll`/`stoull`); schema,
  asset id, hash, dims, profile, path-under-support-root validated.
- [x] Corrupt manifests surfaced in `list_offline_edit_proxies().corrupt`.
- [x] Staging tree + atomic replace; failure retains prior good proxy.
- [x] Original export-usable requires catalog identity match (size/mtime/
  fingerprint), not file presence alone; export refuses `source_identity_mismatch`.
  Explicit reconnect remains the verified clear path.
- [x] Publish-boundary failure injection (`set_before_offline_proxy_publish`)
  retains the prior verified proxy.
- [x] `pixel_provenance=recipe_baked_srgb8`; Develop applies identity on proxy
  consume (ADR-0146 COR-01 note). No double-grade.

### aHash near-dup bounds (ADR-0149 / COR-01)

- [x] Hard `max_assets` upper bound (default 4096); fail-closed
  `near_dup_asset_bound_exceeded`.
- [x] Non-authoritative heuristic documented; pairwise max-edge groups (no
  transitive union beyond Hamming).

**Acceptance gate (tranche):**

- wrong IDs, cycles, missing children, corrupt manifests, path escape,
  cancellation before publish, injected review failures, and near-dup bound
  have regression tests;
- every listed error path either changes nothing or returns exact committed
  state for the closed items above;
- residual reopen/backup/restore corpus evidence remains under REL-01.

## REL-01 — Real mixed-photo corpus, source safety, and recovery

**Status:** macOS host C2-style evidence recorded for candidate
`07dbb9ef4aad7e23f891fdb1adf6ea0b9d966ff9` — report id
`20260905_020736` under `~/Documents/RavoEvidence/reports/` (hashed corpus
manifest, PrivatePhotoManagementReleaseProbe PASS with source SHA-256/size/mtime
preservation, backup/restore/reopen smoke PASS, scrubbed logs). Follow-on
packaged-CLI deepen on candidate `e1a68eeb0ebc4f8aafab45287549f5720e8c2580`
(report `20260905_023651`): Add vs Copy ingest keeps source SHA-256/size/mtime
unchanged and copy destination byte-identical; XMP sidecar export/import on a
writable working copy restores `dc:title`/`dc:creator` with `catalog-newer` →
sidecar resolve → `identical` (rating-via-XMP still residual; read-only corpus
dir fail-closes adjacent `.xmp` write). Further host-local deepen on report
`20260905_024633` (same package binary + Debug contracts): filesystem-card
disconnect partial + PTP stub reconnect, folder relink, offline missing-volume
export fail-closed (`original_missing`) with restore-to-original, catalog import
Move relocates with destination SHA match, filesystem-card ingest Move rejects
(`ingest_move_unsupported`), and ENOSPC/`disk_full` markers at encoded-
publication and original-copy publication boundaries via existing injection
harnesses. Private corpus still has **0 PNG / 0 TIFF** (not invented). A host
path alone is still not evidence; Windows/Linux and remaining required scenarios
(X-Trans, PNG/TIFF-in-corpus, rating-via-XMP, live host disk-full, upgrade,
large-library) remain open. Do not claim C3.

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

**Status:** Measurement harness exists. Host-local Gallery→viewer→Develop
samples for SHA `07dbb9ef4aad…` are filed with REL-01 report
`20260905_020736` (`PERF-01-host-local-budget-notes.json`) — **observation-only
machine-local Debug notes**, not admitted product budgets and **not** a PERF-02
admit. Documented observation ceilings from that host-local note (same SHA,
`mac_clang_debug`, warmups=2 / n=8 where recorded): warm gallery→loupe P90 ≤50 ms,
adjacent revisit P90 ≤50 ms, loupe→Develop first-frame P90 ≤30 ms, warm preview
P90 ≤5 ms, page P90 ≤2000 µs. Cold gallery→loupe (~6.7 s) and cold settled
preview (~3.8 s P90) remain separately characterized. Large-library / Release
same-corpus freeze and cross-host evidence remain open. Do not start PERF-02
optimization work yet.

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

**Status:** ADR-0151 CPU-gold policy plus macOS Debug/Release **contract**
expansions on `main`: persist preview ↔ export RGB8 bit-exact + ICC identity,
packed ROI crop reopen/export equality, interactive GPU packed-delta residual
(with persist remaining CPU gold), and `catalog probe` `iq_consistency` policy
JSON documenting host_scope=`macos_debug_release_contract`,
`win_linux_hosts_claimed=false`, and GPU live residual (interactive develop +
RAW viewport ROI). Catalog probe `Iq00RawRoiLiveVersusCpuExportDocumentsResidual`
records that settled/reopen stay CPU gold while ROI may report GPU. **Not** a
full corpus / Win/Linux / proof-monitor / multi-instance matrix closure.

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

**Status:** macOS Release DMG tranche recorded (honest C2-style, not C3) for
candidate `e1a68eeb0ebc4f8aafab45287549f5720e8c2580` — report id
`20260905_023651` under `~/Documents/RavoEvidence/reports/`. Sanctioned
`RavoPackage` produced `RavoStudio-0.9.2-arm64-macOS.dmg`; artifact was copied
away from the build tree before smoke. Installed CLI 18/18: catalog
create/import (JPEG+ARW+DNG)/preview/probe/rate/snapshot/develop/export/
reopen/backup/verify/restore + `display-profile status` (system ICC) and probe
`gpu_backend=metal`. otool/`@loader_path` deps show no `…/Ravo2/build` leakage;
QSQLITE present, Mimer absent; Studio GUI click smoke skipped (no Computer Use).
**Still open:** Windows ZIP; Linux AppImage + DEB; Studio interactive /
localization / upgrade-failure host matrix; notarization/Developer ID claims.
Do not claim full REL-02 or C3.

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

**Maturity:** C2 on `main` for Studio multi-instance workflow evidence. Snapshot /
history reopen / backup-restore smoke, failure-injection depth (stale revision on
instance mutate, wrong `instance_id` after recreate, cancel mid structural batch
apply), and live Studio preview-scale / 1:1 ROI mask-authoring contracts are
closed via `Local01*` / catalog / presenter / QML-contract tests. Residual: GPU
mask evaluation only after PERF-01 CPU equality; any leftover Main.qml extraction
must shrink chrome, not grow it. Full mixed-corpus reopen/backup remains REL-01.

**Remaining work:**

- COR-01 LOCAL residuals closed (id high-water, owned-mask GC, revision-bound saves);
- keep bounded instance counts / last-instance Studio Delete gate as documented;
- [done] Exposure (+ CBR) instance group Add/Union/Difference/Intersect + Invert
  (root/child) + opacity/feather authoring round-trips with geometry fields
  (`Local01GroupComposition*`, `Local01InvertIntersect*`);
- [done] selected-instance overlay attachment slot (`exposure_mask_id` /
  `color_balance_rgb_mask_id`), feather, and brush flow/density/hardness through
  the C++-owned mask DAG (`Local01OverlayFeatherBrushFlow*`); MaskEditor chrome
  already exposes Invert/Intersect/overlay/feather/brush without Main.qml growth;
- [done] coordinate legs: orientation, lens, Canvas, 1:1 crop aspect, export
  recipe survival (`Local01GeometryLegsMaskCoordSurvival`); live Studio
  preview-scale / 1:1 ROI authoring path
  (`Local01PreviewScaleAndOneToOneRoiMaskAuthoring`,
  `Local01MaskPlaceUsesPhotoPlaneNotInspectRoi`) — Fit/Fill/1:1 normalize to
  `photoPlane`; `map_mask_place_preview` maps through 1:1 crop onto the selected
  instance; inspect ROI is display-only;
- [done] snapshot / history reopen / catalog reopen + backup/restore smoke for
  multi-instance Exposure/CBR + masks
  (`Local01MultiInstanceSnapshotHistoryReopenAndStaleRevision`,
  `Local01MultiInstanceBackupRestoreSmoke`);
- [done] failure-injection depth: stale revision on instance mutate, wrong
  `instance_id` after recreate (rename/bypass/enable/delete), cancel mid
  structural multi-instance batch apply (`Local01WrongInstanceIdAfterRecreate*`,
  stale revision in snapshot test, `Local01CancelMidStructuralMultiInstanceApply`);
- retain existing history/undo/style/copy/batch/virtual-copy contracts;
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

**Maturity:** C2 on Studio workflow (prepare/return/auto-stack/reopen/abandon,
backup/restore relocation of working-copy sessions, TIFF sRGB uint8/uint16
profile/bit-depth/dimensions/naming/conflict service coverage, and clear
stale/missing/source-conflict Studio states). macOS installed-package TIFF
matrix closed on report `20260905_024633` using the REL-02 copy-away DMG CLI:
external-editor working copies + catalog export for sRGB uint8/uint16 with
matching bit-depth, max-edge dimensions, `working.tif` naming under
`working-copies/`, session `profile=srgb`, and ICC present (source SHA
unchanged). Residual toward C3: Windows/Linux installed-package TIFF matrix.

**Remaining work:**

- validate TIFF profile/bit-depth/dimensions/naming equality on installed
  Windows/Linux packages (macOS package matrix evidenced on `20260905_024633`;
  service/CLI coverage remains present);
- keep watch-folder auto-import and proprietary scripting out of scope.

**Acceptance gate:** originals remain byte-identical; unchanged, missing,
source-mutated, stale, and conflicting returns are visible; publication is
atomic; reopen/backup/restore/export work without CLI assembly.

## CULL-01 — Transactional keyboard review and evaluated assistance

**Maturity:** Keyboard review **C2** on `main` (Pick/Reject/Unflag/rating/colour,
auto-advance, previous-review undo, filters, Survey compare under paging /
collapsed stacks / restart; COR-01 atomicity). Analysis **C1+** toward C2:
bounded non-authoritative aHash with exact-byte vs heuristic `group_kind` in
reports / Studio suggestion chips (Exact byte duplicate vs Near duplicate
(heuristic)), one accepted RAW/raster fingerprint decode path, fail-closed
`max_assets`, incremental fingerprint cache (`{catalog}.cull/`) with
source-identity invalidation, bounded entries, dismiss-across-reopen, cancel,
and throttle. Not full analysis C2 (precision/recall corpora; pairwise O(N²)
still capped rather than indexed).

**Remaining work:**

- [done] COR-01 review atomicity;
- [done] verify Pick/Reject/Unflag/rating/colour, auto-advance, previous-review
  undo, filters, Survey comparison under paging, collapsed stacks, and restart
  (`Cull01ReviewUnderPaging*`, `Cull01UnflagColour*`);
- [done] first Ready fingerprint/proposal persistence: incremental aHash cache +
  dismiss under `{catalog}.cull/` with identity invalidation, bound, cancel,
  throttle (`Cull01FingerprintCache*`);
- [done] distinguish exact-byte / same-file vs heuristic aHash `group_kind` in
  service + CLI reports (Studio chips already separate exact vs near vs burst);
- [done] enforce aHash non-authoritative + fail-closed above `max_assets`;
- [done] never auto-delete/auto-reject; auto-stack only with `user_initiated`;
- [done] produce fingerprints through one accepted image resource path for
  supported RAW/raster orientation and colour
  (`decode_cull_fingerprint_raster` → `decode_import_candidate_thumbnail`;
  report `fingerprint_decode_path`; Studio chips label Exact byte vs Near
  (heuristic); dismiss persists across catalog reopen);
- replace pairwise O(N²) with an indexed design beyond the hard asset bound
  (PERF-01);
- measure precision/recall and false-positive cost on approved corpora.

**Acceptance gate:** the keyboard loop needs no pointer; a committed mutation
cannot be reported as failed; review remains responsive during analysis;
heuristic thresholds have metrics and known failures; cancel/restart/missing
source/corrupt decode/stale fingerprint/resource exhaustion are deterministic.
Keyboard gate: met on `main` via service/Studio contract tests. Analysis gate:
partial (deterministic cancel/restart/corrupt/stale/bound); metrics open.

## OFFLINE-01 — Offline-original editing

**Maturity:** C2 on `main` for Studio create/list/pin/delete/status/reconnect
and baked-proxy consume. Verified proxies feed Loupe/Develop/Before-After/scopes
with explicit `media_state=proxy` and `preview_apply_mode=identity_baked`
(`pixel_provenance=recipe_baked_srgb8`); catalog recipes stay canonical and are
not double-applied. Reconnect re-renders from the verified original
(`catalog_recipe`) and may clear the proxy unless pinned. User-initiated
eviction skips pinned proxies. Export remains fail-closed
(`proxy_export_forbidden`); ROI inspect while offline stays fail-closed
(`offline_proxy_roi_unsupported`). Residual toward C3: background quota policy,
collection generation, disk-full/cleanup automation, backup/restore corpus
(REL-01), and deferred delta-preview on baked pixels.

**Remaining work:**

- [done] COR-01 manifest/publication residuals;
- [done] baseline-vs-baked: identity consume while `media_state=proxy`; tests
  prove live Develop params cannot double-grade baked pixels
  (`OfflineEditProxyBakedIdentityNoDoubleGradeBeforeAfterAndReconnect`);
- [done] Before/After (`ignore_edits`) and interactive/scope preview consume the
  verified proxy under `media_state=proxy`;
- [done] reconnect re-renders from original, compares resolution-dependent
  cache keys/pixels, optional `--clear-proxy`;
- [done] Studio create/list/pin/delete/status/reconnect chrome in
  `OfflineEditDialog.qml` (no Main.qml growth);
- [done] user-initiated pin + evict (`max_total_bytes`, skip pinned);
- retain fail-closed full-resolution export while the original is unavailable;
- background quota/collection generation, disk-full automation, and
  backup/restore corpus remain REL-01 / C3;
- delta-preview on baked proxy pixels remains deferred.

**Acceptance gate:** an offline original can be viewed, graded, saved, reopened,
and compared in Studio; edits remain canonical recipes; reconnect returns to
original authority; stale/corrupt proxies are explicit; pinned data survives
eviction; source bytes remain unchanged. Met on `main` via service/Studio
contract tests. C3 corpus/package evidence remains open.

## DISPLAY-01 — Per-display ICC presentation

**Maturity:** C2 on macOS Studio workflow: monitor ICC discovery + screen-move
owner, machine-visible view pixel-kind contracts (Gallery/Loupe/Develop/
Before-After/comparison/magnifier/scopes/GPU), Loupe/Develop/Before-After/
comparison CPU paths apply `apply_display_presentation_rgb8` after soft-proof/
output, and contract tests prove recipe JSON / history / catalog revision /
export bytes unchanged across discover/refresh/inject. Soft-proof remains
recipe-owned; display transform is presentation-only. Windows uses best-effort
ICM (`windows_icm_display_icc` or explicit `windows_*` `fallback_srgb`); Linux
stays explicit `fallback_srgb` (C2 host discovery macOS-primary with
cross-platform fallback). Missing/corrupt injected ICC reasons are machine-
visible.

**Remaining work (post-C2 residuals, not blocking this C2 claim):**

- GPU native preview presentation parity (currently declared `output_referred`);
- Gallery thumbnail monitor convert (declared `output_referred` by contract);
- Linux packaged monitor discovery (or keep fallback claim);
- CPU/GPU matrix/LUT parity evidence, multi-display performance, SDR/HDR policy.

**Acceptance gate:** display movement changes presentation only; recipe/history/
catalog/export remain unchanged; every view declares pixel kind; transformed
pixels are compared directly; unsupported states are visible. Met for C2 Studio
CPU preview surfaces; residuals above remain for C3/host breadth.

## INGEST-01 — Packaged native camera transport

**Maturity:** C2 on `main` for Studio + filesystem-card + ptp-stub contract:
ImportPage transport selector and progress/report chrome (no `Main.qml`
growth), Copy-only ingest via `execute_ingest_detailed` with selected-path
filter, structured per-item report (imported/duplicate/skipped/failed), resume
checkpoint after disconnect/cancel with reconnect tests, idempotent repeated
ingest reporting duplicates, and fail-closed native PTP/MTP probe
(`native_ingest_adapter_not_packaged`) so mounts are never mislabeled native.
Move/camera-delete stay rejected on ingest transports.

**Remaining work (post-C2 residuals → C3):**

- decide/package ImageCaptureCore, WPD/WinRT, and libmtp ownership, licence,
  notices, and support matrix; flip `native_ingest_adapter_is_packaged()`;
- live device/session enumeration, permission, timeout, and hardware resume
  evidence on claimed hosts;
- Studio retry/permission UX once a packaged adapter exists;
- real camera/card corpus source-preservation and installed-package smoke.

**Acceptance gate:** each claimed host has a packaged adapter; sessions recover
from disconnect/cancel/restart; reports are exact; device originals are
unchanged; filesystem mounts are not mislabeled native. Met for C2 Studio
filesystem-card + stub; packaged native adapters remain C3.

## IQ-01 — Camera/profile and denoise quality admission

**Maturity:** C2 on `main` for fixture evaluation workflow evidence (ADR-0152).
Photographer-facing support claims can cite deterministic CPU denoise +
camera-profile probe results via `ravo iq evaluate` / Studio
`evaluateIqQuality`; missing corpus fail-closes. Aligns with IQ-00 CPU gold
(`require_cpu_gold_backend` on the denoise path). Learned denoise remains
blocked. Product camera certification and licensed real corpus = C3 residual.

**Done for C2:**

- [done] Engine `evaluate_iq_fixture_support` bundle with
  `support_claim_status=fixture_evidence_ready`,
  `camera_product_support_claimed=false`, `learned_denoise_admitted=false`,
  denoise `mean/max_abs_delta`, camera document SHA-256 + provenance;
- [done] in-tree synthetic corpus `Ravo/tests/fixtures/iq_evaluation_corpus/`;
- [done] CLI `ravo iq evaluate [--corpus] [--strength]` fail-closed without corpus;
- [done] thin Studio `iqQualityPolicy` + `evaluateIqQuality` (no Main.qml growth);
- [done] contract/CLI/presenter tests for fixture pass and missing-corpus fail-closed.

**Remaining (C3):**

- Build a licensed redistributable corpus plus private extension across skin,
  saturated fabrics, foliage, mixed light, underexposure, clipped highlights,
  high ISO, moiré, hot pixels, and lens extremes; record provenance and measure
  colour/hue/highlights/detail/noise/halos/false colour/scaling with blinded
  human review;
- Define camera/profile/lens update workflows that promote fixture evidence to
  product camera-support claims;
- Admit a real denoise provider only after runtime, weights, licence, package,
  memory, cancellation, updates, and no-model behavior are accepted (AI-00).

**Acceptance gate:** camera support means image-quality evidence, not decode
alone; changes carry before/after results and known failures; preview and native
export are covered; deterministic defaults never change silently. C2 satisfies
the fixture evaluation workflow portion of this gate; C3 closes licensed corpus
+ human review + learned-denoise admission.

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
