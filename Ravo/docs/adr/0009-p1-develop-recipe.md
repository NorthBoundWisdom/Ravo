# ADR-0009: P1 Develop uses one canonical recipe per asset

- Status: Accepted
- Date: 2026-08-24
- Relates to: ADR-0008, SPEC.md P1

## Context

P1 needs non-destructive global edits that survive catalog reopen without
writing into originals or storing QML control state. The engine already has a
versioned recipe/operation contract and a CPU exposure path.

## Decision

- Catalog schema version is 3.
- Each asset may have one active canonical recipe in `asset_recipe`.
  Identity/default edits store no row (`has_edits = false`).
- `DevelopParams` is a service/desktop mapping onto the recipe. Persistence
  is recipe JSON, not slider positions or a UI blob.
- CPU operations for white balance, light, color, rotate and crop execute on a
  linear RGB working buffer. Raster inputs are linearized from sRGB8; RAW
  continues through the existing Bayer path before the same ops.
- Interactive previews include a recipe digest in the cache key. Before/after
  requests the identity preview without changing stored edits.
- Undo/redo is in-session only.

## Consequences

- Opening a v1 or v2 catalog migrates to v3 in one transaction.
- Grid thumbnails may remain the unedited cache; loupe/develop show the edited
  preview and an edit badge.
- Unknown future recipe/operation versions stay fail-fast.

## Rejected alternatives

- Serializing QML or Qt object state into catalog rows.
- Applying QML graphical effects as product edits.
- Building a full version/history graph in P1.
