# ADR-0042: Canonical ROI scale and recursive Color Harmonizer smoothing

- Status: Accepted
- Date: 2026-08-28
- Extends: [ADR-0035](0035-colorharmonizer-core-contract.md), [ADR-0041](0041-colorharmonizer-smoothing-zero-vertical-slice.md)

## Context

The frozen C14 positive-smoothing branch caches J/chroma/normalized hue plus
two correction channels, computes a scale-dependent sigma, runs the
source-order zero-order recursive Gaussian, then applies the smoothed signals.
The prior Ravo boundary deliberately accepted only smoothing zero because it
had neither a stable pixel-density contract nor an owned recursive filter.

## Decision

- `LinearWorkingBuffer` owns one immutable-value `CanonicalRoiScale`: current
  working-pixel density divided by original input-pixel density. RAW and raster
  creation establish it from oriented original geometry and the current buffer;
  full size is `1`. The proportional `fit_within_max_edge` integer rule is the
  only proof accepted. Unknown or non-proportional geometry remains explicitly
  invalid, never silently becomes `1`; operations and Catalog cached working
  buffers propagate the value unchanged.
- Only positive C14 smoothing consumes this contract. `smoothing == 0` retains
  its accepted bit path even with unknown scale. Positive smoothing with an
  invalid value fails before allocation/publication as
  `invalid_colorharmonizer_roi_scale`.
- Engine-private S2.2 owns the two-channel interleaved signal and scratch under
  RAII. It reproduces `DT_IOP_GAUSSIAN_ZERO` coefficients, forward/backward
  initialization, vertical-then-horizontal expression order, and the frozen
  `dt_gaussian_mean_blur` per-read `[-1e9, +1e9]` clamp. The correction signal
  receives no extra clamp. Sigma is exactly
  `smoothing * fmaxf(1.5f, 8.0f * roi_scale) * fmaxf(1.0f, pull_width)`.
- Positive processing allocates JCH 3c, the sole correction 2c buffer moved to
  S2.2, S2.2 scratch, and output before publication. RAW memory estimation uses
  the S2.2 owner byte calculation plus JCH storage with saturating arithmetic.
  Cancellation is checked before validation, through mapping and recursive
  stages, through apply, and immediately before publication. Non-finite,
  overflow, allocation, cancellation, and output failures publish nothing.
- Recipe/Develop/CLI/Catalog/Studio use the same schema-v1 parameter and engine
  path; Studio only presents/forwards the bounded `0..2` numeric intent. The
  strict legacy-XMP adapter remains evidence-bound: frozen records 12/13 have
  zero smoothing, and synthetic positive legacy payloads reject as
  `unsupported_legacy_colorharmonizer_unevidenced_smoothing`.

## Consequences

Canonical recipes can use positive smoothing synchronously in all supported
clients without adding a public Gaussian ABI, new thread model, GPU/OpenCL
path, mask graph, auto-detection, picker, histogram, harmony guide, or C15.
General C14 mask/presentation decisions and atomic legacy-owner retirement
remain unfinished; the frozen legacy owner is not removed.

## Rejected alternatives

- Infer scale in Color Harmonizer from current dimensions or substitute scale
  `1` when original geometry is unknown.
- Use a kernel Gaussian, box blur, generic image library, or unbounded/no-clamp
  recurrence in place of the frozen helper.
- Relax strict legacy import merely because canonical Ravo recipes support a
  positive value.
