# ADR-0119: Hierarchical keywords (catalog vocabulary)

- Status: Accepted
- Date: 2026-09-03
- Relates: PRO-METADATA in [TODO.md](../TODO.md), R1 in
  [ProductRoadmap.md](../ProductRoadmap.md)
- Extends: [ADR-0064](0064-atomic-metadata-refresh-and-export-privacy.md)
  (catalog-owned writable metadata and export privacy modes)

## Context

Catalog assets already carry flat pick/rating/colour labels and searchable
comma-separated tags persisted in `asset_tag`. Photographers need nested
keyword vocabularies for cull and delivery without inventing a second live
metadata authority beside SQLite. IPTC Core/Ext fields beyond keyword
membership, GPS/location tables, camera/lens/date facets, faces/people, AI
proposals, and adjacent-XMP writeback remain adjacent PRO-METADATA /
PRO-INTERCHANGE work and are deliberately out of this decision.

## Decision

### Authority and privacy

- The catalog-owned keyword vocabulary and asset↔keyword membership are the
  sole live authority for hierarchical keywords in this tranche.
- Adjacent source/XMP keywords are not a second live authority. This tranche
  does not import, merge, or write back embedded keyword packets. Source
  refresh (`refresh_capture_metadata`) continues to leave keyword membership
  untouched so catalog-only keywords cannot be clobbered by a capture refresh.
- Rendered-export privacy stays ADR-0064: `full` and `no-location` may embed
  the asset's keyword display paths as today's export tags; `none` strips
  them. Original-copy remains exact source bytes. No new privacy mode is
  introduced here.

### Identity, hierarchy shape, and display paths

- Each keyword node has a stable catalog ID (`kwd_` + random hex). Rename and
  reparent preserve membership because assets link by ID, not by path string.
- The vocabulary is a **single-parent tree** (nullable `parent_id` for roots).
  Multi-parent DAGs, synonyms/aliases, and case-folded uniqueness across the
  whole tree are out of scope.
- Sibling uniqueness uses the same trimmed UTF-8 normalization as today's tag
  names (`normalize_tag_name`), with the additional rule that a node name must
  not contain the path separator `|`.
- Display paths are derived: ancestors joined with `|` (Lightroom-style). Path
  strings are a projection for LibraryQuery, Studio/CLI text entry, recovery
  sidecars, and export tags—not the identity of a node.
- Depth is bounded (`kKeywordMaximumDepth`); vocabulary size is bounded
  (`kKeywordMaximumCount`). Oversized or cyclic moves fail closed.

### Membership and multi-selection

- `asset_keyword(asset_id, keyword_id)` stores explicit membership to the named
  node only (assigning a child does not auto-assign ancestors).
- `asset_tag` remains the denormalized, sorted list of display paths for each
  asset so existing LibraryQuery `tag` predicates, recovery sidecars, and
  export tag packets keep working without QML-built SQL.
- Replacing tags by path list resolve-or-creates vocabulary nodes along each
  path inside CatalogService / SqliteCatalogRepository—never in QML.
- Multi-selection tag edits use one catalog transaction with optional
  `expected_revision` preflight. On stale revision, missing asset, validation,
  or SQL failure the whole edit rolls back (no partial membership publish).
  Bulk apply is cancellable at the services boundary where a progress token is
  provided; otherwise the single transaction is atomic.

### Rename, move, and delete

- Rename changes only the node's name (sibling-uniqueness checked) and
  recomputes cached paths for the node and descendants, then rebuilds
  `asset_tag` rows for every asset that members any affected node.
- Move reparents a node (no cycle, depth bound, sibling uniqueness under the
  new parent) with the same path/membership rebuild rules.
- Delete without recursion fails when children exist. Recursive delete removes
  the subtree and its membership rows, then rebuilds affected `asset_tag`
  projections. Orphan vocabulary nodes are allowed (unused keywords may
  remain until explicitly deleted).

### LibraryQuery and Studio/CLI surface

- `LibraryQuery.tag` continues to exact-match a canonical display path in
  `asset_tag` (bounded EXISTS subquery + `asset_tag_name_idx`). Ancestor-or-
  descendant facet expansion is out of scope.
- CLI keeps `catalog tag --asset-id --add/--remove` with path-capable tag
  strings and adds vocabulary commands (`keyword-list`, `keyword-create`,
  `keyword-rename`, `keyword-move`, `keyword-delete`) owned by CatalogService.
- Studio's existing Tags field accepts comma-separated hierarchical paths
  (`Nature|Birds`) through the same services path; no empty keyword pane is
  added.

## Non-goals (explicit)

- IPTC Core/Ext field tables, location/GPS catalog tables, and camera/lens/date
  facets.
- Faces/people, AI keyword proposals (blocked on AI-00).
- Adjacent XMP/sidecar keyword merge matrix and writeback (PRO-INTERCHANGE).
- Multi-parent DAG, synonyms, and QML-built SQL.

## Consequences

Schema v12 adds `keyword` and `asset_keyword`, migrates existing flat
`asset_tag` names to root keywords, and keeps create/reopen/migrate/backup/
restore on the same SQLite authority. Hierarchical keyword edits survive
reopen and verified backup/restore because membership is ID-based inside the
catalog database. A later PRO-METADATA tranche may add IPTC/location/facets
without reopening catalog-vs-sidecar dual authority.

## Rejected alternatives

- Treating path strings as identity (rename would orphan membership).
- Dual live authority with adjacent XMP keyword writeback in this tranche.
- Shipping empty Studio keyword panes or unused tables before the services API.
- Waiting for full IPTC/location/facets before accepting hierarchical keywords.
