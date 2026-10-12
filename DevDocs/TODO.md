# Ravo product execution TODO

The queue prioritises source safety, catalog durability, colour correctness,
responsiveness and release qualification. Current capabilities and evidence
belong to the authorities linked below; historical run logs belong in Git/CI.

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

The current user-authorized AI control tranche extends LC-03 through ADR-0184;
it reuses existing owners without admitting new inference models. The code
retirement queue remains tracked below rather than implicitly closed by it.
[TODO_CODE_RETIREMENT.md](TODO_CODE_RETIREMENT.md) owns the ordered removals and
validation gates; [ADR-0181](adr/0181-retire-unused-dependencies-and-product-stubs.md)
protects stored edits and durability while retiring unused roots and stubs.

The user-requested LightCraft adoption queue is
[TODO_LIGHTCRAFT_ADOPTION.md](TODO_LIGHTCRAFT_ADOPTION.md). Its first priority is
LC-00: comparable Release parameter-render measurements and bottleneck attribution
inside PERF-01, followed by measured LC-01/02 optimizations under PERF-02 and
IQ-00. Performance qualification remains open for idle-CPU windows; the current
user-directed execution advances LC-03 machine workflows and other adoption work.
Existing source-safety/correctness blockers still take precedence. The
queue does not admit Rust, a second renderer/UI, silent fallback, or unapproved
models/formats; those expansion items retain explicit decision gates.

The explicitly requested video work is bounded by
[ADR-0165](adr/0165-video-library-and-playback.md). Host, audio and real-corpus
qualification remains in [TODO_VIDEO_SUPPORT.md](TODO_VIDEO_SUPPORT.md).

Architecture-refactor qualification shares REL-01, PERF-01, IQ-00 and REL-02.
Its remaining external gates are consolidated under
[Cross-cutting qualification gaps](#cross-cutting-qualification-gaps). Current
Import/Inspect ownership belongs in [ARCHITECTURE.md](ARCHITECTURE.md); this
stream admits no new capability, renderer, scheduler, schema or dependency.

The explicit user-requested HDR/Panorama work is admitted by
[ADR-0163](adr/0163-hdr-panorama-derived-assets.md) as a narrow exception to the
specialization freeze. Remaining release gates are real exposure-bracket and
overlapping-photo corpus quality (motion, clipping, rotation, parallax), native
resolution memory/latency measurement and installed macOS/Windows/Linux package
smoke. Float DNG, spherical projection and multiband blending need separate
contracts. This exception does not admit tethering or reopen leftover IOPs.

The user-requested RAW starting-appearance design is recorded in
[ADR-0162](adr/0162-versioned-raw-rendering-profiles.md). Its pending calibration,
baseline-binding, Standard/Camera Matching and validation gates are tracked in
[TODO_RAW_RENDERING_PROFILES.md](TODO_RAW_RENDERING_PROFILES.md). This design does
not claim a new runtime default or reopen leftover IOP ports.

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
- do not restore retired deterministic AI proposal chrome or tethering probes;
  real model/hardware work requires its own admission and SPECIALIZE stays deferred;
- finish correctness, Studio workflow, persistence, backup/restore,
  cross-platform, and performance residuals on accepted contracts;
- do not grow `Main.qml`; a P1 change that touches it must move orchestration
  into a dedicated component or C++ presenter and produce no net line growth.

A data-loss, security, catalog-corruption, colour-integrity, or unbounded
resource defect preempts both streams.

## Execution order

### Release and correctness lane

| Order | ID | Priority | Outcome |
| ---: | --- | --- | --- |
| 1 | CI-01 | P0 | Keep the latest main SHA green under direct-push history safety |
| 2 | COR-01 | P0 | Fix reviewed mutation, rollback, and support-file safety defects |
| 3 | REL-01 | P0 | Prove source safety, catalog durability, interruption recovery, and upgrade |
| 4 | PERF-01 | P0 | Freeze Gallery, viewer, Develop, analysis, and large-library budgets |
| 5 | IQ-00 | P0 | Complete preview/reopen/export, ICC, ROI, and CPU/GPU consistency evidence |
| 6 | REL-02 | P0 | Prove installed packages and backup/restore on supported hosts |

### Remaining workflow qualification

Respect the WIP limit and the dependencies below. Existing C2 workflows are
accepted baselines; this lane lists their unfinished implementation/evidence gates.

| Order | ID | Priority | Outcome |
| ---: | --- | --- | --- |
| 1 | LOCAL-01 | P1 | Mixed-corpus mask interaction/recovery; measured GPU follow-up after PERF-01 |
| 2 | EDITIN-01 | P1 | Windows/Linux installed-package TIFF matrix |
| 3 | CULL-01 | P1 | Analysis corpus precision/recall and indexed scaling |
| 4 | OFFLINE-01 | P1 | Quota, collection generation, corpus and delta-preview gates |
| 5 | DISPLAY-01 | P1 | Linux monitor discovery, host/corpus parity and SDR/HDR policy |
| 6 | INGEST-01 | P1 | Packaged native adapters and physical-device qualification |
| 7 | IQ-01 | P1 | Licensed corpus, human review and real denoise admission |

# P0 — Repository, correctness, and release gates

## CI-01 — Green main under direct-push history safety

Follow [direct-main policy](TESTING.md#direct-main-development-loop) and
[package qualification](Packaging.md). Every new source SHA needs its own CI
results; previous local repairs do not close the remote-platform gate.

**Remaining work:**

- keep `main` protected from deletion and non-fast-forward rewrites;
- do not accept another product-state commit or tag while the latest main run is
  queued, in progress, failed, or cancelled;
- keep Release / RelWithDebInfo compile plus minimal CLI/Studio smoke on the
  matrix;
- make tag packaging and release publication depend on the same successful
  source SHA, with no red-commit or skipped-job path;
- document direct-main policy and fix-forward repair (no required-check
  merge gate pretending to be the daily path);
- keep flaky tests visible and owned rather than converting them to permanent
  skips.

**Acceptance gate:**

- main history is protected from deletion/non-fast-forward updates;
- every pushed main SHA receives the normal CI matrix;
- a red/incomplete main blocks subsequent product work and release qualification;
- package/release operates only from a successful same-SHA workflow;
- published main history is repaired forward rather than rewritten.

## COR-01 — Reviewed mutation regression gate

- Exercise the existing instance-ID, mask ownership, catalog transaction,
  cancellation, proxy publication and near-duplicate bounds contracts whenever
  these owners change. Preserve their failure-injection and committed-state
  assertions.
- Complete mixed-corpus reopen/backup/restore qualification under REL-01.
  Unit/contract coverage does not close that corpus gate.
- Keep durable mutation invariants and regression scope in
  [ARCHITECTURE.md](ARCHITECTURE.md) and [TESTING.md](TESTING.md), rather than
  retaining completed implementation checklists here.

## REL-01 — Real mixed-photo corpus, source safety, and recovery

Next vertical scenario only: **source offline → restore → reopen the same
library** on a real mixed directory with immutable source bytes and persistent
readback. Do not expand Import page structure further in that tranche.

Real mixed-corpus qualification remains open, including X-Trans, PNG/TIFF,
Windows/Linux, host disk-full, large-library and upgrade-failure scenarios.
Historical selected-corpus or fixture results do not establish C3. Package
records belong in [Packaging.md](Packaging.md), not an execution diary here.

Use an explicit read-only mixed RAW/raster corpus in `RAVO_PHOTO_CORPUS`, a
Release build, and a unique temporary root for every generated catalog,
preview, recovery mirror, proxy, derived file, backup, and report.
`RAVO_PHOTO_CORPUS` is the only private photo corpus entry point; do not use a
separate REL-01-only env var. An availability probe is not REL-01 evidence.
`PhotoCorpusOfflineRestoreReopenVertical` is an opt-in validation entry point;
its bounded selection cannot close the full REL-01 corpus gate.

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
- catalog, recipes, metadata, stacks, proxies, derived assets and provenance remain
  coherent through reopen/upgrade/backup/restore;
- missing dependencies and unsupported inputs fail structurally;
- an unset corpus, skipped test, or unshared local report is not evidence.

## PERF-01 — End-to-end interaction and large-library budgets

Freeze cross-host budgets from current Release reports; historical one-shot
observations are not accepted budgets or PERF-02 admission. Complete LC-00's
remaining workload/profile/native-frame qualification in
[TODO_LIGHTCRAFT_ADOPTION.md](TODO_LIGHTCRAFT_ADOPTION.md). Performance may wait
for an idle CPU window under the current user direction. Follow the
[measurement protocol](TESTING.md#interactive-performance-comparison); its
interactive matrix requires at least 30 non-no-op samples.

Measure complete user-visible paths with two warmups and at least eight recorded
samples:

- import publication, first placeholder/thumbnail, viewport completion, and
  exact browse preview;
- adjacent select/revisit, Fit/30%/Actual Size, and 1:1 ROI;
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

Apply the CPU-gold and admitted-GPU policy in
[ADR-0151](adr/0151-iq-cpu-gpu-consistency-gate.md) and
[MIGRATION.md](MIGRATION.md). Existing macOS fixture contracts do not close real
corpus, Windows/Linux, proof-monitor, multi-instance or full sensor coverage.
Same-scale comparisons and explicit cross-scale non-comparisons remain required.

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

- Qualify [ADR-0169](adr/0169-studio-release-update-check.md) on installed
  macOS/Windows/Linux packages: manual check, silent offline startup, saved
  automatic-check preference and confirmed system-browser release handoff.
  Injected network/URL tests do not establish native browser launch.

Windows ZIP, Linux AppImage/DEB, native interaction/localization/upgrade-failure
host matrices and signing/notarization claims require final-artifact evidence.
Historical macOS package results in [Packaging.md](Packaging.md) do not close
REL-02 or C3 for a new source candidate.

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

## LOCAL-01 — Professional mask-scoped local adjustments

Current ownership and compatibility are specified in
[ADR-0158](adr/0158-mask-scoped-develop-workspace.md) and
[ARCHITECTURE.md](ARCHITECTURE.md). The prior per-operation instance UI is an
advanced global control, not the primary local-adjustment workflow.

**Remaining work:**

- Measure mixed-corpus mask creation, long brush strokes, multiple local groups,
  and 1:1 interaction against PERF-01 budgets on all three platforms.
- Extend GPU mask/group evaluation only after measured need and CPU equality.
- Retain the full mixed-corpus reopen/backup acceptance under REL-01.

**Acceptance gates for further changes:**

- Global and every selected mask remain independent across all exposed controls.
- Gesture cancellation, stale requests and failed writes preserve prior state.
- Preview/export/reopen/history/undo/snapshot/style/copy/batch agree.
- Masks remain aligned through accepted geometry; QML owns no durable geometry
  or pixels, and further Main.qml orchestration is extracted into owned views.

## EDITIN-01 — Studio external-editor round trip

**Remaining work:**

- validate TIFF profile/bit-depth/dimensions/naming equality on installed
  Windows/Linux packages (service/CLI coverage is not deployed-host evidence);
- keep watch-folder auto-import and proprietary scripting out of scope.

**Acceptance gate:** originals remain byte-identical; unchanged, missing,
source-mutated, stale, and conflicting returns are visible; publication is
atomic; reopen/backup/restore/export work without CLI assembly.

## CULL-01 — Transactional keyboard review and evaluated assistance

**Remaining work:**

- replace pairwise O(N²) with an indexed design beyond the hard asset bound
  only after an accepted ADR (PERF-01);
- measure precision/recall and false-positive cost on approved corpora.

**Acceptance gate:** the keyboard loop needs no pointer; a committed mutation
cannot be reported as failed; review remains responsive during analysis;
heuristic thresholds have metrics and known failures; cancel/restart/missing
source/corrupt decode/stale fingerprint/resource exhaustion are deterministic.
Analysis quality metrics and indexed scaling remain open.

## OFFLINE-01 — Offline-original editing

**Remaining work:**

- retain fail-closed full-resolution export while the original is unavailable;
- background/auto quota enablement, collection generation, live host disk-full,
  and backup/restore corpus remain REL-01 / C3;
- delta-preview on baked proxy pixels remains deferred.

**Acceptance gate:** an offline original can be viewed, graded, saved, reopened,
and compared in Studio; edits remain canonical recipes; reconnect returns to
original authority; stale/corrupt proxies are explicit; pinned data survives
eviction; source bytes remain unchanged. C3 corpus/package evidence remains open.

## DISPLAY-01 — Per-display ICC presentation

**Remaining work:**

- Qualify full-corpus Windows/Linux CPU/GPU matrix/LUT parity, ROI and monitor
  behavior. Keep same-scale pixel comparisons and explicit cross-scale exclusions
  from IQ-00; macOS owned-surface fixtures do not close other hosts.
- Extend non-admitted GPU operations only after PERF-01 identifies measured need,
  with CPU-gold admission; no blanket GPU port queue is implied.
- Qualify Linux/non-Metal native publication and packaged Linux monitor discovery,
  or retain the explicit current fallback claim.
- Measure multi-display performance and decide SDR/HDR presentation policy.

**Acceptance gate:** display movement changes presentation only; recipe/history/
catalog/export remain unchanged; every view declares pixel kind; transformed
pixels are compared directly; unsupported states are visible. Residuals above
remain for C3/host breadth.

## INGEST-01 — Packaged native camera transport

**Remaining work:**

- decide/package ImageCaptureCore, WPD/WinRT, and libmtp ownership, licence,
  notices, and support matrix; flip `native_ingest_adapter_is_packaged()`;
- live device/session enumeration, permission, timeout, and hardware resume
  evidence on claimed hosts;
- Studio retry/permission UX once a packaged adapter exists;
- real camera/card corpus source-preservation and installed-package smoke.

**Acceptance gate:** each claimed host has a packaged adapter; sessions recover
from disconnect/cancel/restart; reports are exact; device originals are
unchanged; filesystem mounts are not mislabeled native. Packaged native adapters
remain unqualified.

## IQ-01 — Camera/profile and denoise quality admission

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
export are covered; deterministic defaults never change silently. Licensed corpus,
human review and learned-denoise admission remain open.

# P2 — Frozen important extensions

Do not implement these while the WIP freeze is active.

- **FORMAT-01 — HEIC/HEIF:** admit Windows/Linux providers and package/licence
  evidence; expand beyond ADR-0159's macOS primary-photo SDR decode only with
  explicit HDR/alpha/sequence contracts and a broader corpus.
- **FORMAT-02 — DNG and browse Smart Preview:** choose owned converter/encoder;
  preserve Copy-only/browse-only semantics and keep this class distinct from
  offline-edit proxies.
- **META-01:** add only a target cohort's required IPTC Extension/contact/scene/
  subject fields, controlled vocabularies, privacy, facets, and XMP authority.
- **CONVERT-01:** close RAWmakase integration and real-catalog acceptance gates
  in [TODO_LIGHTROOM_IMPORT.md](TODO_LIGHTROOM_IMPORT.md). ADR-0166 admits
  compatible database edits, copies, metadata, static collections, preservation
  and explicit import mapping. Raw preservation does not complete operational
  field support or remaining ownership/renderer gates.
  Capture One still requires a read-only, versioned, reportable packaged reader.
- **DELIVERY-01:** advanced overlays, fonts, templates, printing, and publishing
  need separate delivery ownership and must not mutate Develop.
- **SPECIALIZE-01:** tethering is deferred and has no product probe/API.
  Do not restore it without an external cohort,
  hardware/corpus access and a new tethering decision. ADR-0163 admits only the
  separately requested HDR/Panorama derived-asset workflows.

# P2 — Real AI admission

Deterministic proposal/suggestion providers and their Studio pane are retired
under ADR-0181. Do not add another stub, provider, prompt surface or product claim.

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

## Import qualification gaps

- Qualify production Import keyboard/text-focus interactions on each supported
  host, including GeoControls fields and filename-template Select All. Component
  harnesses and offscreen smoke do not establish native packaged interaction.
- Keep decoder peak RSS separate from bounded model-thumbnail accounting.
  Corpus, thermal and close/join qualification follow PERF-01 and the
  cross-cutting gates below.
- Audit remaining string-valued Import draft inputs before claiming complete
  typed-state admission; preserve strict command validation.
- Run current-SHA packaged import/reopen tests with AppImage FUSE, DEB install
  and native display under REL-02. Reproduce output digests and runtime identity
  from final artifacts; prior local CLI runs are not that evidence.
- Retain instrumented-Qt TSan and private-corpus gaps in their owning
  architecture/release queues. A queued wakeup is not race proof, synthetic PNG
  is not a real corpus, and process lifetime is not catalog reopen.

## Cross-cutting qualification gaps

- Extend independent Release fixture/synthetic evidence to representative corpus
  and thermal runs under REL-01/PERF-01/IQ-00. Retain comparison baseline
  `7656ba368732755f94ccd7c2b529e6fa38adb16e`. Measure browse/filter/paging,
  edit-to-visible latency, thumbnail demand, queue/thread bounds, RSS and close/
  join latency under the same workload. Do not regenerate pixel gold or enlarge
  timing windows to accept regressions. Offscreen publication does not qualify
  native frame presentation, private corpus, ICC/GPU or physical hardware.
- Qualify Windows/Linux against complete current-source CI job logs. Historical
  failures and local macOS fixes establish neither a current pass nor a current
  remote failure. Missing toolchains remain untested.
- Obtain a matching instrumented Qt SDK for unsuppressed desktop TSan Import,
  Inspect, command and lifecycle checks. Qt-only queued-handoff reproductions
  classify a mechanism; joined service/executor tests and ASan do not qualify
  desktop synchronization. Do not suppress callbacks or skip mandatory smoke.
- Run installed-package create/import/probe/reopen/backup/restore/relocate checks
  on actual Windows/Linux/macOS runtimes under REL-02/Packaging. CMake staging
  does not deploy Qt or establish native display/hardware behavior. Signing and
  release publication retain separate authorization.

Validation: read-only source-root/fixture/boundary checks, serial affected-target
build and RavoCodeQuality, focused owner tests, then full applicable suites for
shared lifecycle/surface changes. Use the commands in [TESTING.md](TESTING.md).
The external gates above remain unfinished; consolidation does not close them.
