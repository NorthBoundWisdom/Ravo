# ADR-0138: XMP adjacent keyword/IPTC/location merge matrix

- Status: Accepted
- Date: 2026-09-04
- Relates: PRO-INTERCHANGE / PRO-METADATA in [TODO.md](../TODO.md), R2 in
  [ProductRoadmap.md](../ProductRoadmap.md)
- Extends: [ADR-0120](0120-xmp-interchange-conflict-matrix.md),
  [ADR-0119](0119-hierarchical-keywords.md),
  [ADR-0124](0124-iptc-core-catalog-subset.md),
  [ADR-0126](0126-catalog-owned-location-fields.md)
- Supersedes: the ADR-0120 deferral of “keyword / IPTC / location packet merge”

## Context

ADR-0120 shipped explicit adjacent-XMP status/import/export for **CRS recipe
fields** with a fail-closed conflict matrix. Hierarchical keywords (ADR-0119),
IPTC Core title/description/creator/copyright (ADR-0124), and the location
quartet country/province_state/city/sublocation (ADR-0126) remain catalog-owned
and were deliberately left out of that first slice so adjacent sidecars could
not become a second live authority. Photographers still need user-initiated
merge of those field groups under the same matrix.

## Decision

### Authority

- Catalog SQLite remains the **sole live authority** for hierarchical keywords,
  IPTC Core, and the location quartet. Adjacent `.xmp` is a conversion artifact
  the user points at via `catalog xmp-status|xmp-import|xmp-export` only.
- No watcher, import auto-attach, Studio auto-sync, or silent writeback.
- `refresh_capture_metadata` continues to rewrite capture identity only and must
  **not** clear, merge, or overwrite catalog-owned Core/location writables or
  keyword membership (existing ADR-0119/0124/0126 rule; unchanged here).

### Conflict matrix (same classes as ADR-0120)

| Class | Meaning |
| --- | --- |
| `missing` | No sidecar at the resolved path. |
| `identical` | Baseline present and both catalog and sidecar fingerprints still match it. |
| `catalog-newer` | Baseline present; catalog fingerprint changed; sidecar unchanged. |
| `sidecar-newer` | Baseline present and sidecar changed while catalog matches; **or** no baseline and catalog has no recipe edits **and** no catalog-owned adjacent metadata/tags while a sidecar exists. |
| `both-changed` | Baseline present and both sides changed; **or** no baseline while catalog has recipe edits **or** catalog-owned adjacent metadata/tags and a sidecar exists. |

Resolve modes remain `--resolve abort|catalog|sidecar` with the same
import/export allow rules as ADR-0120 (default abort; no silent last-write-wins).

### Fingerprints and exchange baseline

- **Catalog fingerprint** extends ADR-0120 with a deterministic
  `metadata_sha256` over catalog-owned adjacent fields for the asset:
  Core quartet, location quartet, and sorted keyword display paths (`asset_tag`
  / ADR-0119 projection). Recipe SHA-256 and recovery generation stay as today.
- **Sidecar fingerprint** remains file SHA-256 + size + mtime (whole sidecar).
- Exchange baseline under `<catalog>.ravo/xmp-exchange/<asset-id>.json` bumps to
  **version 2** and stores `metadata_sha256`. Version-1 baselines load with an
  empty-metadata hash so pre-0138 baselines remain readable; a catalog that
  later gains adjacent metadata correctly becomes `catalog-newer`.

### Field groups in status / import / export

- `xmp-status` reports CRS parse state (ADR-0120) **and** adjacent-metadata parse
  state (`metadata_parse_ok` / `metadata_parse_reason`) for the sidecar.
- Supported sidecar packets for this tranche:
  - Core: `dc:title`, `dc:description`, `dc:creator`, `dc:rights`
  - Location: `photoshop:Country`, `photoshop:State`, `photoshop:City`,
    `Iptc4xmpCore:Location`
  - Keywords: `lr:hierarchicalSubject` as an RDF Bag of `|`-separated display
    paths (ADR-0119). When that element is absent, flat `dc:subject` Bag items
    are accepted only as **single-segment** root paths.
- **Fail closed** on unsupported hierarchical keyword shapes (nested/structured
  Lightroom/MWG keyword trees, multi-valued path separators we do not own,
  invalid ADR-0119 paths). Import never invents a second vocabulary authority
  and never partially applies when keyword parse fails.
- Import apply (when resolve allows):
  - CRS look when the sidecar carries supported CRS (ADR-0086/0120);
  - field-patch Core/location for elements present in the sidecar;
  - replace keyword membership when `lr:hierarchicalSubject` or `dc:subject` is
    present (resolve-or-create via existing `set_tags` / ADR-0119 paths).
  - If CRS is present but unsupported, the whole import fails (no metadata-only
    partial apply). Metadata-only sidecars without CRS may import metadata.
- Export writes one adjacent XMP that includes the CRS PV2012 look subset **and**
  the catalog Core/location/keyword packets above. Original media bytes stay
  untouched. Catalog remains authority for unmapped Develop features (listed in
  `omitted_catalog_fields` as today).

### Out of scope

- IPTC Extension / additional Core fields beyond ADR-0124 + ADR-0126.
- Automatic adjacent watch/merge; treating recovery JSON as interchange XMP.
- Vendor `.lrcat` readers; external-editor launch (ADR-0122 residuals).

## Consequences

Explicit XMP interchange covers recipe CRS and the catalog-owned keyword/Core/
location groups under one matrix without dual live authority. Unsupported
keyword shapes stay visible and refuse apply. Capture refresh and export privacy
contracts for those fields are unchanged.

## Rejected alternatives

- Silent last-write-wins or dual-write mirrors for keywords/IPTC/location.
- Treating flat `dc:subject` as hierarchical without `lr:hierarchicalSubject`
  when items contain `|` or nested structures (fail closed / single-segment only).
- A second SQLite store or sidecar-owned vocabulary for live membership.
