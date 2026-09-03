# ADR-0124: Catalog-owned IPTC Core subset

- Status: Accepted
- Date: 2026-09-03
- Relates: PRO-METADATA in [TODO.md](../TODO.md), R1 in
  [ProductRoadmap.md](../ProductRoadmap.md)
- Extends: [ADR-0038](0038-embedded-export-metadata.md),
  [ADR-0064](0064-atomic-metadata-refresh-and-export-privacy.md),
  [ADR-0119](0119-hierarchical-keywords.md)
- Supersedes: the ADR-0119 deferral of “IPTC Core/Ext field tables” for the
  narrow Core writable quartet only (keywords remain ADR-0119)

## Context

Rendered export already embeds Catalog `WritableMetadata` (title, description,
creator, copyright) plus hierarchical keyword display paths as public Exif/XMP/
IPTC packets (ADR-0038/0064/0119). Schema columns for those four strings already
exist on `asset_metadata`. What was still undecided for PRO-METADATA was the
**product boundary**: which IPTC Core fields the catalog owns, how they interact
with capture refresh and export privacy, and how multi-selection edits stay
transactional—without opening location tables, Ext schemas, or dual live
authority with adjacent XMP.

## Decision

### Owned IPTC Core subset

- The catalog-owned IPTC Core subset in this tranche is exactly:
  - `title` (IPTC 2:05 / XMP `dc:title`)
  - `description` (IPTC 2:120 / Exif ImageDescription / XMP `dc:description`)
  - `creator` (IPTC 2:80 / Exif Artist / XMP `dc:creator`)
  - `copyright` (IPTC 2:116 / Exif Copyright / XMP `dc:rights`)
- Hierarchical **keywords** remain the ADR-0119 vocabulary + membership; they are
  the keyword half of delivery metadata and continue to project through
  `asset_tag` into export subject/IPTC 2:25 packets. This ADR does not add a
  second keyword store.
- Persistence stays the existing `asset_metadata` writable columns. **No schema
  version bump** is required for this subset.

### Authority, refresh, and interchange

- Catalog SQLite is the sole live authority for the Core quartet. Adjacent
  source/XMP IPTC is not a second live authority in this tranche.
- `refresh_capture_metadata` continues to rewrite **capture** identity only
  (camera/numeric/time/GPS). It must **not** clear, merge, or overwrite title,
  description, creator, or copyright (catalog-only writables).
- Import may leave Core writables empty. No automatic import of embedded IPTC
  Core into the catalog in this tranche. Adjacent merge/writeback remains
  PRO-INTERCHANGE (ADR-0120 matrix).

### Export embed / omit

- Privacy modes stay ADR-0064; no new mode:
  - `full` and `no-location` may embed the Core quartet and keyword tags under
    the existing ADR-0038 packet rules (IPTC omitted only when all four
    writables are absent and tags are empty).
  - `none` strips public Exif/XMP/IPTC packets entirely (ICC may remain).
  - Original-copy remains exact source bytes and rejects non-`full` modes.
- Location/GPS stripping under `no-location` is unchanged and independent of
  Core writables.

### Multi-selection transactionality

- Multi-selection Core edits publish through CatalogService in **one** SQLite
  transaction with optional `expected_revision` preflight.
- Edits are **field patches**: named Core fields are applied to every selected
  asset; unspecified Core fields stay per-asset. This matches Studio’s single-
  field inspectors without clobbering sibling writables.
- Full-record replace of the four fields remains available for single-asset
  CLI/API (`catalog metadata` get/set).
- On stale revision, missing asset, validation, or SQL failure the whole edit
  rolls back (no partial Core publish across the selection).
- Studio must call the selection API (not per-asset loops outside a catalog
  transaction). CLI may keep one-asset get/set; bulk CLI is optional later.

### Validation

- Field names and UTF-8 bounds continue to use `validate_metadata_field` /
  `kMetadataFieldMaxLength`. Empty present values remain three-state optionals
  as today (absent vs present-empty vs present-nonempty) for export.

## Non-goals (explicit)

- IPTC Extension, contact/location/city/country, scene/subject codes, and other
  Core fields beyond the quartet.
- Catalog-owned location **tables** or camera/lens/date **facets** (still
  PRO-METADATA residual). Capture GPS columns remain ADR-0040/0064 refresh
  territory, not editable IPTC location.
- Faces/people, AI metadata proposals, and adjacent XMP IPTC merge/writeback.
- Schema v13 or new tables for this subset.

## Consequences

PRO-METADATA gains an accepted, narrow IPTC Core ownership contract on top of
existing columns and export serializers. Hierarchical keywords (ADR-0119) plus
this quartet form the delivery-metadata slice photographers need before
location/facets. Create/reopen/backup continue to carry writables via existing
`asset_metadata` recovery triggers. A later tranche may add location/facets
without reopening catalog-vs-sidecar dual authority.

## Rejected alternatives

- Waiting for full IPTC Ext/location/facets before accepting any Core subset.
- Treating embedded source IPTC as live authority beside SQLite.
- Letting capture refresh clobber catalog-only title/creator/copyright.
- Per-asset Studio loops that can publish a partial multi-selection.
- Inventing new privacy modes for Core-only strip.
