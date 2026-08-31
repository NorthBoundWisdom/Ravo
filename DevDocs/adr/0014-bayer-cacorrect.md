# ADR-0014: Bayer chromatic aberration keeps the frozen two-pass fit

- Status: Accepted
- Date: 2026-08-26
- Relates to: ADR-0013, `DevDocs/TODO_LEGACY_MIGRATION.md` RAW repair

## Context

The fixture-backed `cacorrect.c` is a pre-demosaic Bayer algorithm derived
from RawTherapee. Its result is not a fixed red/blue radial displacement: each
iteration estimates local optical sample shifts from green/color differences,
rejects outlier blocks, fits a full-image polynomial and interpolates corrected
color differences. The optional avoid-color-shift path derives blurred factors
from the original CFA.

## Decision

- `ravo.raw.cacorrect` v1 stores `iterations` in `[1,5]` and
  `avoid_color_shift`. A zero Develop value means the operation is absent.
- The C++20 CPU owner preserves 128×128 tiles, 16-pixel overlap, directional
  green interpolation, high/low-pass weights, 3×3 block median, 4×4 polynomial
  fit with the frozen linear fallback, ±3.99 shift clamp and the two-stage
  color-difference interpolation. Iteration state feeds the next pass.
- Avoid-color-shift retains the original non-green samples, applies the frozen
  ratio bounds, Gaussian factor smoothing and final red/blue compensation.
- The input is normalized with explicit camera white-balance coefficients and
  converted back to Ravo's owned `DecodedRaw` copy only after the whole task
  succeeds. The cached frame and original file remain immutable.
- The operation is Bayer-only, checks cancellation during tiles, Gaussian rows
  and publication, and reports insufficient signal, singular fits, invalid CFA,
  non-finite output and memory budget failure structurally.
- The RAW memory estimator owns all preprocess allocations. Signed tile indices
  remain signed through mirrored-border calculations; conversions occur only
  after bounds are established.

## Consequences

- RAW preview/render/export share one hotpixels → highlights → cacorrect →
  demosaic order and one preprocess cache key.
- Default two-pass `mire1.cr2`, the frozen `0084-cacorrect` five-pass
  avoid-shift parameters, unsupported inputs, cancellation, memory and catalog
  reopen are automated contracts.
- `legacy/src/iop/cacorrect.c` and its CMake registration are retired after
  those automated contracts passed.
- `cacorrectrgb` remains a distinct unaccepted post-demosaic owner; no behavior
  is inferred or deleted with this item.

## Rejected alternatives

- Constant R/B scaling or one radial coefficient: it drops the tile statistics
  and polynomial fit.
- Mutating the cached CFA between previews: results become request-order
  dependent.
- Treating `cacorrectrgb` as equivalent: it has a different stage and no frozen
  fixture in the current corpus.
