# ADR-0057: Dehaze owns source-linear dark-channel and guided-filter processing

- Status: Accepted
- Date: 2026-08-29
- Extends: [ADR-0056](0056-source-exact-lab-sharpen.md)

## Context

P2's next Ready operation was `iop/hazeremoval.c`. The prior Ravo
`ravo.effect.dehaze` was a constant-airlight per-pixel approximation. It did not
estimate ambient light or image depth, use scaled dark-channel windows, refine
transmission with the frozen RGB guided filter, or retain the source stage.

Frozen XMP evidence contains two enabled priority-zero unnamed singleton
records: v1 strength/distance 0.2 with a v9 default-unmasked blend, and v2
strength 0.9, distance 0.8, adaptive false with a v13 default-unmasked blend.
The older v1 document contains a mask history and remains a full-document
negative.

## Decision

- `ravo.effect.dehaze` schema v2 owns exactly five fields:
  `working_space=source_linear_rgb`,
  `algorithm=dark_channel_guided_v4`, strength -1–1, distance 0–1, and
  adaptive window scaling. Existing Ravo v1 `amount` upgrades to strength with
  distance 0.2 and adaptive true; its approximation is not retained.
- Dehaze runs after RAW demosaic and before profile-gamma/Input Color on the
  declared camera-matrix/source-linear RGB buffer. It becomes part of RAW
  preprocess/cache identity and is disabled before the reusable working buffer
  reaches ordinary RGB recipe dispatch. Encoded raster input rejects
  structurally rather than running the scene-linear algorithm on nonlinear
  samples.
- CPU computes the local RGB dark channel, 95% hazy and bright quantiles,
  deterministic ambient RGB and characteristic haze distance. It builds the
  strength-weighted transition map, applies source-order box maximum then
  minimum, and refines it with the frozen 3-channel guided-filter covariance
  solve.
- Adaptive windows use current/original pixel density: dark radius
  `2 + ceil(4*scale)` and guided radius `3 + ceil(6*scale)`. Non-adaptive uses
  full-scale radii. Guided filtering uses bounded 512-pixel tiles with the
  frozen rounded `3*w` overlap, Kahan box means, Cramer's-rule solve and
  singular fallback.
- Output clamps only the minimum transmission to
  `clamp(exp(-distance*distance_max), 1/1024, 1)` and applies the frozen
  atmospheric equation per channel. There is no constant-airlight fallback,
  clipping, or non-finite repair.
- RAW memory preflight includes full transition planes and bounded guided tile
  statistics. Invalid parameters, dimensions, source profile/scale, ambient
  population/light, non-finite values, allocation, and cancellation during
  dark-channel, selection, transition, guided statistics/solve, output, or
  pre-publication publish nothing.
- Strict import accepts only the evidenced v1/v2 singleton envelopes and exact
  v9/v13 default-unmasked blends. Disabled, masked, custom-blend,
  multi-instance, malformed, or other-version state rejects.
- CLI, Catalog and Studio share strength/distance/adaptive persistence,
  preview, close/reopen and export. GTK preview-pipe cache and OpenCL are not
  ported.

## Consequences

The prior Ravo approximation and old Haze Removal owner/kernel/pixmap are
removed. The generic frozen `common/guided_filter*`, `box_filters*`, and
`guided_filter.cl` remain because blend, rasterfile, cacorrectrgb and other
unaccepted consumers still own them; Ravo's private bounded implementation
does not authorize that shared deletion.

## Rejected alternatives

- Constant airlight or a per-pixel dark-channel shortcut. It omits the two
  source algorithms that define F11.
- Running after Input Color or on encoded raster RGB. The evidenced module
  order and description require source-linear scene RGB.
- Reusing the existing single-plane simplified guided filter. Haze Removal
  requires the RGB covariance matrix, Kahan box means, and singular solve.
- Keeping the old preview-pipe ambient cache. Ravo owns a complete immutable
  frame per request and its existing revision/cancellation lifecycle.
