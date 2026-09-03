# ADR-0126: Catalog-owned IPTC location Core fields

- Status: Accepted
- Date: 2026-09-03
- Relates: PRO-METADATA in [TODO.md](../TODO.md), R1 in
  [ProductRoadmap.md](../ProductRoadmap.md)
- Extends: [ADR-0038](0038-embedded-export-metadata.md),
  [ADR-0040](0040-capture-time-gps-metadata.md),
  [ADR-0064](0064-atomic-metadata-refresh-and-export-privacy.md),
  [ADR-0119](0119-hierarchical-keywords.md),
  [ADR-0124](0124-iptc-core-catalog-subset.md)
- Supersedes: the ADR-0124 deferral of “catalog-owned location tables” for the
  narrow IPTC location text quartet only (GPS capture columns remain ADR-0040;
  camera/lens/date facets remain undecided)

## Context

ADR-0124 accepted catalog ownership of the IPTC Core delivery quartet
(title/description/creator/copyright). Photographers still need **human location
labels** (country / province-state / city / sublocation) that survive reopen,
export privacy, and multi-select edits. Capture GPS (`CaptureLocation`) already
exists as import/refresh identity (ADR-0040/0064) and must not become a second
editable location authority. A full location table, map UI, or facet index is
out of scope for this tranche.

## Decision

### Owned location Core subset

- The catalog-owned location subset in this tranche is exactly four UTF-8 text
  fields on `WritableMetadata` / `asset_metadata`:
  - `country` (IPTC 2:100 / XMP `photoshop:Country`)
  - `province_state` (IPTC 2:95 / XMP `photoshop:State`)
  - `city` (IPTC 2:90 / XMP `photoshop:City`)
  - `sublocation` (IPTC 2:92 / XMP `Iptc4xmpCore:Location`)
- **No catalog-editable lat/lon.** Capture GPS columns and `CaptureLocation`
  remain ADR-0040 refresh territory. This ADR does not add a location table,
  place ID, or map feature.
- Persistence requires **catalog schema v13**: four nullable TEXT columns on
  `asset_metadata`. Migration from v12 is additive `ALTER TABLE` only.

### Authority, refresh, and interchange

- Catalog SQLite is the sole live authority for the location quartet. Adjacent
  source/XMP IPTC location is not a second live authority in this tranche.
- `refresh_capture_metadata` continues to rewrite **capture** identity only
  (camera/numeric/time/GPS). It must **not** clear, merge, or overwrite
  `country`, `province_state`, `city`, or `sublocation`.
- Import may leave location fields empty. No automatic import of embedded IPTC
  location into the catalog in this tranche. Adjacent merge/writeback remains
  PRO-INTERCHANGE.

### Export embed / omit (ADR-0064 privacy)

- Privacy modes stay `full` / `no-location` / `none`; no new mode:
  - `full` may embed the location quartet under ADR-0038 packet rules (IPTC/XMP)
    alongside the Core delivery quartet and tags.
  - `no-location` preserves Core delivery writables, camera/time, and tags while
    removing **every** location signal: capture GPS **and** the catalog location
    quartet (no Country/State/City/Sublocation in Exif/XMP/IPTC).
  - `none` strips public Exif/XMP/IPTC packets entirely (ICC may remain).
  - Original-copy remains exact source bytes and rejects non-`full` modes.

### Multi-selection transactionality

- Multi-selection location edits reuse CatalogService
  `set_writable_metadata_selection` / `WritableMetadataPatch` in **one** SQLite
  transaction with optional `expected_revision` preflight (same contract as
  ADR-0124).
- Edits are **field patches**: named location fields apply to every selected
  asset; unspecified writable fields stay per-asset.
- On stale revision, missing asset, validation, or SQL failure the whole edit
  rolls back.
- Studio must call the selection API (not per-asset loops). CLI may keep
  one-asset get/set with optional location flags.

### Validation

- Field names and UTF-8 bounds use `validate_metadata_field` /
  `kMetadataFieldMaxLength`. Export IPTC byte caps:
  city/sublocation/province_state 32, country 64 (IPTC IIM limits).

## Non-goals (explicit)

- Catalog-editable GPS / lat-lon / altitude authoring.
- Location tables, place vocabularies, map/search facets, or LibraryQuery
  location filters.
- Camera/lens/date facets (still PRO-METADATA residual).
- IPTC Extension beyond this location quartet; faces/people; AI location
  proposals; adjacent XMP location merge/writeback.
- PRO-PRESENT map UI.

## Consequences

PRO-METADATA gains an accepted, narrow catalog-owned location text contract on
schema v13. Capture GPS remains capture-only. Export `no-location` becomes the
single privacy switch for both GPS and human location labels. A later tranche
may add facets or interchange merge without reopening dual authority.

## Rejected alternatives

- Treating capture GPS as the editable “location” surface for photographers.
- Waiting for map UI / facet indexing before accepting text location fields.
- Letting capture refresh clobber catalog-only location labels.
- Inventing a fourth privacy mode for “strip GPS but keep city”.
- Per-asset Studio loops that can publish a partial multi-selection.
