# Ravo developer documentation

Closed Lightroom Classic catalog import, source preservation and conversion
limits are defined in [ADR-0164](adr/0164-lightroom-catalog-reader.md), with
usage in [Ravo/README.md](../Ravo/README.md).

Metadata-only import folder planning, provisional v2 counts and independent
worker ownership are specified in [ARCHITECTURE.md](ARCHITECTURE.md), with
service/CLI/scheduling contracts in [TESTING.md](TESTING.md).

Studio Light's Sigmoid baseline policy is recorded in
[MIGRATION.md](MIGRATION.md); recipe preservation and 30% viewport zoom are
specified in [ARCHITECTURE.md](ARCHITECTURE.md) and validated by
[TESTING.md](TESTING.md).

Library view/selection resume, thumbnail/navigator stability, photo switching
without stretching, standard controls,
pinned Develop tools, directly visible global operation-instance controls,
local mask geometry/coverage feedback, curve gesture ownership
and stable-identity page location are owned by
[ARCHITECTURE.md](ARCHITECTURE.md), with startup/service/CLI contracts in
[TESTING.md](TESTING.md) and current behavior in [Ravo/README.md](../Ravo/README.md).

Display-resolution Develop interaction is specified by the 2026-10-02
amendment to [ADR-0087](adr/0087-progressive-develop-preview.md), with current
ownership in [ARCHITECTURE.md](ARCHITECTURE.md) and validation in [TESTING.md](TESTING.md).
Crop-aware prepared source density and exact final cache dimensions are also
specified in those authorities; previews are bounded by native source pixels.
Bounded, pixel-exact ICC row parallelism and the monitor-bound exposure latency
probe are documented in [ARCHITECTURE.md](ARCHITECTURE.md) and
[TESTING.md](TESTING.md).
Pinned crop controls, Auto Level, and the reduced crop surround are specified
by the presentation amendment to [ADR-0161](adr/0161-full-source-crop-workspace.md).

Same-size inspect-pan GPU source ownership is specified in
[ARCHITECTURE.md](ARCHITECTURE.md), with CPU-gold pan/cache regression contracts
in [TESTING.md](TESTING.md). Sparse filmstrip metadata demand and deferred
row selection share those ownership and validation authorities.

[ADR-0163](adr/0163-hdr-panorama-derived-assets.md) admits exposure-bracket merge
and planar panorama. Ownership is in [ARCHITECTURE.md](ARCHITECTURE.md), usage in
[Ravo/README.md](../Ravo/README.md), and validation in [TESTING.md](TESTING.md).

[ADR-0162](adr/0162-versioned-raw-rendering-profiles.md) defines the future RAW
calibration/default-rendering boundary; [profile execution gates](TODO_RAW_RENDERING_PROFILES.md)
track its pending implementation. Current runtime behavior remains in the
architecture and product README.

Companion JPEG preflight and JPEG file-size limits are recorded in [ARCHITECTURE.md](ARCHITECTURE.md),
its validation contract in [TESTING.md](TESTING.md), and user-facing behavior
in [Ravo/README.md](../Ravo/README.md).

Gallery thumbnail listing and demand lifecycle is recorded in
[ARCHITECTURE.md](ARCHITECTURE.md); reset-order, sparse-page cache recovery and
production Gallery image-readiness coverage are recorded in [TESTING.md](TESTING.md).

`DevDocs/` is the repository-owned source for architecture, product planning,
validation, dependency, packaging, compliance, and historical migration
records. Component `README.md` files remain beside the code they describe, and
`AGENTS.md` files remain at their scope roots for tool discovery.
CI compiler-cache restore and immediate save after successful compilation are
specified in [TESTING.md](TESTING.md); the build/cache/package ordering is
specified in [Packaging.md](Packaging.md).

Ravo's product north star is a professional, cross-platform photo manager and
non-destructive editor for working photographers, with optional AI-assisted
culling, retouching, and colour work that remains reviewable, reversible,
private by default, and reproducible enough to audit.

Import destination-tree preview ownership is in [ARCHITECTURE.md](ARCHITECTURE.md),
including inline counts, asynchronous branch reveal after listings settle, and
remembered destination/organization choices without importing, with overlay lifecycle
and QML presentation coverage in [TESTING.md](TESTING.md). Those authorities also
specify cancellable blocking during planning and bounded metadata reuse.
Import's always-visible preview settings, C++-owned filename component builder
and optional second-copy checkbox
are specified in [ARCHITECTURE.md](ARCHITECTURE.md), with opt-in/original-name,
extension, byte-preservation and production layout coverage in [TESTING.md](TESTING.md).
Color Harmonizer's inverse dt-UCS exception semantics and optimized
Clang coverage are recorded in [ARCHITECTURE.md](ARCHITECTURE.md) and
[TESTING.md](TESTING.md).
The asynchronous GPU preview handoff and its owned-pixel lifetime are specified
in those same architecture and testing authorities.
Library thumbnail-progress visibility and stable rail geometry are also recorded
in those authorities.

## Document authority

Service capability/resource ownership, bounded Preview buffers, Import workspace,
Library/Develop/Inspect/Export presenters, comparison frame identity and shared
cancellation generations are specified in
[ARCHITECTURE.md](ARCHITECTURE.md), with lifecycle and regression coverage in
[TESTING.md](TESTING.md). Historical migration records do not form a current
implementation checklist; current product execution stays in [TODO.md](TODO.md).
Remaining architecture-refactor qualification gates are in
[TODO_ARCHITECTURE_REFACTOR.md](TODO_ARCHITECTURE_REFACTOR.md) within that queue.

Burst Compare availability and stack/selection lifetimes are specified in
[ARCHITECTURE.md](ARCHITECTURE.md), with regression coverage in
[TESTING.md](TESTING.md) and the service contract in
[ADR-0155](adr/0155-cull-burst-stack-compare-pair.md).

Full-source crop rotation and its output-frame mapping are specified in
[ADR-0161](adr/0161-full-source-crop-workspace.md).

Foreground preview/import isolation and photo-scoped edit conflicts are specified
in [ADR-0160](adr/0160-foreground-preview-and-import-isolation.md).

The Global/Mask Develop workspace and Recipe v4 local groups are specified in
[ADR-0158](adr/0158-mask-scoped-develop-workspace.md); current ownership is in
[ARCHITECTURE.md](ARCHITECTURE.md) and validation policy in
[TESTING.md](TESTING.md).

| Document | Owns | Does not own |
| --- | --- | --- |
| [ARCHITECTURE.md](ARCHITECTURE.md) | Current target boundaries, image-pipeline defaults, ownership, lifecycle, threads, data, and failure behavior | Future work or run diaries |
| [MIGRATION.md](MIGRATION.md) | Accepted capability history, removed leftovers, and retirement decisions | Product backlog |
| [ProductRoadmap.md](ProductRoadmap.md) | Outcome order, product principles, and cross-layer decisions not ready for execution | Task-level status |
| [TODO.md](TODO.md) | Product execution queue: corpus/latency, Gallery evidence, professional workflow, and AI | Completed behavior, durable decisions, or package closeout |
| [TESTING.md](TESTING.md) | Test layers, fixtures, deterministic contracts, performance probes, and validation depth | Product priority |
| [adr/README.md](adr/README.md) | Accepted architecture decisions and supersession history | Mutable implementation status |

Product execution belongs only in [TODO.md](TODO.md).
Export form defaults and size-selection ownership are documented in
[Ravo/README.md](../Ravo/README.md) and [ARCHITECTURE.md](ARCHITECTURE.md).
Gallery's bounded persistent display-thumbnail cache, automatic eviction recovery,
and worker lifecycle are
specified in [ARCHITECTURE.md](ARCHITECTURE.md).
Studio's supported photo shortcuts are listed in [Ravo/README.md](../Ravo/README.md);
command ownership and transactional keyboard review belong to
[ARCHITECTURE.md](ARCHITECTURE.md).
Immediate photo click-to-1:1 and view restoration are specified in
[ADR-0076](adr/0076-photo-inspect-toggle-actual-size.md), with current ownership
and input tests in architecture and testing above.
Three-platform package evidence belongs in [Packaging.md](Packaging.md) (includes packaged-runtime checker + `package_rehearsal`).
That document also owns Linux ICU runtime bundling and the package-local
SONAME and origin-relative executable search-path verification gates;
build-host libraries cannot substitute for payload files.

The import workspace, exact-content classification with visible disabled duplicate
photos, remembered source/destination paths in Home and mounted-volume folder
trees, and the
Home non-recursive scan safeguard, read-only destination folder/count previews,
independent folder loading, enumeration-first placeholders and ordered,
catalog-independent thumbnail scheduling are defined by
[ADR-0102](adr/0102-planned-managed-import-workspace.md), with typed
desktop preference ownership in [ADR-0066](adr/0066-typed-desktop-language-setting.md).
Their current contracts and reproducible validation live in architecture and testing above.
Import scan and execution exclude the current catalog's preview and support
trees; path identity and regression coverage live in those same authorities.
They also define Import's source-file context menu and its generation-bound
candidate identity, isolated from Gallery commands.
Import readiness after enumeration, immediate Gallery handoff and viewport-first/background thumbnail
scheduling are specified in architecture, with the blocked-worker tests in testing.
Embedded Exif altitude defaults and strict malformed-tag handling are owned by
the Engine metadata reader, documented in architecture and testing above.
Gallery folder publication, background monitor-thumbnail presentation and its
private-catalog latency probe are documented in those same authorities.
They also define cache-first RAW selection, settled-frame reuse on Develop
entry, and camera-JPEG-first Import thumbnails.
RAW black-level normalization and preview-cache invalidation are specified in
[ARCHITECTURE.md](ARCHITECTURE.md), with synthetic DNG coverage in
[TESTING.md](TESTING.md).
The same authorities define owned CPU/GPU inspect-ROI publication and hidden-image
source gating.
Startup splash ownership, main-window handoff and geometry isolation are also
specified in [ARCHITECTURE.md](ARCHITECTURE.md); their lifecycle and offscreen
validation are specified in [TESTING.md](TESTING.md).
Those authorities also define per-user side-panel/filmstrip size persistence,
bounded layout intents, and settings-failure/offscreen validation.
Import source restoration, unavailable-source tree collapse and superseded
restore rejection are specified in the same architecture/testing authorities.
Review-bar vector flag presentation and its existing command ownership are
specified in [ARCHITECTURE.md](ARCHITECTURE.md).
HEIC/HEIF macOS decode and deterministic import file identity follow
[ADR-0159](adr/0159-owned-heic-macos-decode.md) and the same architecture/testing
authorities; native dependency ownership is in
[Dependency_Workflow.md](Dependency_Workflow.md).

## Operations and compliance

| Document | Scope |
| --- | --- |
| [Dependency_Workflow.md](Dependency_Workflow.md) | FreeCM source roots, local integration, refresh, and publication order |
| [Packaging.md](Packaging.md) | CI Qt/Python prerequisites, Linux desktop runtime dependencies, five platform/architecture combinations, seven release artifacts, and fresh-runner native startup gates |
| [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) | Tracked third-party attribution and licence notices packaged with Ravo |

The following remain separate owners and are not folded into `DevDocs/`:

- `Ravo/README.md`: current user-visible and machine-visible capability baseline;
- `userdoc/`: publishable user handbook;
- `FreeCM/`: independent submodule;
- `.codex/skills/`: executable agent workflows.

## Planning flow

A product idea moves through one direction only:

```text
ProductRoadmap -> dated ADR -> TODO -> code/tests -> current authorities
```

1. Keep an undecided cross-layer capability in `ProductRoadmap.md`.
2. Before implementation, accept a dated ADR that names the user outcome,
   owner, persisted or machine contract, cancellation/failure behavior,
   privacy and security constraints where relevant, and validation gate.
3. Add only the unfinished execution slice to `TODO.md`.
4. On completion, move durable facts to code, tests, `Ravo/README.md`,
   `ARCHITECTURE.md`, or `TESTING.md`, then delete the completed TODO item.
5. Record removed or explicitly rejected legacy behavior in `MIGRATION.md`.

## Maintenance rules

1. Keep one authority per topic. Do not duplicate current behavior across the
   roadmap, TODO, architecture, and migration documents.
2. TODO entries contain only unfinished work, dependencies, risks, concrete
   validation, and acceptance gates. They do not contain completed checklists.
3. Do not use target dates as a substitute for evidence. Priorities are ordered
   by user outcome, dependency, and release risk.
4. Remove obsolete plans, historical run diaries, and concept mockups instead
   of archiving competing descriptions.
5. Keep transient reports, private-corpus results, screenshots, and machine-
   local measurements outside the repository unless a stable test fixture or
   generated evidence owner explicitly requires them.
6. Update generated output, including third-party notices, only through its
   owning script.
7. For documentation-only changes, verify real paths, relative links, commands,
   terminology, and whitespace; do not claim an unrun platform check passed.
