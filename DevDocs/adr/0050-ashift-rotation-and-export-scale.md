# ADR-0050: Ashift rotation-only import and export final scale

- Status: Accepted
- Date: 2026-08-29
- Extends: [ADR-0049](0049-legacy-crop-box-contract.md)
- Extended by: [ADR-0113](0113-studio-export-long-edge.md)

## Context

P2 after orientation/crop is resize/output dimensions/straighten. Studio already
authors `straighten_degrees` (-45..45) and Catalog export already resamples
through `ExportRequest.max_edge`. Leftover `ashift` is a full perspective
module (shift, shear, automatic crop, line detection). Leftover `finalscale` is
a hidden dummy that only scales the pixelpipe into the export ROI. Those two
must not be treated as the same capability.

## Decision

- Leftover ashift v4/v5 unmasked singleton history is imported only when the
  first four floats (rotation, vertical shift, horizontal shift, shear) are
  finite and shift/shear are identity. Rotation maps to
  `ravo.geometry.straighten`. Zero rotation is identity.
- Non-zero lens shift or shear rejects with
  `unsupported_legacy_ashift_perspective`. Rotation outside ±45° rejects with
  `unsupported_legacy_ashift_rotation_range`. This is not complete G6 ALG.
- Leftover `finalscale` has no history and a dummy parameter. Catalog/CLI
  export `max_edge` is the Ravo output-size owner (G7 product subset). Pixel
  aspect `scalepixels` (G3) stays later.

## Consequences

Rotation-only leftover straighten histories become one canonical operation.
Perspective fixtures such as `0110-perspective-bilinear` stay rejected until
G6 lens geometry is implemented. Export dimension tests already cover G7
`max_edge`.

## Rejected alternatives

- Mapping every ashift history to straighten. Shift/shear change geometry.
- Porting LSD/RANSAC automatic fit in this tranche.
- Treating leftover `finalscale` as a recipe operation. It never enters history.
