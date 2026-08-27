# ADR-0035: Bound Color Harmonizer to the smoothing-zero CPU core

- Status: Accepted
- Date: 2026-08-27

## Context

The frozen `legacy/src/iop/colorharmonizer.c` version-1 owner pulls dt-UCS
hues toward predefined or custom harmony nodes, protects neutral colours, and
can adjust saturation around the winning node. Its full path can additionally
smooth the two correction channels with a recursive Gaussian whose radius
depends on pixelpipe ROI scale. The old GTK graph, auto-detection and picker,
blend/mask UI, OpenCL path, and dynamic module lifecycle surround that CPU
owner but are not serialized colour mathematics.

The complete frozen fixture set contains two real version-1 Color Harmonizer
history records in 0176. Record 12 is the post-initialization default. Record 13
changes the rule to split complementary, anchor hue to the float value 0.55,
pull strength to 0.82, pull width to 1.84, and the four node-saturation values
to 1.26, 0.18, 1.52, and 1.0. Both records have `smoothing=0`. They are exact
parameter and CPU-fixture evidence only: Ravo does not yet claim strict legacy
import of either record or of the complete 0176 document.

The engine-private S1.1 D50 Lab and S1.2 dt-UCS bridges plus the immutable S2.1
720-step RYB lookup and harmony-node geometry are already accepted. They are
sufficient to reproduce the default-unmasked, smoothing-zero CPU path without
inventing a general mask graph, a Gaussian/ROI contract, or a public colour-
science API.

## Decision

- `ravo.color.colorharmonizer` schema v1 has exactly 17 required flat fields:
  `working_space=profile_linear_rgb_d50`,
  `algorithm=dt_ucs_harmony_v1`, `rule`, `anchor_hue`, `pull_strength`,
  `neutral_protection`, `pull_width`, `custom_hue_0` through `custom_hue_3`,
  `num_custom_nodes`, `node_saturation_0` through `node_saturation_3`, and
  `smoothing`. Rule names cover all nine frozen predefined geometries plus
  `custom`. Every numeric field is finite and float-representable; hue,
  strength, protection, width, node-count, node-saturation, and smoothing use
  their frozen hard bounds.
- The accepted CPU boundary is only an enabled, unmasked operation with
  `smoothing == 0`. A mask fails with structured unsupported state. Any
  positive smoothing value fails with
  `unsupported_smoothing_requires_recursive_gaussian`; it never silently
  becomes zero or runs an approximate blur.
- Each finite input RGB channel is clipped with the frozen `fmaxf(value, 0)`
  order, transformed by the declared profile matrix to XYZ D50, and passed
  through the private source-order D50/CAT16-D65/dt-UCS JCH bridge. Predefined
  rules use the immutable S2.1 hue tables and node geometry; custom rules use
  two through four ordered canonical hues. The strict winning-node attraction,
  circular shift, per-node saturation, and pull width are unchanged.
- Neutral protection is cubed before multiplication by `0.03F`. The source
  addition order is preserved for the chroma denominator, hue pull, wrap,
  saturation scale, inverse dt-UCS conversion, and profile-matrix inverse.
  `color_harmonizer.cpp` builds with contraction disabled (`-ffp-contract=off`
  or `/fp:strict`); no FMA, reassociation, clamp, transfer curve, or non-finite
  repair is added.
- Dimensions, RGB buffer length and overflow, declared RGB matrix profile,
  matrix finiteness/invertibility, parameters, and all input/output samples are
  validated before publication. Cancellation is observed before work, across
  validation and output rows, and before return. Success owns separate RGB and
  profile storage, preserves exact profile state, and shares the immutable
  exposure-analysis snapshot. Failure leaves the borrowed input unchanged and
  publishes no partial result. The smoothing-zero operation needs one output-
  sized buffer and no operation-specific analysis or mutable global state.
- Fixed bit goldens for the two 0176 parameter states must match both an
  independent source-order scalar oracle and production. Perturbations that
  omit the negative clip or replace cubic neutral protection prove the oracle
  detects meaningful drift. All nine predefined rules, custom node counts two
  through four, canonical dispatch, cancellation, ownership, and structured
  failures remain direct contract tests.

## Consequences

Ravo now has one strict recipe identity and one CPU implementation for the
bounded Color Harmonizer core. This is not a complete C14 vertical slice. It
does not provide strict legacy XMP import, Develop state, CLI/Catalog/Studio
consumers, cache persistence, nonzero smoothing, canonical ROI scale, the S2.2
recursive Gaussian, masks, presentation behavior, or legacy owner retirement.
Those tranches require separate authorization after the current feature-
convergence pause. `legacy/src/iop/colorharmonizer.c` and its registration
therefore remain frozen evidence and recoverable old ownership.

The accepted default-unmasked boundary is not blocked by the general mask
graph, but it also does not weaken that graph's gate: every mask or other
unowned presentation state remains structured unsupported. C15 and S2.2 are
not authorized by this decision. No Windows/Linux execution evidence or GPU
contract is claimed.

## Rejected alternatives

- Treat both 0176 records or the complete document as imported compatibility:
  no strict decoder or full-history mapping exists yet.
- Coerce positive smoothing to zero or use a generic blur: the frozen recursive
  Gaussian depends on a separately owned two-channel and ROI-scale contract.
- Use RGB/HSL nearest-hue math, a shortened rule set, mutable lookup tables, or
  existing UI approximations: each changes the frozen dt-UCS/RYB owner.
- Reorder float expressions, permit contraction, or repair extended/non-finite
  results: the source-derived bit goldens intentionally distinguish those
  changes.
- Retire the legacy owner after the core-only tranche: importer, product
  consumers, nonzero smoothing, authority, and retirement gates remain open.
