# ADR-0140: IPTC Extension / additional Core subset (PRO-METADATA)

- Status: Accepted
- Date: 2026-09-04
- Relates: PRO-METADATA in [TODO.md](../TODO.md), R1 in
  [ProductRoadmap.md](../ProductRoadmap.md)
- Extends: [ADR-0124](0124-iptc-core-catalog-subset.md),
  [ADR-0126](0126-catalog-owned-location-fields.md),
  [ADR-0138](0138-xmp-adjacent-keyword-iptc-location-merge.md)
- Supersedes: the ADR-0124/0126 deferral of IPTC Extension / additional Core
  fields for the bounded writable subset below

## Context

ADR-0124/0126 shipped catalog-owned delivery Core (title/description/creator/
copyright) and location text (country/province_state/city/sublocation).
Photographers still need a small IPTC Extension / additional Core slice for
press and delivery workflows—without adopting the entire Extension schema or
reopening dual live authority with adjacent XMP.

## Decision

### Owned writables (bounded subset)

Catalog-owned UTF-8 text fields on `WritableMetadata` / `asset_metadata`:

| Field | IPTC / XMP | Notes |
| --- | --- | --- |
| `headline` | IPTC 2:105 / XMP `photoshop:Headline` | Extension |
| `credit` | IPTC 2:110 / XMP `photoshop:Credit` | Extension |
| `source` | IPTC 2:115 / XMP `photoshop:Source` | Extension |
| `instructions` | IPTC 2:40 / XMP `photoshop:Instructions` | Extension |
| `usage_terms` | XMP `xmpRights:UsageTerms` | Rights / Ext |
| `job_id` | IPTC 2:103 / XMP `photoshop:TransmissionReference` | cheap Core |

Persistence requires **catalog schema v15**: six nullable TEXT columns on
`asset_metadata`. Migration from v14 is additive `ALTER TABLE` only.

### Authority, refresh, interchange

- Catalog SQLite is the sole live authority for these writables.
- `refresh_capture_metadata` must **not** clear, merge, or overwrite them.
- Import may leave them empty; no automatic embedded IPTC import in this tranche.
- Adjacent XMP merge/writeback includes these fields in the ADR-0138 matrix
  classes (fingerprint + present-field patches + export packets).

### Export privacy (ADR-0064)

- `full` / `no-location`: may embed these writables with Core/location/tags.
- `none`: strips public Exif/XMP/IPTC packets entirely.
- `no-location` does **not** strip these non-location fields.
- Original-copy remains exact source bytes and rejects non-`full` modes.

### Multi-selection / CLI / Studio

- Field patches via existing `WritableMetadataPatch` /
  `set_writable_metadata_selection` (one SQLite transaction).
- CLI one-asset get/set gains optional flags for the new fields.
- Studio may expose inspectors when the desktop TU allows; service contract is
  required regardless.

### Validation

- `validate_metadata_field` / `kMetadataFieldMaxLength` apply.
- Export IPTC IIM byte caps follow IIM where defined (headline 256, credit/
  source/instructions 32, job_id/transmission reference 32); `usage_terms` is
  XMP-oriented and may omit from IIM when empty.

## Non-goals (explicit)

- Full IPTC Extension / contact / scene codes / subject codes.
- Faces/people, AI metadata proposals, auto-import of embedded IPTC.
- New privacy modes.
- PRO-PRESENT publishing surfaces.

## Consequences

PRO-METADATA gains a bounded Extension/Core delivery slice on schema v15 with
the same authority/refresh/privacy posture as ADR-0124/0126. ADR-0138 adjacent
fingerprints and packets expand to cover the new fields.

## Rejected alternatives

- Waiting for the entire IPTC Extension before any additional Core/Ext fields.
- Treating embedded source IPTC as live authority beside SQLite.
- Letting capture refresh clobber catalog-only Extension writables.
