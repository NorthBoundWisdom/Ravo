# ADR-0052: Leftover RGB curve maps to `ravo.color.rgbcurve`

- Status: Accepted
- Date: 2026-08-29
- Extends: [ADR-0051](0051-legacy-rgblevels-contract.md)

## Context

P2 after RGB levels is leftover `iop/rgbcurve.c`. It is a working-space RGB
curve with linked/independent channels, up to 20 monotone-hermite nodes, a
65536 LUT, exponential extrapolation, and optional preserve-colors. Frozen
fixture `0060-rgbcurve-indep` stores a gzipped v1 516-byte payload with
`compensate_middle_grey=1`. That flag remaps nodes through the pipe work
profile Lab uncompensate (`dt_ioppr_uncompensate_middle_grey`).

`ravo.core.tonecurve` remains a different leftover (`tonecurve.c`).

## Decision

- Leftover rgbcurve v1 unmasked singleton history maps to
  `ravo.color.rgbcurve` when every used channel is monotone-hermite, node counts
  are 2–20, nodes are finite unit-interval and strictly increasing in x, and
  `compensate_middle_grey` is 0. Identity `(0,0)-(1,1)` adds no operation.
- `autoscale` 0/1 becomes `linked`/`independent`. Preserve-colors 0–6 use the
  same names as RGB levels. CPU LUT construction reuses the canonical monotone
  hermite evaluator plus leftover unbounded exponential extrapolation.
  Linked+preserve uses canonical `rgb_norm`. Independent or `none` apply per
  channel.
- `compensate_middle_grey` is owned by [ADR-0053](0053-rgbcurve-middle-grey-uncompensate.md).
  Catmull-Rom and cubic-spline interpolators reject.

## Consequences

Compensate-off leftover RGB-curve histories become one canonical operation.
Studio can persist imported recipes through Develop round-trip; a dedicated
curve editor is later P2 UI work.

## Rejected alternatives

- Folding RGB curve into `ravo.core.tonecurve`. Payloads, colour space, and
  linked-channel behaviour differ.
