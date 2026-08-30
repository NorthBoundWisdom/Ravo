# ADR-0094: Adaptive Profile Denoise

- Status: Accepted
- Date: 2026-08-31
- Extends: [ADR-0085](0085-interchange-ready-grading-tools.md),
  [ADR-0089](0089-exact-interactive-prefix-cache.md)

## Context

The first `ravo.detail.denoiseprofile` implementation retained the accepted
Y0U0V0 edge-aware à-trous wavelet structure, but every image and render scale
used one fixed generic variance profile and assumed that the stabilized finest
band had unit noise. That assumption left visible residual grain when the
generic profile underestimated a capture. The public Radius value was
serialized and shown in Studio but never entered the engine, so distinct
Radius edits could produce byte-identical previews. Tests covered only a
synthetic change and determinism, not noise reduction, edge retention, control
separation, scale, cancellation, or memory.

Static study of current Darktable confirms the Y0U0V0 variance-stabilized
BayesShrink and edge-aware wavelet foundations, and records the generic
Poisson baseline as `a=0.0001`, `b=0`. ART/RawTherapee use image-derived noise
analysis when a fixed profile is insufficient. vkdt normalizes wavelet bands by
their expected noise and retains edge shielding. These projects are design
evidence only; Ravo neither links them nor claims pixel equivalence.

## Decision

- `ravo.detail.denoiseprofile` remains recipe schema v1 before exposure, with
  Luminance strength, relative Chroma strength, and Radius. The operation stays
  global and uses the existing Y0U0V0 variance-stabilized edge-aware à-trous
  decomposition and soft BayesShrink synthesis.
- The generic transform baseline is `a=0.0001`, `b=0`. After the first wavelet
  decomposition, each Y0/U0/V0 channel estimates its actual stabilized noise
  from median absolute detail divided by the Gaussian MAD constant. Sampling is
  deterministic and bounded to 2^18 values per channel; estimates are bounded
  to 0.25–4.0. The finest edge-aware decomposition is repeated with the
  calibrated joint noise scale, and the calibrated channel scales propagate to
  coarser bands through the frozen à-trous variance factor.
- BayesShrink subtracts calibrated noise variance from measured band variance
  and uses `8 * noise_variance / signal_sigma`, retaining the accepted default
  wavelet force. Radius 1 is the source-default spatial response. Radius
  0.5–8 scales the à-trous sampling dilation and progressively weights coarser
  thresholds, so it controls spatial reach instead of being ignored.
- A full denoised candidate is reconstructed into owned scratch. The source and
  candidate delta is separated with linear-Rec.709 luminance: Luminance mixes
  the neutral delta, while `Luminance * Chroma` mixes the remaining colour
  delta. Zero strength is exact identity; Chroma zero cannot silently remove
  colour variation.
- Canonical ROI scale selects the wavelet bands visible at the current render
  size. Parameters, dimensions, scale, input, wavelet detail, and output must be
  finite and valid. Cancellation can stop every long pass. The caller-visible
  image swaps to the owned result only after all processing and validation
  succeeds.
- RAW memory preflight includes four RGB float scratch planes and the greater
  of the bounded MAD sample or per-scale coordinate tables. Preview contract
  v10 invalidates prior fixed-profile pixels; recipe schema stays v1 because
  stored control intent and bounds do not change.

## Consequences

Existing Profile Denoise recipes intentionally render differently. Noise
calibration follows the actual working pixels instead of assuming one camera
response, Radius has an observable spatial effect, and Luminance/Chroma retain
separate, monotonic mix ownership. The extra calibrated finest-band pass costs
bounded CPU time and no persistent state.

This does not introduce a camera noise-profile database, neural denoiser,
NLMeans mode, RAW-domain replacement, or silent algorithm fallback. A future
camera-profile owner may provide a stronger prior, but it must still retain the
finite, scale, resource, cancellation, and atomic-publication gates here.

## Rejected alternatives

- Increase the fixed threshold or default slider value. That hides one sample
  while retaining incorrect capture/scale assumptions.
- Keep Radius serialized but document it as unused. Machine-visible controls
  must either have defined behavior or be removed through a schema/UI decision.
- Suppress sharpening to disguise noise. Sharpening cannot remove pre-existing
  luminance or chroma noise and remains a separate operation.
- Copy a third-party camera database or denoiser into Ravo. That would add a
  second owner and update lifecycle without a versioned dependency contract.
