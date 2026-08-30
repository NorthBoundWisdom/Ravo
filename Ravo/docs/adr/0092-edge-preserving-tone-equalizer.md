# ADR-0092: Edge-preserving Tone Equalizer

- Status: Accepted
- Date: 2026-08-30
- Extends: [ADR-0089](0089-exact-interactive-prefix-cache.md),
  [ADR-0091](0091-monotonic-whites-blacks-response.md)

## Context

The first `ravo.core.toneequal` implementation inserted the five Studio
controls sparsely into a nine-value parameter vector, duplicated Blacks, left
the intervening targets at identity, and solved an under-determined inverse for
eight RBF factors. A smooth five-control edit could therefore oscillate between
the authored zones. Its detail mask filtered linear RGB L2 energy with hidden
defaults equivalent to a broad radius and unit regularization. In dark regions
that regularization dominated the signal variance: fine stem texture collapsed
into one correction while correction leaked across the much brighter flower
boundary as a visible halo.

The frozen Darktable implementation establishes nine internal exposure bands
and edge-aware detail-preservation modes. Static study of ART's adapted Tone
Equalizer additionally demonstrates exposure-domain luminance filtering and
scale-aware guided regularization. These sources are design evidence only;
Ravo has no production dependency on either implementation and does not claim
pixel equivalence with them.

## Decision

- Recipe operation `ravo.core.toneequal` remains schema v1, global, unmasked,
  before Sigmoid, and controlled by Blacks, Shadows, Midtones, Highlights, and
  Whites. Studio retains its -2 to +2 range; the existing machine-visible
  -4 to +4 validation interval remains accepted, with the resulting common RGB
  correction bounded to 0.25–4.0.
- The five controls own correction EV at -8, -6, -4, -2, and 0 EV. Adjacent
  control averages fill the intervening -7, -5, -3, and -1 EV targets, so all
  nine accepted bands have an authored value. Each target converts to positive
  linear gain. A normalized Gaussian RBF with sigma sqrt(2) builds an 80,001
  sample LUT over [-8, 0] EV at 10,000 samples per stop. Normalization makes the
  identity exact and removes the unstable pseudo-inverse.
- Per-pixel assignment retains the accepted linear-RGB L2 energy. Energy is
  converted to bounded log2 EV before mask filtering. A self-guided filter uses
  radius 240 in original-image pixels, multiplied by immutable canonical ROI
  scale, and regularization 0.04 EV². Low-contrast variation is removed from
  the assignment mask so one correction preserves that local contrast; strong
  exposure boundaries retain their own mask value and do not produce broad
  bright or dark halos.
- One positive correction multiplies R, G, and B, preserving channel ratios.
  Invalid dimensions, missing canonical scale, non-finite input, non-finite or
  overflowing output, cancellation, and allocation failure remain structured
  failures. Input is never published partially.
- The guided-filter implementation reuses coefficient planes. Tone Equalizer's
  peak beyond ordinary working RGB is five float planes plus the LUT, and the
  RAW preflight memory estimate owns those bytes. Preview contract v9
  invalidates pixels produced by the former curve and mask; the stored recipe
  schema does not change because the five control intents do not change.

## Consequences

Existing recipes containing Tone Equalizer intentionally render differently.
The five controls now produce a smooth nine-band response, small dark-region
structure remains visible after a shadow lift, and high-contrast subject edges
do not acquire the former guided-mask halo. Full and downscaled previews use
the same original-pixel mask radius.

Tone Equalizer remains luminance-selective rather than semantic or spatially
selective. It cannot distinguish a subject and background that have the same
exposure without an explicit canonical mask; no hidden subject detector or
fallback mask is introduced by this correction.

## Rejected alternatives

- Replace the internal curve with only the five visible controls. That would
  violate the accepted nine-band capability.
- Retain sparse identity targets and tune the pseudo-inverse. The instability
  follows from the authored discontinuities and under-determined solve, not one
  tolerance constant.
- Blur linear luminance more aggressively or clamp the result. That hides the
  halo but further destroys dark local contrast.
- Add user-facing radius, feathering, or third-party compatibility modes in
  this correction. They would expand recipe and UI scope without being needed
  for the stable default contract.
