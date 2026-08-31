# ADR-0013: Hot pixels are repaired on an owned Bayer CFA copy

- Status: Accepted
- Date: 2026-08-26
- Relates to: ADR-0009, `DevDocs/TODO_LEGACY_MIGRATION.md` RAW repair

## Context

The frozen `hotpixels.c` operates before demosaic on normalized sensor values.
It reads one CFA frame and writes a separate copy so two candidate pixels do
not affect one another during detection. Ravo caches decoded RAW frames in
`CatalogService`, so mutating that cache would make later recipes depend on
request order.

## Decision

- `ravo.raw.hotpixels` v1 stores strength, threshold and permissive mode.
  `markfixed` is a legacy diagnostic overlay and is not product recipe data.
- The first accepted sensor contract is a complete Bayer 2×2 CFA. Strength
  zero is identity; otherwise each interior site compares its normalized value
  against the four same-colour sites at horizontal/vertical distance two.
- `multiplier=strength/2`; strict mode requires all four neighbours and
  permissive mode requires three. A detected site is replaced by the maximum
  qualifying neighbour, matching the frozen CPU path. The outer two rows and
  columns remain unchanged.
- RAW preprocessing copies `DecodedRaw` once, applies hot pixels before
  highlights and demosaic, then disables all CFA-only operations before the RGB
  recipe stage. Catalog cache keys include every enabled RAW-preprocess
  parameter and operation order.
- X-Trans, monochrome CFA, raster input, incomplete frames and invalid
  black/white levels return structured errors. Processing checks cancellation
  at row boundaries and never publishes a partially mutated frame.

## Consequences

- Interactive preview, full render and export share the same CFA repair while
  the cached decoded frame and original file remain unchanged.
- Synthetic single/pair/edge fixtures, raster/X-Trans rejection and a real
  `mire1.cr2` channel-sum reference define the CPU contract.
- The legacy GTK diagnostic UI, dynamic IOP lifecycle and old preset/XMP ABI
  are not migrated; `legacy/src/iop/hotpixels.c` and its registration are
  retired after automated acceptance.

## Rejected alternatives

- Repair after demosaic: neighbour topology and results differ from the frozen
  RAW algorithm.
- In-place detection on the cached CFA: output would depend on scan/request
  order and contaminate later recipes.
- Route X-Trans through the Bayer offsets: it silently compares different
  colours.
