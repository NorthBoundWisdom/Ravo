# ADR-0055: Color Reconstruction owns the full-frame D50 Lab bilateral grid

- Status: Accepted
- Date: 2026-08-29
- Extends: [ADR-0054](0054-legacy-rawdenoise-contract.md)

## Context

P2's next Ready operation was `iop/colorreconstruction.c`. It recovers chroma
near clipped highlights by splatting non-clipped D50 Lab samples into a spatial
and lightness bilateral grid, blurring that grid, and slicing replacement a*/b*
values while retaining the input L*. The operation needs the whole attached
frame; the old owner explicitly prohibited tiling because a partial
neighbourhood changes the result.

Frozen fixture `0052-color-reconstruction` contains the only actual history
record across all 158 XMPs: one enabled v3, priority-zero, unnamed,
default-unmasked instance with threshold 60, spatial extent 300, range extent
10, hue approximately 0.66, and saturated-colour precedence.

## Decision

- `ravo.color.colorreconstruct` schema v1 owns exactly seven fields:
  `working_space=lab_d50`, `algorithm=bilateral_grid_v3`, threshold 50–150,
  spatial extent 0–1000, range extent 0–50, hue 0–1, and precedence
  `none|chroma|hue`.
- The operation runs on the explicit linear-Rec709 compatibility working
  buffer immediately before Output Color. Engine-private D50 conversion
  surrounds the frozen Lab algorithm; no ICC handle crosses the boundary.
- CPU uses deterministic row-major splatting, the frozen in-place 5-tap
  x/y/lightness blur, trilinear slicing, the 95%-threshold blend ramp, and
  source-order chroma/hue weights. It preserves L*, the profile, immutable RAW
  analysis, and canonical ROI scale.
- Spatial extent is measured in original-input pixels. The immutable canonical
  scale converts it to the current full-frame pixel density. Unknown or
  non-proportional geometry is a structured failure; Ravo does not guess a
  scale or process independent tiles.
- One owned bilateral grid and one owned output are budgeted before RAW render.
  Invalid dimensions, buffer/profile/scale, non-finite input/output,
  allocation, and cancellation during splat, each blur pass, slice, or
  pre-publication publish nothing and leave the input unchanged.
- Strict XMP import accepts only the evidenced v3 singleton and its exact v10
  default-unmasked blend. The exact built-in RAW blend tuples present in 0052
  may be absorbed. Disabled, masked, custom-blend, multi-instance, malformed,
  or other-version state rejects structurally.
- Develop, CLI numeric intents, Catalog preview/save/reopen/export, and Studio
  use the same recipe and CPU owner. Historic blend modes, GTK caching/UI, and
  OpenCL are not product contracts.

## Consequences

The 0052 document imports without inventing unsupported history. Studio exposes
enable, precedence, threshold, spatial/range extent, and hue controls. The old
C owner, registration, exclusive OpenCL program, and pixmap retire with this
acceptance. Shared old order/manual names and the read-only fixture remain
separate D0.4/E1 owners.

## Rejected alternatives

- A neighbourhood-average or RGB highlight tint. Neither reproduces the
  bilateral Lab propagation.
- Tile-local reconstruction. It changes the surrounding colour population and
  violates the frozen no-tiling contract.
- Reusing RAW CFA highlight reconstruction. `ravo.raw.highlights` runs before
  demosaic and has different sensor mathematics and unsupported states.
- Porting the old preview-grid cache or OpenCL kernels. Ravo's existing task,
  cache, and CPU correctness owners remain authoritative.
