# Deferred product decisions

This document contains only cross-layer capabilities whose product contract is
not ready for execution. It is not a release checklist or a second migration
queue.

- Current P0/P1 release evidence: [TODO_PHOTO_MANAGEMENT.md](TODO_PHOTO_MANAGEMENT.md)
- Gallery performance evidence and gated candidates:
  [TODO_GALLERY_PERFORMANCE.md](TODO_GALLERY_PERFORMANCE.md)
- Ranked Lightroom/Capture One user-outcome gaps (not independently ready):
  [TODO_PRO_WORKFLOW.md](TODO_PRO_WORKFLOW.md)
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

Add/Copy/Move from a local scanned root is accepted (ADR-0102). Bounded rename
templates and a byte-verified second-copy tree are accepted by ADR-0104 without
catalog schema changes. Still undecided ingest expansions, each needing
collision, cancellation, and original-safety rules before schema or UI work:

- PTP/MTP camera-card ingest;
- DNG conversion and Smart Previews;
- HEIC/HEIF still photography, and whether video is ever in scope.

Also undecided:

- Legacy catalog import needs a bounded source-version matrix and a read-only
  conversion artifact. Ravo must not open or migrate a frozen catalog in place.
- Adjacent standard-XMP writeback needs conflict and authority rules. It must not
  overwrite an existing sidecar or become a second live authority beside
  SQLite.
- External-editor round-trip needs a new raster asset or version, a wait/cancel
  contract, and must not mutate the original RAW.
- External LUT/image/font resources need versioned lookup, immutable content
  identity, and deterministic missing/corrupt behavior.

## Export and background work

Foreground typed batch export is accepted. Still undecided: long-edge or box
resize, output sharpen relative to resize/watermark, durable background export
jobs with restart recovery, and reusable export-option presets. PNG pHYs and
TIFF multipage masks stay with their format-specific migration owners rather
than entering a generic export rewrite.

## Extended library workflows

Named manual collections and smart `LibraryQuery` sets are accepted
(ADR-0103). Virtual copies, collapsed stacks with a pick, and Survey N-up
cull are accepted (ADR-0105). Automatic RAW+JPEG pairing on import is not.
Still undecided: duplicate-content detection, hierarchical keywords and IPTC
depth, face/GPS workflows, selection-wide Develop sync, dual-monitor viewers,
and removable or network catalogs. Each remaining topic requires explicit
privacy, indexing, offline/conflict, persistence, cancellation, and recovery
contracts. Do not add placeholder tables or empty Studio surfaces. ADR-0059
session filters are not a smart-collection substitute.

## Non-candidates

The non-algorithm UI/ABI/OpenCL/data leftovers listed in
[MIGRATION.md](MIGRATION.md) are removed rather than redesigned. Remaining image
algorithms stay in migration scope under ADR-0015 and enter execution only
through [TODO_LEGACY_MIGRATION.md](TODO_LEGACY_MIGRATION.md).
