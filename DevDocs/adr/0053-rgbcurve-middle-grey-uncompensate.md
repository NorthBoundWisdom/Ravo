# ADR-0053: RGB curve middle-grey uncompensate uses the working matrix

- Status: Accepted
- Date: 2026-08-29
- Extends: [ADR-0052](0052-legacy-rgbcurve-contract.md)

## Context

ADR-0052 imported leftover rgbcurve v1 only when `compensate_middle_grey` was
0. Fixture `0060-rgbcurve-indep` stores independent monotone-hermite nodes with
that flag set. Leftover remaps each node through
`dt_ioppr_uncompensate_middle_grey`: Lab L* = x·100, a=b=0, then the pipe
working-profile XYZ→RGB matrix. Ravo linear working buffers already carry
`matrix_to_xyz_d50`.

## Decision

- `compensate_middle_grey` is a canonical boolean on `ravo.color.rgbcurve`.
  Leftover v1 0/1 maps to that flag. Identity 2-point `(0,0)-(1,1)` still
  omits the operation.
- At LUT generation, when the flag is set, each node coordinate is converted
  with the current working buffer's inverted D50 RGB→XYZ matrix, matching
  leftover Lab→XYZ D50 (`0.9642, 1, 0.8249`) then XYZ→working RGB. Missing or
  singular matrices reject with `unsupported_rgbcurve_middle_grey_profile`.
- Authored leftover nodes need not start at x=0. Curve evaluation still clamps
  outside the first and last node, matching leftover `CurveDataSample`.

## Consequences

`0060` imports as independent `ravo.color.rgbcurve` with middle-grey
compensation. The conversion follows the live working profile (default linear
Rec.709 in Studio/CLI raster tests; leftover `0060` colorin is Rec.2020).
Nonlinear ICC TRC uncompensate is outside Ravo linear working buffers.

## Rejected alternatives

- Baking Rec.709 or Rec.2020 at import time. Leftover remaps at process time
  from the current work profile.
- Keeping `0060` rejected. It is the frozen rgbcurve fixture.
