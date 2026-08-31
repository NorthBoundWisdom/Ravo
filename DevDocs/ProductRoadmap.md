# Deferred product decisions

This document contains only cross-layer capabilities whose product contract is
not ready for execution. It is not a release checklist or a second migration
queue.

- Current P0/P1 release evidence: [TODO_PHOTO_MANAGEMENT.md](TODO_PHOTO_MANAGEMENT.md)
- Gallery performance evidence and gated candidates:
  [TODO_GALLERY_PERFORMANCE.md](TODO_GALLERY_PERFORMANCE.md)
- Legacy absorption and retirement: [TODO_LEGACY_MIGRATION.md](TODO_LEGACY_MIGRATION.md)
- Accepted ownership and invariants: [ARCHITECTURE.md](ARCHITECTURE.md)
- Legacy boundary and capability status: [MIGRATION.md](MIGRATION.md)

A topic leaves this file only after a dated decision defines its user outcome,
owner, persisted contract, failure/cancellation behavior, and validation gate.
Implementation status belongs in code, tests, or the applicable TODO.

## Local adjustment expansion

Ravo already owns the bounded canonical mask graph, normal mix, overlay, and
Studio authoring described by ADR-0043–0045. Still undecided:

- additional blend modes with a named operation consumer and exact ROI/order
  semantics;
- picker or histogram-assisted authoring that keeps graph mathematics out of
  QML;
- additional masked operations whose geometry survives Canvas, Perspective,
  crop, and sub-ROI evaluation without implicit coordinate conversion.

Legacy mask/custom-blend import remains fail-closed until an accepted mapping
exists.

## Originals, catalogs, and interchange

- Managed copy or move needs collision, cross-volume publication, source-change,
  rollback, cancellation, and recovery-generation policy before schema or UI
  work begins.
- Legacy catalog import needs a bounded source-version matrix and a read-only
  conversion artifact. Ravo must not open or migrate a frozen catalog in place.
- Adjacent standard-XMP writeback needs conflict and authority rules. It must not
  overwrite an existing sidecar or become a second live authority beside
  SQLite.
- External LUT/image/font resources need versioned lookup, immutable content
  identity, and deterministic missing/corrupt behavior.

## Export and background work

Foreground typed batch export is accepted. Durable background export jobs,
restart recovery, and reusable export-option presets remain undecided. PNG pHYs
and TIFF multipage masks stay with their format-specific migration owners rather
than entering a generic export rewrite.

## Extended library workflows

Collections and smart search, stacking/versions, duplicate-content detection,
face/GPS workflows, and removable or network catalogs each require explicit
privacy, indexing, offline/conflict, persistence, cancellation, and recovery
contracts. Do not add placeholder tables or empty Studio surfaces.

## Non-candidates

The non-algorithm UI/ABI/OpenCL/data leftovers listed in
[MIGRATION.md](MIGRATION.md) are removed rather than redesigned. Remaining image
algorithms stay in migration scope under ADR-0015 and enter execution only
through [TODO_LEGACY_MIGRATION.md](TODO_LEGACY_MIGRATION.md).
