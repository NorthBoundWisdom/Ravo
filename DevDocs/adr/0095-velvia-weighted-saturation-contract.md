# ADR-0095: Velvia owns the frozen low-saturation weighted colour boost

- Status: Accepted
- Date: 2026-08-30
- Extends: [ADR-0015](0015-migrate-all-non-ui-algorithms.md)

## Context

Ravo already had the frozen per-pixel formula behind a schema-v2 operation,
but Develop reduced it to one `0..1` amount, rewrote saved recipes as schema
v1 with a fixed bias, did not execute canonical masks, and could not import the
sole frozen Velvia history. That partial surface did not satisfy C21.

## Decision

- `ravo.color.velvia` schema v2 declares `linear_rec709`,
  `frozen_velvia_v2`, strength `0..100`, and mid-tones bias `0..1`. Develop
  owns explicit presence, enabled state, both parameters, and an optional
  canonical mask. The former Ravo schema-v1 `amount` maps to
  `strength=amount*100`; CLI field `velvia` remains the same compatibility
  input and zero removes the complete operation.
- The CPU path retains the frozen float order: HSL-style luminance and
  saturation estimate, low-saturation/midtone-bias weight, strength scale,
  per-channel distance from half the other-channel sum, then `[0,1]` clamp.
  Zero strength is bit-preserving identity. No transfer curve, colour-space
  fallback, or non-finite repair is added.
- Canonical masks use the shared full-frame evaluator and normal mix. Invalid
  dimensions, buffer/profile/parameter/sample state, allocation, row or
  pre-publication cancellation fail without changing the caller-owned source
  or publishing a partial result.
- Strict XMP import accepts only the exact enabled version-2 singleton in
  fixture 0063: its eight-byte strength/bias payload, priority-zero unnamed
  state, and blend-v10 default-unmasked envelope. Modified, disabled, masked,
  custom-blend, multi-instance, or other state rejects structurally.
- Recipe, CLI, Catalog, Studio, history, styles, preview, save/reopen, and
  export share the typed v2 state. Studio exposes enable, strength, and bias;
  imported canonical masks are preserved read-only in this panel.

## Consequences

C21 is accepted. The old `iop/velvia.c`, its exclusive `extended.cl` kernel,
and darkroom icons are removed. Shared order, module-group, manual, OpenCL
program, and frozen fixtures remain with their existing owners. CPU remains
the accepted backend; this decision does not add a GPU fallback.

## Rejected alternatives

- Keep the one-slider product surface and fixed bias. It cannot preserve the
  frozen parameter state or round-trip fixture 0063.
- Reimplement the weighting in QML or an export encoder. Velvia is canonical
  linear-RGB recipe math shared by every preview and export path.
