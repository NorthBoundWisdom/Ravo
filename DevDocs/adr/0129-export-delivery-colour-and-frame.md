# ADR-0129: Export delivery colour conversion and output frame

- Status: Accepted
- Date: 2026-09-03
- Relates: PRO-EXPORT remaining work in [TODO.md](../TODO.md)
- Extends: [ADR-0019](0019-explicit-output-color-profiles.md),
  [ADR-0022](0022-final-display-packing-and-diagnostic-disposition.md),
  [ADR-0070](0070-canvas-and-output-frame-contract.md),
  [ADR-0117](0117-export-box-sharpen-presets-and-restartable-jobs.md),
  [ADR-0127](0127-export-delivery-text-watermark.md),
  [ADR-0032](0032-encoded-byte-publication-contract.md)
- Supersedes: the ADR-0117 / ADR-0127 deferral of colour-conversion/frame for
  **export delivery** only (Develop recipe colour/frame ownership unchanged)

## Context

Photographers often need a **delivery-only** output colour space (for example
Adobe RGB for a print lab) and/or an ADR-0070 decorative Frame without baking
those choices into the Develop recipe, soft-proof inspector, or history.
ADR-0019 already owns recipe `ravo.color.output`; ADR-0070 already owns recipe
`ravo.output.frame` after Output Color / optional Dither and before packing.
ADR-0127 showed the ExportOptions pattern for delivery watermark. PRO-EXPORT
still listed colour-conversion/frame as undecided.

Post-pack colour conversion would fight ADR-0022 packing and ICC embedding.
QML colour or frame mathematics would invent a second owner.

## Decision

### ExportOptions ownership (not Develop recipe mutation)

- Delivery colour and delivery frame are optional fields on **ExportOptions** /
  export presets / restartable jobs (ADR-0117). They never rewrite stored recipe
  bytes, history, or Develop UI state.
- When absent/disabled, export matches today’s recipe-only path (recipe output
  colour and recipe Frame still apply when present).
- Original-copy rejects any delivery colour or frame option (exact source bytes).

### Delivery colour conversion

- Reuses the **ADR-0019** `OutputColorParams` contract (built-in or file ICC,
  four rendering intents, black-point compensation). Soft-proof and gamut-check
  are **not** delivery modes: when the export colour option is enabled,
  CatalogService forces `proof_mode=off` for that render only.
- CatalogService applies the override by cloning the in-memory recipe used for
  the export render and replacing (or inserting) the enabled `ravo.color.output`
  boundary. The engine remains the sole colour-transform owner; QML forwards
  profile/intent strings only.
- Because colour conversion must precede packing, delivery colour runs **inside**
  the Develop export render, before resize completion is packed. Long-edge/box
  fit remains the existing CatalogService `output_width`/`output_height` path.

### Delivery output frame

- Reuses the **ADR-0070** `FrameParams` / `frozen_borders_v4` mathematics and
  engine stage. No second border helper, encoder padding, or QML layout math.
- CatalogService applies delivery frame on packed `RenderedExportImage` pixels
  after resize (already reflected in the rendered size) and after optional
  output sharpen, and **before** delivery watermark and encode. Border thickness
  is therefore relative to the delivery dimensions, not the pre-resize master.

### Fixed pipeline order (rendered JPEG/PNG/TIFF)

1. Develop export render (optional delivery **colour** override of
   `ravo.color.output`; recipe Frame / recipe watermark still apply if present;
   long-edge/box fit via render output size)
2. Optional ExportOptions **output sharpen** (ADR-0117)
3. Optional ExportOptions **delivery frame** (this ADR)
4. Optional ExportOptions **delivery watermark** (ADR-0127)
5. Encode → metadata embed → atomic no-replace publication

Skipping an optional step does not reorder the rest. Delivery colour cannot move
after packing. Delivery frame cannot move after watermark.

### First Ready tranche

- ExportOptions `output_color` and `frame` fields, preset/job JSON, CatalogService
  apply, Studio/CLI controls, and equality / no-recipe-mutation tests on `main`.
- Soft-proof-as-export, logo/SVG frames, and silent recipe baking remain out of
  scope.

## Consequences

PRO-EXPORT colour-conversion/frame is no longer blocked on an open decision.
Delivery colour shares ADR-0019 transforms; delivery frame shares ADR-0070
layout; neither becomes a second live Develop authority.

## Rejected alternatives

- Post-encode ICC relabel or QML colour conversion.
- Encoder-only padding instead of ADR-0070 Frame.
- Baking delivery colour/frame into Develop recipe/history by default.
- Applying delivery frame before sharpen or after watermark.
- Treating soft-proof/gamut-check as a delivery export colour mode.
