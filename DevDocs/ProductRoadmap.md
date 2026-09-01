# Ravo product roadmap

This roadmap orders product outcomes. It does not authorize implementation,
replace a dated ADR, or duplicate the task-level queue in [TODO.md](TODO.md).
Current behavior belongs in `Ravo/README.md`, [ARCHITECTURE.md](ARCHITECTURE.md),
[TESTING.md](TESTING.md), code, and tests.

## North star

Ravo is intended to become a professional photo-management and lightweight
non-destructive editing application for photographers who need to ingest,
cull, organize, grade, deliver, recover, and revisit large bodies of work.
AI should reduce repetitive work without taking authority away from the user:
its results must be previewable, reversible, attributable to a model/provider,
and safe for originals and catalog state.

## Product principles

- **Professional reliability before feature count.** No lost originals, silent
  catalog corruption, hidden fallback, or ambiguous partial success.
- **One non-destructive authority.** SQLite, canonical recipes, canonical masks,
  catalog versions, and explicit derived assets remain the owned product state.
- **One behavior across clients.** Studio and the `ravo` CLI use the same domain,
  service, recipe, and Engine contracts; QML presents state and forwards intent.
- **Measured responsiveness.** Optimize from representative Release evidence,
  not synthetic component timing alone.
- **Explicit interchange.** XMP, external editors, catalog conversion, remote
  services, and uploads are user-initiated operations with conflict rules.
- **AI is optional and subordinate.** The product remains usable without a
  model, accelerator, network connection, or provider account.
- **Privacy and provenance are product features.** Remote payloads, credentials,
  model identities, licences, and generated results have visible ownership and
  retention rules.

## Outcome sequence

The sequence is dependency-based rather than date-based. A later stage may be
researched early, but it is not release-ready until the earlier exit gates it
relies on are current.

### R0 — Release confidence

**Outcome:** the existing catalog, Gallery, Develop, recovery, export, package,
and machine contracts are demonstrably safe on supported hosts and a real
mixed-photo corpus.

**Exit gate:** the private-corpus, Gallery measurement, Windows, Linux, and
same-commit closeout items in `TODO.md` pass without platform-specific hidden
fallback. Release evidence records source preservation and does not generalize
one host's latency budget to another.

### R1 — Professional ingest, catalog, and culling

**Outcome:** a photographer can move from card or folder to a durable, searchable
shoot with predictable naming, verified copies, fast culling, deep metadata,
and recoverable organization at large-catalog scale.

**Decision themes:** PTP/MTP ingest, HEIC/HEIF, DNG/Smart Preview policy,
hierarchical keywords and IPTC depth, location/facet indexing, duplicate
identification, and offline/removable storage behavior.

**Exit gate:** representative 100,000-photo workflows remain bounded; bulk
mutations are revision-checked, cancellable, undoable where appropriate, and
recoverable; originals remain byte-identical unless an explicit destructive
command succeeds.

### R2 — Professional grading and delivery

**Outcome:** daily edit and delivery work does not require another application
for ordinary local grading, repeatable batch treatment, web/client exports, or
explicit external-editor interchange.

**Decision themes:** remaining masked Highlights/Shadows/Curves and
picker-assisted authoring, output resize and sharpen order, reusable export
presets, restartable jobs, explicit XMP/catalog conversion, and derived-asset
external-editor round trips.

**Exit gate:** preview, export, reopen, history, undo, and batch application use
one canonical result; package and colour behavior are verified on supported
hosts; every external round trip preserves the original and has a conflict
matrix.

### R3 — AI-assisted selection and colour

**Outcome:** AI can propose useful culling, metadata, tonal, colour, and semantic
mask work while every change remains understandable and under user control.

**Initial capability order:**

1. opt-in metadata/cull suggestions and duplicate/near-duplicate assistance;
2. white-balance, exposure, tone, colour, and reference-grade proposals expressed
   as normal recipe fields;
3. subject, sky, background, skin, clothing, and object selections expressed as
   canonical masks;
4. shoot-level consistency that applies an explicit proposal to a selected set
   through the existing batch contract.

**Exit gate:** a dated AI architecture decision defines provider/model identity,
local versus remote execution, payload minimization, credentials, cancellation,
resource bounds, model/licence distribution, evaluation data, and failure
behavior. Applying a proposal creates normal history/undo state; rejecting or
cancelling it changes nothing.

### R4 — AI retouch and derived generation

**Outcome:** repetitive dust, blemish, distraction, and object-cleanup work can
be accelerated without disguising generated pixels as deterministic recipe
parameters.

**Decision themes:** deterministic retouch proposals may use canonical Retouch
regions; non-replayable generated pixels become an immutable derived asset or
version with source revision, model/provider/version or weight hash, input
settings, content hash, and privacy provenance. Originals are never replaced.

**Exit gate:** reopening, backup/restore, export, missing-model behavior, model
upgrade, cancellation, partial publication, and provider unavailability are all
explicit. A result remains inspectable even when the generating model is no
longer installed.

### R5 — Studio-scale workflow

**Outcome:** Ravo supports repeated commercial work across capture, review,
delivery, archive, and optional collaboration without creating a second catalog
or cloud-only authority.

**Decision themes:** tethering, print, proofing/publishing, dual-display review,
portable or shared catalogs, and collaboration. These remain unauthorized while
[MIGRATION.md](MIGRATION.md) records them as removed leftovers unless a new dated
product decision accepts an independent Ravo contract.

## Deferred product contracts

The following topics require a dated decision before a Ready item may enter the
execution queue.

### Local adjustment expansion

Ravo already owns the canonical mask graph and accepted consumers. Color
Balance RGB (ADR-0108) and Exposure (ADR-0109) each may carry one owned
canonical mask. Decide which additional everyday operations (Highlights,
Shadows, Whites, Blacks, Curves) may own a mask, whether multi-instance
grading is allowed, how picker/histogram assistance stays in C++, and how mask
geometry survives Canvas, Perspective, crop, sub-ROI evaluation, preview, and
export. Legacy mask/custom-blend import remains fail-closed without an exact
mapping.

### Originals, catalogs, and interchange

Define collision, authority, cancellation, and recovery rules for PTP/MTP,
DNG conversion, Smart Previews, HEIC/HEIF, explicit standard-XMP exchange,
read-only catalog conversion, external-editor derived assets, and external
LUT/image/font resources. Ravo must not open a foreign/frozen catalog in place
or create automatic adjacent-XMP writeback as a second live authority.

### Export and background work

Define output geometry, no-enlarge behavior, sharpen order relative to resize,
watermark, colour conversion and frame, reusable preset publication, and a
restartable job that reuses the existing encoder/publication owners.

### Extended library workflows

Define hierarchical keyword semantics, IPTC/location fields, duplicate and
near-duplicate identity, face/privacy policy, camera/lens/date facets, removable
or network storage, and any shared-catalog conflict model. Do not add placeholder
schema tables or empty Studio surfaces before those contracts exist.

### AI architecture and governance

The existing Studio assistant is a desktop HTTP chat/controller surface; it is
not an image-inference or pixel-publication contract. Before AI image work:

- separate conversational intent from model inference and ordinary edit apply;
- define local and remote provider ports without moving business rules into QML;
- distinguish replayable recipe/mask proposals from non-replayable generated
  derived assets;
- expose provider, model, version/weight identity, input revision, settings,
  output hash, and consent/provenance needed for audit;
- define credential storage, visible upload scope, retention, logging, telemetry,
  and training-use policy;
- admit model runtimes, accelerators, and redistributed weights only after
  dependency, licence, package-size, CPU/GPU, memory, cancellation, and fallback
  gates are accepted;
- establish a licensed evaluation corpus and human review protocol covering
  cameras, RAW/raster inputs, lighting, skin tones, genres, and failure cases.

## Non-candidates

Removed GTK/UI ABI, dynamic IOP, OpenCL, and unaccepted leftover algorithm work
remain removed under [MIGRATION.md](MIGRATION.md) and ADR-0106. Ravo does not
advance by recreating empty Lightroom/Capture One modules, silently approximating
Adobe pixels or AI masks, or importing incompatible code without a provenance
and licence decision.
