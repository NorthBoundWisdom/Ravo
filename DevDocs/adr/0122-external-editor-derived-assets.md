# ADR-0122: External-editor derived asset round-trip (first slice)

- Status: Accepted
- Date: 2026-09-03
- Relates: PRO-INTERCHANGE in [TODO.md](../TODO.md), R2 in
  [ProductRoadmap.md](../ProductRoadmap.md)
- Extends: [ADR-0105](0105-asset-versions-stacks-and-survey.md),
  [ADR-0120](0120-xmp-interchange-conflict-matrix.md),
  [ADR-0121](0121-ai-architecture-privacy-provenance.md),
  [ADR-0028](0028-original-copy-publication-contract.md),
  [ADR-0032](0032-encoded-byte-publication-contract.md)

## Context

ADR-0120 deferred external-editor round-trips as too large for the XMP conflict
matrix. Photographers still need to open a catalog original in Photoshop,
Affinity, or another editor and bring the returned pixels back without mutating
the original RAW/raster, inventing a hidden Ravo renderer, or treating editor
output as a second live authority.

ADR-0105 virtual copies share one original URI. Editor output is a **different
file**, so it cannot be a virtual copy. ADR-0121 already requires non-replayable
pixels to publish as an immutable derived asset/version with provenance.

## Decision

### Initiation and authority

- Registration is an explicit CatalogService / CLI command
  (`catalog editor-register`, `catalog editor-show`). No Studio auto-watch, no
  import-time attachment, no background job that mutates the catalog.
- **Ravo never launches or embeds an external editor or hidden renderer** in this
  slice. The user runs the editor; Ravo only registers the returned file.
- SQLite remains the sole live edit authority for catalogued assets. Provenance
  JSON under `{catalog}.ravo/external-editor/` is catalog-owned metadata, not an
  interchange dialect other products write.

### Derived asset publication

- Editor output becomes a **new catalog asset** with its own `normalized_uri`
  (distinct file). It is not an ADR-0105 virtual copy (`source_asset_id` /
  shared URI). Linking to the source is via provenance, not schema v11 version
  ordinals.
- Default publication copies the editor output into the catalog-owned store
  `{catalog}.ravo/derived/<source-asset-id>/<content-sha256-prefix>-<filename>`
  with atomic no-replace semantics, then imports that path. Optional
  `--destination` may choose another directory; Add-in-place of an arbitrary
  path without a catalog-owned copy is out of scope for this slice.
- After a successful register, Develop/export/history on the derived asset are
  ordinary catalog operations on that new asset. The source asset recipe is
  unchanged.

### Originals stay byte-identical

- Before copy/import, fingerprint the source original (SHA-256 + size + mtime)
  and verify it still matches the catalogued identity (`size_bytes` /
  `mtime_unix_ms` / `content_fingerprint`).
- After successful publication, re-fingerprint the source original. Any mismatch
  fails closed with `source_mutated_during_register` (or preflight
  `source_identity_mismatch`) and leaves no new catalog row when possible.
- The register path never opens the source original for write.

### Provenance contract (`ravo.external-editor.derived/v1`)

Catalog-owned file
`{catalog}.ravo/external-editor/<derived-asset-id>.json` records at least:

- `schema` / `schema_version`
- `derived_asset_id`, `source_asset_id`
- observed catalog revision and source recovery generation
- source original path fingerprint (sha256, size, mtime)
- derived path fingerprint (sha256, size, mtime)
- `editor_id` (required opaque product id, e.g. `photoshop`)
- optional `editor_version`
- `registered_unix_ms`
- `derived_path`

Missing provenance on a derived asset id is `not_found` for `editor-show`.

### Timeout, cancel, conflict / fail-closed

- Cancellation is owned by the caller `CancellationToken`. Checked before copy,
  before import, and before provenance write. Cancel mid-flight leaves no catalog
  row; a copied-but-unimported derived file is removed on cancel/failure when
  Ravo created it in the catalog-owned store.
- There is no hidden timeout daemon in this slice; callers cancel when their
  timeout elapses.
- Fail closed (structured reasons, no partial catalog asset) when:
  - source asset missing;
  - editor output missing / not a regular file;
  - editor output is the same filesystem object as the source original;
  - destination already exists (no-replace);
  - destination URI already cataloged;
  - stale `--revision`;
  - empty / invalid `--editor`;
  - source identity mismatch or source mutation during register;
  - unsupported / undecodable editor output (ordinary import fail-closed).

### First Ready slice scope

In scope:

- Explicit register of one editor output file as a new derived catalog asset
  with provenance.
- Explicit show of provenance by derived asset id.
- CLI surface; unit/service tests proving originals unchanged.

Out of scope (deferred; not silently approximated):

- Launching, scripting, or embedding external editors / round-trip watchers.
- Auto-stacking or Gallery UX for derived pairs (operators may stack manually).
- Rewriting the source recipe from editor output; treating TIFF/PSD layers as
  Develop ops.
- Backup/restore packaging of `{catalog}.ravo/derived/` byte trees beyond the
  existing catalog-owned sidecar posture (document residual until a backup ADR
  extends excludes/includes).
- AI-05 generative fill (shares derived-asset ideas but stays under ADR-0121).

## Consequences

Photographers can re-import editor output as a first-class derived asset without
mutating originals or inventing a second authority. Virtual copies remain
same-URI grades. Launching editors and richer UX stay later work.

## Rejected alternatives

- Overwriting the original or writing editor pixels into the source recipe.
- Treating editor output as an ADR-0105 virtual copy (URI collision / wrong
  semantics).
- Auto-launching Photoshop/Affinity/GIMP from Ravo (hidden renderer).
- Silent last-write-wins if the destination path already exists or is cataloged.
- Watcher-based auto-register of adjacent `*-edit.tif` files.
