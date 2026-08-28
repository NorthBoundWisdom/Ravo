# ADR-0051: Leftover RGB levels maps to `ravo.color.rgblevels`

- Status: Accepted
- Date: 2026-08-29
- Extends: [ADR-0050](0050-ashift-rotation-and-export-scale.md)

## Context

P2 after geometry import is RGB curves/levels. Leftover `iop/rgblevels.c` is a
working-space RGB point operator: linked or independent channels, a 65536-entry
gamma LUT, and optional preserve-colors via `dt_rgb_norm`. Frozen fixtures
`0054-rgblevels-linked` and `0055-rgblevels-indep` store v1 44-byte payloads.
Auto-levels and the GTK picker write those stops into history; they are not a
separate pixel-time owner.

`ravo.core.tonecurve` is a different leftover (`tonecurve.c`) and must not
absorb RGB levels.

## Decision

- Leftover rgblevels v1 unmasked singleton history maps to
  `ravo.color.rgblevels`. `autoscale` 0/1 becomes `linked`/`independent`.
  `preserve_colors` 0–6 use the same names as the migrated tone-curve
  preserve-colors contract. Identity stops `0 / 0.5 / 1` add no operation.
- Same-instance later history wins. `0055` therefore imports as independent.
  `multi_priority != 0`, masks, and other versions reject.
- CPU math copies leftover `_compute_lut` / `process`:
  `inv_gamma = 10^((grey-mid)/delta)`, LUT `pow(i/65536, inv_gamma)`, clip
  below black, extrapolate above white. Linked+preserve uses canonical
  `rgb_norm` already used by tone curve. Independent or `none` apply per
  channel. Auto-levels/picker UI are not imported; stops in the blob are the
  source of truth.
- Studio authors the same parameters through Develop Light. Catalog/CLI persist
  the operation through recipe round-trip.

## Consequences

Linked and independent leftover RGB-levels histories become one canonical
operation. ICC work-profile luminance in leftover `dt_rgb_norm` is not a second
matrix; preserve-colors luminance uses the existing canonical `rgb_norm`.
Duplicate named instances stay unsupported. T2 `rgbcurve` remains a separate
owner.

## Rejected alternatives

- Folding RGB levels into `ravo.core.tonecurve`. The leftover modules, payloads,
  and LUT construction differ.
- Replaying every history snapshot as stacked operations. Leftover pixelpipe
  uses the current instance; `0055` is last-write independent.
- Porting the GTK auto-levels picker as a live engine pass. History already
  contains the computed stops.
