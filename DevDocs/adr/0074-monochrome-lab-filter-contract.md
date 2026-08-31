# ADR-0074: Monochrome uses the frozen Lab colour-filter and bilateral base

- Status: Accepted
- Date: 2026-08-29
- Extends: [ADR-0015](0015-migrate-all-non-ui-algorithms.md)

## Context

Ravo's early `amount` control only reduced D50 Lab chroma. The frozen
Monochrome owner instead builds a virtual colour filter in the a*/b* plane,
smooths its response through a scale-aware bilateral base, applies a lightness
envelope and highlight preservation, then publishes a neutral Lab result. The
shortcut was not C18 acceptance.

## Decision

- `ravo.color.monochrome` schema v2 declares `lab_d50`,
  `frozen_monochrome_v2`, filter a*/b* in [-128,128], size 0.5–3, highlights
  0–1, and Ravo mix 0–1. Existing schema-v1 `amount` upgrades to the same mix
  with frozen defaults; zero mix is bit-preserving identity.
- Engine privately converts linear Rec.709 to D50 Lab, evaluates the frozen
  bit-level fast exponential over the colour-distance filter, and feeds that
  lightness plane into the shared deterministic bilateral-grid owner with
  original-pixel sigma 20 and requested range sigma 250. It retains the source
  envelope branches, highlight blend, normalized filter multiplication, and
  zero a*/b* output. Mix blends in Lab before the inverse bridge.
- The shared bilateral lightness primitive is extracted from Retouch's existing
  accepted grid implementation. Retouch continues to use the same owner and
  regression tests; Monochrome adds no copied grid or fallback path.
- Canonical masks use the standard full-frame evaluator and normal mix.
  Conversion/filter/bilateral/output rows and pre-publication are cancellable;
  dimensions, canonical scale, samples, grid bounds, allocation, and output
  finiteness fail before publication. Memory estimates include Lab, filter,
  output, grid, and mask terms.
- Strict XMP import accepts only the exact enabled v2 singleton in 0017 with
  its 16-byte parameter payload and blend-v9 default. The complete document
  remains negative evidence because unrelated old operations are unsupported.
- Recipe/CLI/Catalog/Studio/styles share all five controls. Studio preserves an
  imported canonical mask read-only. The former numeric `monochrome` field
  remains a compatibility alias for mix, not a second algorithm.

## Consequences

C18 is accepted. The old `iop/monochrome.c`, its two exclusive OpenCL kernels
and envelope helper, and darkroom icons are removed. Shared bilateral,
colour-picker, image monochrome-camera flags, demosaic modes, preset categories,
order/module-group/manual names, and fixtures remain with their separate
owners.

## Rejected alternatives

- Keep the chroma-only shortcut. It omits the defining colour filter and
  lightness behavior.
- Reimplement bilateral filtering locally. One deterministic primitive must
  own grid sizing, blur order, cancellation, and resource accounting.
- Infer Monochrome from a camera's monochrome flag. Sensor workflow state and
  this creative output operation are independent.
