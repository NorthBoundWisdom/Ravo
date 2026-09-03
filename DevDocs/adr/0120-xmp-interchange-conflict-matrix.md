# ADR-0120: Adjacent XMP interchange conflict matrix (first slice)

- Status: Accepted
- Date: 2026-09-03
- Relates: PRO-INTERCHANGE in [TODO.md](../TODO.md), R2 in
  [ProductRoadmap.md](../ProductRoadmap.md)
- Extends: [ADR-0063](0063-explicit-no-automatic-sidecar-policy.md),
  [ADR-0064](0064-atomic-metadata-refresh-and-export-privacy.md),
  [ADR-0086](0086-lightroom-crs-interchange.md),
  [ADR-0097](0097-catalog-recovery-sidecars-and-verifiable-backups.md),
  [ADR-0119](0119-hierarchical-keywords.md)

## Context

PRO-INTERCHANGE needs user-initiated XMP exchange without adjacent sidecars
becoming a second live authority. ADR-0063/0097 keep SQLite as the sole live
edit owner and treat adjacent `.xmp` as non-attached on import. ADR-0086 already
maps Camera Raw Settings (CRS) XMP onto Develop params fail-closed. ADR-0119
keeps hierarchical keywords catalog-owned and out of automatic XMP writeback.
This ADR accepts the **conflict matrix** and the first Ready CLI/service slice
for explicit recipe-field CRS XMP import/export with preflight.

## Decision

### Initiation and authority

- Every adjacent-XMP read or catalog→XMP write is an explicit CatalogService /
  CLI command (`catalog xmp-status`, `catalog xmp-import`, `catalog xmp-export`).
  No watcher, no import auto-attach, no silent writeback, no Studio auto-sync.
- After a successful import apply, SQLite remains the sole live edit authority.
  The sidecar is a conversion artifact the user pointed at—not a dual-write
  mirror. Catalog-owned recovery JSON under `<catalog>.ravo/sidecars/` is
  unchanged by this tranche (ADR-0097).
- Original media bytes are never rewritten. Export writes only the chosen XMP
  path (default: adjacent `<stem>.xmp` beside the original). Import never
  mutates the sidecar file.

### Fingerprints

- **Catalog fingerprint:** asset recovery `generation` plus SHA-256 of the
  canonical stored recipe JSON (`load_recipe_json` / `serialize_recipe`).
- **Sidecar fingerprint:** SHA-256 of the sidecar file bytes plus
  `FileIdentity` size and mtime.
- **Exchange baseline:** catalog-owned file
  `<catalog>.ravo/xmp-exchange/<asset-id>.json` recording the fingerprints from
  the last successful explicit import or export for that asset. It is not an
  adjacent authority and is never read as interchange input by other products.

### Conflict classes

| Class | Meaning |
| --- | --- |
| `missing` | No sidecar at the resolved path. |
| `identical` | Baseline present and both catalog and sidecar fingerprints still match it. |
| `catalog-newer` | Baseline present; catalog fingerprint changed; sidecar unchanged. |
| `sidecar-newer` | Baseline present and sidecar changed while catalog matches; **or** no baseline and catalog has no edits (`has_edits == false`) while a sidecar exists. |
| `both-changed` | Baseline present and both sides changed; **or** no baseline while catalog has edits and a sidecar exists (no shared sync point—fail closed). |

Ambiguous multi-sidecar layouts (`.xmp` and `.XMP` for the same stem that are
not the same file) remain `kConflict` / `import_sidecar_ambiguous`.

### Outcomes (no silent last-write-wins)

- `xmp-status` is read-only: returns class, fingerprints, paths, and whether a
  CRS parse would fail closed.
- `xmp-import` / `xmp-export` default to `--resolve abort`. Non-`identical`
  work requires an explicit resolve:
  - `--resolve sidecar` — import applies CRS look into the catalog recipe;
    export refuses to overwrite a sidecar that is newer or both-changed.
  - `--resolve catalog` — export overwrites/creates the sidecar from the
    catalog look; import refuses to clobber catalog-newer or both-changed.
- Unsupported CRS dialect/keys fail closed with structured
  `unsupported_*` reasons (ADR-0086). Import never partially applies.
- Successful import/export refreshes the exchange baseline to the post-op
  fingerprints. Cancellation leaves catalog and sidecar as they were; a failed
  baseline write after a successful catalog commit reports
  `catalog_committed=true` and `exchange_baseline_pending=true` without rolling
  back the recipe (same durability posture as ADR-0097 recovery publication).

### First Ready slice scope

In scope:

- Explicit status/import/export of **standard CRS recipe-field XMP** for one
  catalog asset.
- Conflict preflight + structured JSON errors.
- CLI surface above; preserve original bytes.

Out of scope (deferred; not silently approximated here):

- Keyword / IPTC / location packet merge (ADR-0119 remains catalog-owned until a
  later merge matrix).
- **External-editor derived assets / versions** — large enough for a later ADR;
  do not invent a derived-asset lifecycle in this slice.
- In-place foreign Lightroom/Capture One catalog open or migration.
- Automatic adjacent watch/merge; darktable history XMP as a catalog apply
  dialect (existing `recipe import-xmp` file→recipe path remains separate).

## Consequences

Photographers can exchange CRS looks with an explicit conflict matrix while
SQLite stays the only live authority. Unsupported fields stay visible.
External-editor round-trips and foreign-catalog conversion remain blocked until
their own dated ADRs exist.

## Rejected alternatives

- Silent last-write-wins on adjacent XMP. Two authorities without a matrix.
- Treating catalog-owned recovery JSON as interchange XMP.
- Schema-coupled baseline inside SQLite for this first slice (filesystem
  exchange baseline under `<catalog>.ravo/` is enough and matches ADR-0097's
  catalog-owned derived store pattern).
- Auto-import of adjacent XMP on catalog import (forbidden by ADR-0063/0097).
