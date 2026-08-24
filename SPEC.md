# Ravo P0 / P1 Product Specification

> **Status:** Active product specification
>
> **Date:** 2026-08-24
>
> **Scope:** Ravo Studio desktop product priorities after the first catalog/import/viewer vertical slice
>
> **Normative:** Product behavior, architecture boundaries, persistence semantics and acceptance criteria in this document
>
> **Non-binding:** Concept images linked from this document; they communicate information hierarchy and interaction direction, not final visual design

## 1. Purpose

Ravo has crossed the first desktop threshold: the current implementation can create/open a local SQLite catalog, import local JPEG/PNG/RAW assets by reference, maintain an external preview cache, and display a Qt Quick gallery/viewer. The next work should turn that technical vertical slice into a coherent photo-review product before widening the editing surface.

This specification defines two product priorities:

- **P0 — Browse & Review:** make the library fast and comfortable for selecting, comparing and classifying photographs.
- **P1 — Basic Develop:** add a deliberately small, non-destructive global-editing workflow backed by the same versioned recipe and CPU engine used by the CLI.

P0/P1 are **product delivery priorities**, not replacements for the technical M0–M7 capability/risk map in `TODO_REWRITE.md`. A capability may be pulled forward as a narrow vertical slice when required by the product loop, while its broader milestone remains later.

## 2. Product principles

1. **Local-first and offline.** Core browsing, review and editing require no account, network service or remote asset.
2. **Originals are immutable.** Reference-only import never modifies, renames, moves or embeds edit state into the source image implicitly.
3. **Non-destructive by construction.** Review metadata and edits are explicit catalog/recipe state; pixels are regenerated from trusted inputs.
4. **One product path.** Ravo Studio and `ravo` consume the same domain/services/engine contracts. QML does not implement business rules or image algorithms.
5. **CPU correctness first.** UI controls are exposed only after the corresponding CPU operation and schema have a tested contract.
6. **Responsive ownership.** Filesystem scanning, database I/O, decode and render do not block the UI thread. Work is cancellable, bounded and owned.
7. **Versioned persistence.** Catalog migrations, recipes, operation parameters and machine-readable contracts are versioned and reject unknown future versions.
8. **No fake completeness.** Unsupported formats, operations and error states remain explicit instead of silently approximating success.

## 3. Current baseline

The starting point for this specification is the current Ravo Studio vertical slice:

- SQLite catalog schema v1;
- reference-only JPEG/PNG/RAW import and recursive directory import;
- external, rebuildable preview cache;
- Qt Quick/QML Gallery and viewer;
- Fit / 100% viewing baseline;
- C++20 domain/services/presenter boundaries over the existing Ravo Engine;
- versioned recipe/operation infrastructure and an initial CPU exposure operation.

Known product gaps relevant to P0/P1 include orientation/thumbnail correctness across a broader corpus, richer grid behavior, continuous zoom/pan, persisted review metadata, filtering/sorting semantics, edit persistence, preview scheduling for rapid parameter changes, and the required set of CPU operations for a useful basic Develop panel.

---

# P0 — Browse & Review MVP

## 4. P0 outcome

A photographer can import a folder, browse an adaptive image grid, switch between grid and single-image loupe without losing context, zoom and navigate quickly, assign stars/color labels/reject flags from mouse or keyboard, filter the library, close the application, and later reopen the catalog with review state intact.

The intended loop is:

```text
Import → Grid → Select → Loupe → Zoom / Navigate → Rate / Label / Reject → Filter → Revisit
```

P0 intentionally stops before image editing.

## 5. P0 information model

### 5.1 Review state

Each asset gains persisted review state owned by the catalog/domain layer:

```text
ReviewState
  rating:       integer 0..5
  color_label:  none | red | yellow | green | blue | purple
  rejected:     boolean
```

Semantics:

- `rating = 0` means unrated, not zero quality.
- `color_label` is a semantic enum. UI color is presentation; persisted meaning must not depend on RGB values or theme colors.
- `rejected` is independent from rating and color label.
- P0 does not introduce tags, collections, face recognition or a generic metadata key/value store.

### 5.2 Catalog migration

P0 introduces the next catalog schema version rather than mutating v1 in place. Migration must be transactional, repeatable in tests, and preserve an existing v1 library. Unknown higher schema versions remain fail-fast.

Suggested service-level commands are equivalent to:

```text
SetRating(asset_id, rating)
SetColorLabel(asset_id, label)
SetRejected(asset_id, rejected)
```

The exact C++ names may differ, but the mutation boundary must remain explicit. Gallery QML must not execute SQL or mutate repository rows directly.

## 6. Grid view

### 6.1 Required behavior

The Gallery becomes an adaptive, thumbnail-first grid:

- responsive column count and configurable thumbnail density/size;
- stable visual order while asynchronous thumbnails complete;
- clear selected and focused states;
- single selection required; multi-selection may be added only if it does not delay P0 exit;
- lazy thumbnail requests tied to visible/near-visible items;
- cancellation/reuse of work for items scrolled far offscreen;
- placeholder, loading, missing, unsupported and failed thumbnail states;
- orientation-correct thumbnails for the supported corpus;
- star/color/reject indicators visible without opening the loupe;
- empty-library and no-filter-results states with an obvious recovery action.

The model/presenter exposes immutable/read-only item snapshots. QML delegates may display state and submit commands, but they do not own asset lifetime, preview cache keys or repository mutation.

### 6.2 Selection continuity

Changing grid density, applying/removing a filter, returning from loupe, or refreshing a thumbnail must not silently select a different asset. If the current asset is filtered out, selection becomes explicitly empty or moves according to a documented rule; it must not appear to remain on a hidden asset.

## 7. Grid ↔ Loupe switching

P0 has two first-class browsing modes:

- **Grid:** many images, selection and review overview.
- **Loupe:** one image, detail inspection and rapid culling.

Required transitions:

- toolbar buttons expose both modes visibly;
- double-click or `Enter` on a grid item opens Loupe;
- `Esc` returns to Grid;
- switching modes preserves the selected asset;
- returning to Grid restores the selected tile to the visible area rather than resetting to the top;
- previous/next navigation works from Loupe without returning to Grid;
- a compact bottom filmstrip is part of the P0 target because it makes single-image culling and navigation legible, but its implementation may reuse the same virtualized item/presentation model as Grid.

Suggested shortcuts:

| Action | Shortcut |
| --- | --- |
| Grid / Loupe toggle | `G` / `E` or explicit toolbar buttons |
| Open selected in Loupe | `Enter` |
| Return to Grid | `Esc` |
| Previous / next asset | `Left` / `Right` |
| Fit | `F` |
| 100% | `1` |
| Rating | `0` … `5` |
| Reject toggle | `X` |

Shortcut letters may be refined for platform conventions, but numeric rating and arrow navigation are P0 behavior.

## 8. Loupe view

Loupe is an inspection surface, not yet an editor.

Required behavior:

- **Fit**, **Fill**, **100% (1:1)** and continuous zoom percentage;
- mouse wheel / trackpad zoom with reasonable bounds;
- drag-to-pan when image bounds exceed the viewport;
- zoom centered around pointer or a stable visible anchor where practical;
- previous/next navigation with selection continuity;
- current rating/color/reject state available without leaving Loupe;
- no synchronous decode or filesystem I/O on the UI thread;
- when a new selection supersedes an in-flight preview, stale results are discarded by request/revision identity;
- on preview failure, show an explicit state rather than leaving the previous asset visually masquerading as the new selection.

## 9. Rating, color label and reject workflow

Review actions must work identically from Grid, Loupe and filmstrip selection.

### 9.1 Stars

- 0–5 stars;
- click/tap target in the review bar and numeric keyboard shortcuts;
- repeated assignment of the same rating is idempotent;
- persistence completes through services/repository, not UI-only state;
- failures are visible and the UI reconciles to the persisted truth.

### 9.2 Color labels

P0 labels:

```text
None, Red, Yellow, Green, Blue, Purple
```

The first version does not define workflow meaning such as “needs retouch” or “client pick”; users/teams may interpret colors themselves. Persist enum identity, not presentation color.

### 9.3 Reject

- explicit rejected flag;
- visually distinct but not destructive;
- P0 never deletes or moves a rejected original;
- filters can exclude rejected assets without losing their metadata.

## 10. Filters and sorting

P0 deliberately implements a compact filter bar rather than a general query language.

Required filters:

- minimum or exact star rating;
- one or more color labels;
- rejected: include / exclude / only;
- clear-all-filters action.

Required sorting:

- import time;
- filename/display name;
- rating.

Ascending/descending behavior should be explicit. Tags, EXIF faceting, date trees, saved searches and collections are later product work.

## 11. P0 work packages

### P0.0 — Image correctness and preview baseline

- close orientation/rotation gaps discovered by the first Studio loop;
- make thumbnail and Loupe orientation agree;
- verify supported JPEG/PNG/RAW representatives and failure states;
- make preview cache contract/version invalidate stale incompatible entries safely.

**Exit:** the same asset has stable geometry/orientation in Grid and Loupe, and rebuilding cache does not change catalog truth.

### P0.1 — Review persistence

- catalog schema migration from v1 to the review-state schema;
- domain value types and repository/service commands;
- transactional persistence and reopen tests;
- presenter snapshots/signals for rating, label and reject.

**Exit:** review state survives application restart and does not touch originals.

### P0.2 — Production Grid

- adaptive layout and thumbnail density;
- virtualized/lazy preview requests and explicit states;
- selection/focus semantics;
- review badges and filter/sort bar.

**Exit:** representative libraries can be scrolled and filtered without synchronous decode/DB work on the UI thread.

### P0.3 — Loupe and navigation

- Grid/Loupe transitions with position preservation;
- Fit/Fill/1:1/continuous zoom and pan;
- previous/next navigation;
- virtualized filmstrip;
- visible review controls and shortcuts.

**Exit:** a user can cull a shoot without repeatedly returning to Grid.

### P0.4 — Recovery, regression and polish

- missing source / broken preview / cache rebuild behavior;
- keyboard focus and shortcut conflict review;
- HiDPI and resizing behavior;
- cancellation and late-result tests;
- long-scroll memory and resource checks;
- accessibility names for primary actions and review controls.

**Exit:** P0 acceptance criteria below pass on the actually tested platforms.

## 12. P0 acceptance criteria

P0 is complete when all of the following are demonstrated:

1. Import a representative local JPEG/PNG/RAW directory and obtain an adaptive Grid without modifying any original file.
2. Switch Grid → Loupe → Grid repeatedly while preserving asset selection and returning to the selected tile.
3. Fit, Fill, 100%, continuous zoom and pan behave consistently; fast previous/next navigation cannot let a stale preview overwrite the current asset.
4. Set and clear 0–5 stars, all defined color labels and reject state from the supported interaction surfaces.
5. Close and reopen the same catalog; review state and current catalog contents remain correct.
6. Filter by rating, color and reject state, clear filters, and sort by the defined P0 fields.
7. Missing/corrupt assets and failed thumbnails remain isolated failures; the application stays usable and does not display a previous image as a false success.
8. Unicode/non-ASCII local paths continue to work.
9. UI-thread policy is upheld: database I/O, filesystem operations and image decode/render are not performed synchronously from QML interaction handlers.
10. Resource ownership is explicit: closing the window/catalog cancels or drains owned work and prevents late writes into destroyed UI state.

A warm-catalog first-visible-grid target of approximately one second on the project’s reference development hardware is a **performance target**, not a cross-platform correctness invariant. Measured hardware, library size and result must be recorded whenever this target is claimed.

## 13. P0 non-goals

P0 does not include:

- image parameter editing or recipe UI;
- tags, hierarchical keywords, people/face recognition or map/GPS workflows;
- collections, smart collections or a general search DSL;
- managed-copy library semantics, file move/rename or delete-to-trash workflows;
- batch rename, batch metadata editing or sidecar writeback;
- full production export/publish;
- GPU backend work.

---

# P1 — Basic Develop MVP

## 14. P1 outcome

From the selected photograph, the user can enter a simple Develop workspace, make a small set of global non-destructive adjustments, see bounded responsive previews, compare before/after, undo/redo, reset controls, close the application, and later reopen the catalog with the edit restored.

The intended loop is:

```text
Loupe → Edit → Adjust → Compare → Undo / Redo → Reopen → Continue editing
```

P1 is intentionally **not** a miniature clone of the full darktable Darkroom or Lightroom Develop module. It establishes the durable editing architecture and only exposes operations with verified Ravo CPU behavior.

## 15. P1 editing surface

### 15.1 Required global controls

The initial target control set is:

**Geometry**

- Rotate left / right by 90°;
- simple crop with aspect-free mode; a small fixed-aspect set may be added after free crop is correct.

**White Balance**

- Temperature;
- Tint.

**Light**

- Exposure;
- Contrast;
- Highlights;
- Shadows;
- Whites;
- Blacks.

**Color**

- Vibrance;
- Saturation.

**Edit state**

- reset one control;
- reset one section;
- reset all edits;
- before/after toggle;
- undo / redo.

This list defines product intent, not permission to create UI-only approximations. A control enters the product only after its canonical operation/parameter schema, CPU implementation, color-space placement and tests satisfy the engine contract. If an operation is not ready, the control remains absent rather than becoming a fake placeholder.

## 16. Canonical edit persistence

The source of truth for edits is a versioned canonical recipe, not QML control state.

Minimum persistence behavior:

- each edited asset references an active canonical recipe/edit version;
- operation IDs and parameter schema versions remain stable/versioned;
- recipe writes are atomic;
- reopening the catalog reconstructs the control state from the canonical recipe;
- unknown future recipe/operation versions fail explicitly;
- editing state does not overwrite P0 rating/color/reject metadata;
- originals remain untouched.

P1 may use the simplest durable “one active edit version per asset” model that preserves the future ability to introduce photo versions/history. It must not bake a dead-end UI blob into catalog rows.

## 17. Preview/render architecture

Interactive editing uses the existing engine path rather than QML image effects.

Conceptual data flow:

```text
QML intent
   ↓
Desktop presenter / editing service
   ↓
immutable canonical recipe snapshot + request revision
   ↓
Ravo Engine CPU preview render
   ↓
verified read-only preview resource
   ↓
Presenter snapshot → QML
```

Required behavior:

- slider drags are coalesced/debounced so obsolete intermediate renders do not queue without bound;
- every preview request carries asset/edit/request revision identity;
- newer requests cancel or supersede older work where safe;
- stale results are discarded before presentation;
- preview dimensions, memory budget and worker count are bounded;
- CPU is the P1 reference backend;
- render failure preserves the last **verified** preview and displays an error state; it does not claim the failed parameters were rendered;
- progress/cancellation semantics remain compatible with the engine facade rather than creating a UI-specific scheduler.

## 18. Undo, redo and before/after

### 18.1 Undo/redo

Undo/redo operates on edit intent/recipe snapshots or equivalent service commands, not on mutable pixel buffers.

- a committed parameter gesture becomes one logical history step where practical;
- undo/redo results are deterministic for the same recipe/input;
- new edits after undo invalidate the redo branch according to documented session semantics;
- P1 only needs in-session undo/redo persistence unless a broader durable history design is deliberately pulled forward.

### 18.2 Before/after

Before/after compares the current edited result with the asset’s unedited baseline while preserving zoom/pan context when practical. The first implementation may be a press/toggle rather than split view; split/side-by-side comparison is not a P1 exit requirement.

## 19. P1 work packages

### P1.0 — Edit/version contract

- choose the minimal catalog ↔ recipe ownership model;
- schema/migration for active edit reference/version;
- atomic recipe persistence and reopen behavior;
- service commands and immutable edit snapshots;
- define session undo/redo semantics.

**Exit:** an edit recipe can be saved, reopened and validated without any desktop-only serialized state.

### P1.1 — Required CPU operations

For each exposed control:

- define stable operation ID and parameter schema;
- locate it correctly in the CPU pipeline/color contract;
- implement/port the behavior from frozen source/math where appropriate without old IOP/UI ABI;
- add unit/synthetic tests and representative real RAW/reference coverage;
- define illegal input, NaN/Inf, cancellation and resource behavior.

Crop/geometry work must also define ROI/preview implications rather than treating crop as a presentation transform.

**Exit:** every P1 control maps to an actually supported engine operation.

### P1.2 — Interactive preview scheduler

- edit request revisioning;
- coalescing/debounce policy;
- cancellation and stale-result discard;
- bounded preview sizes/memory/workers;
- resource lifetime through asset change, mode change, catalog close and window close.

**Exit:** aggressive slider movement and rapid image switching cannot make an obsolete render replace the current preview or grow work unboundedly.

### P1.3 — Develop UI

- Browse/Loupe → Edit mode transition with same selected asset;
- right-side grouped controls for Geometry, White Balance, Light and Color;
- numeric feedback and reset semantics;
- before/after and undo/redo affordances;
- edit-state indicator in Grid/Loupe where useful;
- keyboard/focus behavior that does not break P0 review shortcuts.

**Exit:** the complete P1 edit loop is usable without direct QML access to recipe internals or engine private state.

### P1.4 — Editing regression and polish

- reopen edited catalog and reproduce the same canonical recipe/result within documented tolerance;
- verify P0 rating/label/reject independence;
- missing original / unsupported operation / invalid recipe recovery;
- representative RAW and raster flows;
- memory/cancellation tests during rapid edits;
- accessibility and HiDPI pass for edit controls.

**Exit:** P1 acceptance criteria below pass on the actually tested platforms.

## 20. P1 acceptance criteria

P1 is complete when:

1. A P0 asset can enter/leave Edit mode without changing selection or review metadata.
2. Every visible P1 adjustment maps to a versioned, validated Ravo operation and CPU implementation; there are no UI-only pixel effects masquerading as product edits.
3. Edits are non-destructive and persist through catalog/application reopen.
4. Reset control/section/all, before/after and undo/redo yield deterministic recipe state.
5. Rapid slider gestures coalesce work and stale renders cannot overwrite newer requests.
6. Changing assets while a render is running cannot display the previous asset/result as a successful new preview.
7. Unsupported/invalid operations return structured errors and retain the last verified preview rather than silently accepting an unrendered state.
8. Representative operations have parameter/schema unit tests, synthetic boundary tests and at least one real RAW/reference validation appropriate to the operation.
9. Original file bytes remain unchanged.
10. Review metadata from P0 remains intact before and after editing.

## 21. P1 non-goals

P1 does not require:

- local masks, brushes, linear/radial gradients or blend graphs;
- tone curve, HSL/color mixer, advanced color grading or channel curves;
- full lens/camera calibration UX beyond whatever foundations are required for correct baseline rendering;
- presets/styles, copy/paste edits or batch synchronization;
- generative/AI features;
- public plugin ABI;
- GPU acceleration;
- full production JPEG/TIFF/metadata/ICC export workflow.

The existing renderer may be used for proof renders and tests, but the complete export product remains a later capability unless separately reprioritized.

---

# 22. Cross-priority architecture rules

P0 and P1 must preserve the existing dependency direction:

```text
Ravo Studio QML
      ↓ intents / immutable view state
Desktop C++ presenter
      ↓
ravo_services
   ↙       ↘
ravo_domain  Ravo Engine / recipe
   ↓             ↓
private catalog / codec / filesystem adapters
```

Hard boundaries:

- QML does not issue SQL, open codecs, own engine objects or serialize business truth.
- `domain` does not know SQLite/QML/codec types.
- `services` coordinate use cases and owned tasks; they do not duplicate image algorithms.
- engine/recipe do not depend on catalog or desktop.
- no production Ravo target includes frozen `src/` private headers or loads legacy IOPs.
- no detached threads; tasks have an owner, cancellation path and join/drain semantics.
- immutable/revisioned snapshots cross async/UI boundaries; late results are safe to discard.

# 23. Testing strategy

The P0/P1 test pyramid extends the existing Ravo test strategy:

| Layer | P0 focus | P1 focus |
| --- | --- | --- |
| Unit | review values, filters, sorting, migrations | operation schema/math, recipe/version semantics |
| Contract | repository/service commands, presenter revisions | edit services, preview scheduler, structured errors |
| Synthetic image | orientation/thumbnail geometry | per-operation boundaries, ROI, NaN/Inf |
| Golden/reference | representative preview correctness | CPU operation/recipe reference results |
| Integration | import → grid → loupe → review → reopen | open → edit → persist → reopen → render |
| Desktop | mode switch, shortcuts, focus, states | control binding, undo/redo, before/after |
| Resource/perf | long scroll, cancellation, cache bounds | rapid sliders, stale work, preview memory |

Only actually executed platform/test results may be reported as passing. Frozen fixtures remain read-only evidence and the frozen 0.9 application/test runner is not executed.

# 24. Relationship to `TODO_REWRITE.md`

`TODO_REWRITE.md` remains the technical capability and release-risk roadmap. This product specification changes immediate sequencing as follows:

- **P0** completes the current M2 browse/view work and pulls only **rating, color label and reject** persistence/filtering forward from the broader M4 photo-workflow milestone. Tags, collections, advanced metadata/search and file-management semantics remain M4/later.
- **P1** pulls a narrow first editing UX forward from M5, but each UI control depends on the corresponding CPU operation being validated through the M2-style engine correctness work. Full operation coverage, masks/blend and export remain later milestones.
- M3 reliability/cross-platform work is not skipped. P0/P1 features must inherit its recovery, deployment and resource-quality requirements as they become release candidates.

If this specification and an older roadmap sentence conflict about immediate product order, this document defines the current P0/P1 product priority, while architecture/MIGRATION/ADR constraints continue to govern technical boundaries.

# 25. Concept images

These images are intentionally **concept-only and non-binding**. They illustrate information hierarchy, density and primary interactions; exact spacing, typography, iconography, theme colors and component styling remain implementation decisions.

- [P0 Library Grid concept](Ravo/docs/concepts/p0-library-grid.svg)
- [P0 Loupe & Review concept](Ravo/docs/concepts/p0-loupe-review.svg)
- [P1 Basic Develop concept](Ravo/docs/concepts/p1-basic-develop.svg)

# 26. Definition of done for this specification

The specification is considered implemented only when the corresponding P0/P1 acceptance criteria are backed by repository evidence: code, migrations, tests, representative fixtures, documented platform validation and any necessary ADR updates. A concept image, QML screen or compilable target alone is not completion evidence.
