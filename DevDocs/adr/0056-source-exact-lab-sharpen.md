# ADR-0056: Sharpen uses the frozen scale-aware D50 Lab USM

- Status: Accepted
- Date: 2026-08-29
- Extends: [ADR-0055](0055-colorreconstruction-bilateral-grid-contract.md)

## Context

P2's next Ready operation was `iop/sharpen.c`. The prior Ravo
`ravo.detail.sharpen` implementation was a simplified whole-plane blur and did
not reproduce the frozen separable kernel, scale-dependent radius/sigma,
unchanged borders, or L*-only threshold path. It therefore could not retire the
old owner.

Three frozen XMPs contain actual Sharpen v1 records. All use radius 2, amount
0.5, and threshold 0.5 in an enabled priority-zero unnamed singleton. Two use
the exact v9 default-unmasked blend and one uses the exact v11
default-unmasked blend. `0171-capture-sharpen` is a separate demosaic capture
sharpening owner and is not evidence for this operation.

## Decision

- `ravo.detail.sharpen` schema v2 owns exactly five fields:
  `working_space=lab_d50`, `algorithm=separable_gaussian_usm_v1`, radius 0–99,
  amount 0–2, and threshold 0–100. Existing Ravo schema-v1 values upgrade
  atomically to v2; their simplified pixel response is intentionally replaced
  by the accepted source-backed meaning.
- CPU converts the explicit linear-Rec709 compatibility buffer privately to
  D50 Lab, sharpens L* only, preserves a*/b*, and converts the complete owned
  result back while retaining profile, RAW analysis, and canonical scale.
- Commit multiplies radius by 2.5. The current-pixel radius is
  `min(12, ceil(committed_radius * canonical_scale))`; Gaussian sigma follows
  the frozen expression and retains radius-dependent truncation. Images smaller
  than the full kernel pass through the Lab core unchanged.
- Interior rows use source-order vertical then horizontal convolution. Borders
  remain unchanged. The detail term is nonzero only when
  `abs(L - blur) > threshold`, subtracts the threshold, restores the sign, and
  scales by amount.
- The engine owns one Lab plane, one row scratch, one bounded kernel, and one
  output. Invalid schema/parameters, dimensions, profile, scale, non-finite
  input/output, allocation, and cancellation during input conversion,
  vertical/horizontal rows, output conversion, or pre-publication publish
  nothing.
- Strict legacy import accepts only the three evidenced v1 singleton
  envelopes and exact v9/v11 default-unmasked blends. Disabled, masked,
  custom-blend, multi-instance, malformed, or other-version state rejects.
- CLI, Catalog preview/save/reopen/export, and Studio use the same v2 recipe.
  Studio exposes amount, the source soft radius 0–8 while preserving the hard
  0–99 schema, and threshold. GTK/presets and OpenCL are not ported.

## Consequences

The prior simplified Ravo executor and the old C/OpenCL owners are removed.
Independent scalar tests pin full/downscaled scale, the radius cap, kernel
normalization, threshold, borders, cancellation, resource accounting, and a
real RAW reference. Shared old order/module-group/manual names remain D0.4 and
the three XMPs remain E1 evidence.

## Rejected alternatives

- Keeping the former Ravo whole-plane blur as schema v1 runtime compatibility.
  It would retain an unaccepted second algorithm and make recipe meaning depend
  on an approximation.
- Sharpening RGB channels independently. The frozen operation changes D50 Lab
  L* only.
- Clamping radius to 4.8 or 12 at the recipe boundary. The convolution radius
  is capped at 12, but larger parameter values still change the truncated
  Gaussian sigma.
- Treating demosaic capture sharpening as the same owner. It has different RAW
  stage, data, and acceptance gates.
