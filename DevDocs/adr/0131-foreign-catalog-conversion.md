# ADR-0131: Read-only foreign catalog conversion (Lightroom / Capture One)

- Status: Accepted
- Date: 2026-09-03
- Relates: PRO-INTERCHANGE in [TODO.md](../TODO.md), R2 in
  [ProductRoadmap.md](../ProductRoadmap.md)
- Extends: [ADR-0063](0063-explicit-no-automatic-sidecar-policy.md),
  [ADR-0086](0086-lightroom-crs-interchange.md),
  [ADR-0120](0120-xmp-interchange-conflict-matrix.md),
  [ADR-0122](0122-external-editor-derived-assets.md)
- Supersedes: the PRO-INTERCHANGE deferral of foreign Lightroom/Capture One
  catalog conversion for the owned conversion contract below (in-place open
  remains rejected)

## Context

Photographers need a path from an existing Lightroom Classic or Capture One
catalog into a **new** Ravo catalog without treating the foreign database as a
live Ravo store. ADR-0120 already owns adjacent XMP for single assets. Opening a
foreign `.lrcat` / Capture One session in place would invent a second authority,
risk silent field loss, and couple Ravo schema evolution to vendor formats.

## Decision

### Read-only conversion into a new catalog

- Foreign catalog conversion is always **user-initiated**, **read-only** on the
  source tree, and writes only a **new** Ravo catalog (or an explicitly empty
  destination catalog the user created for the conversion). Ravo never opens,
  migrates in place, or attaches a foreign catalog as the live SQLite store.
- Source originals on disk remain byte-identical. Conversion may Add/Copy into
  the destination catalog under existing ADR-0102/0104 import rules; it must not
  Move or rewrite originals as part of conversion.
- CatalogService owns the conversion job. QML must not parse vendor databases.

### Source kinds (first contract)

1. **Lightroom Classic catalog** — `.lrcat` (+ related helpers the importer
   documents). Recipe mapping reuses ADR-0086 CRS fail-closed rules where the
   foreign row exposes CRS-equivalent fields; unsupported dialect/keys skip that
  look with structured reasons rather than inventing Develop params.
2. **Capture One session/catalog** — explicit session/catalog root the importer
   documents. Unsupported adjusts fail closed the same way.

Other products remain out of scope until a dated ADR names them.

### Mapping authority and loss reporting

- Destination SQLite is the sole live authority after conversion completes.
- Keywords, IPTC Core (ADR-0124), and location labels (ADR-0126) import only
  when the foreign field maps to an owned Ravo field; hierarchical keyword merge
  follows a later adjacency matrix residual and must not invent dual keyword
  trees mid-conversion.
- Every run produces a structured conversion report: imported, skipped,
  unsupported, failed, and per-item reasons. Silent drop of supported fields is
  a defect; unsupported fields are expected and counted.
- Partial completion follows cancel/disconnect semantics of ingest: completed
  destination rows remain; incomplete items are reported; the source foreign
  catalog is untouched.

### Non-goals (explicit)

- In-place open or “attach foreign catalog.”
- Bidirectional sync, watchers, or treating `.lrcat` as a Ravo recovery store.
- Guaranteeing pixel-identical Develop looks vs Lightroom/C1.
- Auto-stack UX for derived pairs (still residual under ADR-0122).
- Shipping the importer implementation in this ADR alone — this accepts the
  product contract; the first Ready code tranche adds CLI/service tests against
  fixtures only (no vendor runtime dependency in the default package).

## Consequences

PRO-INTERCHANGE gains an accepted conversion contract. Implementation may land
as a bounded Ready tranche with fixture catalogs under `Ravo/tests/fixtures/`
without reopening dual-authority debates. AI-03 and HEIC/DNG residuals stay on
their own ADRs.

## Rejected alternatives

- Opening `.lrcat` / C1 sessions as the live catalog.
- Best-effort silent mapping of unsupported Develop modules.
- Requiring Adobe/Phase One libraries in the default Release package for the
  first fixture-backed tranche.
