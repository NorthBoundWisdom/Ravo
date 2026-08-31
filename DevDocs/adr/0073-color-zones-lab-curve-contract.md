# ADR-0073: Color Zones is an optional D50 Lab curve operation

- Status: Accepted
- Date: 2026-08-29
- Extends: [ADR-0015](0015-migrate-all-non-ui-algorithms.md)

## Context

Color Equalizer already owns Ravo's default dt-UCS hue-partition workflow.
Frozen Color Zones is a distinct optional Lab/LCh operation: one selected
lightness/chroma/hue axis indexes three independent curves that modify
lightness, chroma, and hue. Treating it as an alias would lose its curve types,
selection behavior, low-chroma hue blend, and strict legacy state.

## Decision

- `ravo.color.colorzones` schema v1 declares `lab_d50`,
  `frozen_colorzones_v3`, the L/C/h selection axis, three ordered 2–20-node
  curves, cubic-spline/Catmull–Rom/monotone-Hermite interpolation per curve,
  and mix strength from -200 through 200.
- Engine privately bridges linear Rec.709 through D50 Lab, builds three
  65,536-entry source-quantized LUTs, selects by `L/100`, `C/128`, or hue, and
  retains the frozen low-chroma lightness/hue blend, `2^(4*Lm)` lightness,
  `2*Cm` chroma, and hue rotation order. Hue selection makes every curve
  periodic. Nonperiodic endpoints stay constant outside the first/last node.
- Curve derivatives reproduce the frozen V2 smooth cubic, Catmull–Rom, and
  monotone paths; periodic monotone uses the source weighted-slope variant.
  Samples are rounded through 16-bit LUT storage and divided by 65,536 before
  lookup interpolation. A strength/interpolation combination that produces a
  non-finite or out-of-[0,1] LUT fails explicitly instead of relying on an
  undefined float-to-unsigned conversion.
- Canonical masks use the existing full-frame evaluator and normal mix. The
  operation owns an additional RGB output plus 768 KiB LUT budget; allocation,
  LUT construction, rows, and pre-publication are cancellable and preserve the
  caller's source on failure.
- Strict XMP import accepts only the exact enabled v5 singleton from 0022,
  including its 520-byte payload, three-node Catmull curves, reserved tail, and
  blend-v9 default. The complete document remains negative because its
  FilmicRGB state has no accepted mapping.
- Recipe/CLI/Catalog/Studio/styles share the operation. Studio creates an
  eight-band monotone identity and edits its three y values, selection,
  interpolation, and strength. Arbitrary imported 2–20-node curves and masks
  are preserved but shown read-only rather than reshaped.

## Consequences

C17 is accepted while Color Equalizer remains the default. The old
`iop/colorzones.c`, its exclusive `colorzones_v3` OpenCL kernel, GTK graph
configuration, and icons are removed. Shared spline/curve, histogram, picker,
order, module-group, and manual owners remain for other queued capabilities.
The old selection-display alpha visualization is presentation-only and is not
a recipe operation or export mode.

## Rejected alternatives

- Map Color Zones onto Color Equalizer. Their colour spaces and curve semantics
  differ.
- Support only eight fixed bands in the recipe. That would discard the frozen
  three-node record and arbitrary canonical curves.
- Clamp unsafe periodic spline output silently. A recipe outside the defined
  LUT range must fail with its cause visible.
