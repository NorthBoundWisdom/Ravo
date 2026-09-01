# ADR-0043: Canonical mask graph foundation

- Status: Accepted
- Date: 2026-08-28
- Extends: [ADR-0035](0035-colorharmonizer-core-contract.md), [ADR-0041](0041-colorharmonizer-smoothing-zero-vertical-slice.md), [ADR-0042](0042-colorharmonizer-canonical-roi-recursive-smoothing.md)
- Extended by: [ADR-0108](0108-masked-color-balance-rgb.md),
  [ADR-0109](0109-masked-exposure.md),
  [ADR-0110](0110-masked-rgb-curve.md),
  [ADR-0111](0111-masked-tone-curve.md),
  [ADR-0112](0112-masked-light-controls.md)

## Context

Ravo previously persisted only the schema-v1 `all` mask and rejected every
operation attachment. Frozen `develop/blend*`, `develop/blends/*`,
`develop/masks/*`, and `iop/mask_manager.c` show that masks are recipe/history
data and that evaluation is ordered, ROI-aware CPU work; they do not authorize
Ravo to revive the old GTK form manager, mutable pixelpipe state, historic
blend-mode matrix, or legacy-XMP compatibility.

S3 needs a bounded canonical foundation before later M1 consumers can depend on
it. It also needs a real end-to-end consumer now: a schema no client evaluates
is not a product contract.

## Decision

- `ravo_recipe` owns immutable, typed mask nodes. Mask schema v2 has `all`,
  linear gradient, circle, rotated ellipse, parametric, and ordered group
  variants; every node has finite bounded opacity/inversion. Schema-v1
  `{id, kind=all, schema_version=1}` is read only as identity `all` and
  immediately upgrades to v2. A v1 object carrying v2 common state is rejected
  rather than acquiring invented semantics. JSON is strict and deterministic;
  unknown fields/types/versions, duplicate IDs, dangling references, cycles,
  excessive node/child/depth or expanded-evaluation counts, invalid enum state,
  and invalid finite bounds fail closed. The expansion bound prevents shared
  DAG paths from turning depth-first evaluation into exponential work.
- Shape coordinates name pixel centres in the full input frame attached to the
  operation: `(roi_x + x + 0.5) / full_width` and equivalently for Y. The
  evaluator receives full dimensions and an explicit ROI; tiled alpha is
  identical to full-frame alpha. Circle/ellipse radii and feathers normalize
  against `min(full_width, full_height)`. The graph never guesses sensor or
  pre-geometry coordinates.
- The shape mathematics freezes the relevant CPU expressions, not a generic
  signed-distance substitute. Linear gradient retains the source linear
  branch's rotated anchor and
  `0.5 + 0.5 * (inverse_transition * distance)` ordering;
  positive transition retains the frozen `0.001` effective-width floor and
  zero transition is the explicit hard edge. Circle and ellipse retain the
  source quadratic falloff (`clamp(ratio)^2`); zero feather is explicit inside/
  outside membership. The old gradient's curvature/erf modes, distortion
  transforms, ellipse proportional flag, and GTK point editing are not part of
  this canonical schema. Pixel-centre sampling and normalized attached-frame
  coordinates deliberately replace legacy ROI-grid/sample positions; this is a
  product coordinate contract, not compatibility translation.
- Parametric masks retain frozen `_blendif_compute_factor` branches and exact
  endpoint ordering plus its `0.001` slope-denominator floor for a four-key
  `0 → 1 → 1 → 0` ramp. Their source is
  explicitly `input` or `operation_output`, and channel is luminance/R/G/B.
  The engine has already bridged this dispatch point to linear Rec.709, so
  luminance is the explicit linear-Rec.709-to-XYZ-D50 matrix Y row rather than
  a legacy profile-handle call. Missing operation output is structured
  unsupported, never input fallback.
- Group children apply their own referenced-node result, then frozen edge
  inversion and opacity, before ordered composition. The first child is
  `replace`; later children use union/max, intersection/min when both positive
  (else zero), difference `lhs * (1 - rhs)` when both positive (else lhs), or
  exclusion `max((1-lhs)*rhs, lhs*(1-rhs))` when both positive (else max).
  This is the frozen ROI-helper order, not the earlier simplified subtract/
  absolute-difference proposal.
- `ravo_engine` privately evaluates an owned single-channel alpha plane with
  no global graph/scheduler. It validates dimensions, stride, sample count,
  finite RGB input/output, graph, and cancellation before allocation and at
  node/row boundaries. Group evaluation is depth-first and holds only the
  current accumulator/child stack, not one full alpha plane per node. The RAW
  estimator uses saturating arithmetic and explicitly counts masked
  pre-operation snapshot, operation output, alpha plane, and this evaluator
  scratch.
- The only canonical blend is normal masked interpolation
  `input + alpha * (operation_output - input)`. Exact alpha zero copies input
  bits; exact alpha one retains operation-output bits. The unmasked dispatch
  remains untouched. `ravo.color.colorharmonizer`, `ravo.effect.graduatednd`,
  `ravo.color.colorbalancergb` (ADR-0108), `ravo.core.exposure` (ADR-0109),
  `ravo.color.rgbcurve` (ADR-0110), `ravo.core.tonecurve` (ADR-0111), and
  `ravo.core.highlights` / `shadows` / `whites` / `blacks` (ADR-0112)
  advertise and execute `supports_mask`; other attached operations fail closed
  unless a later ADR names them. Graduated ND's own density gradient remains its operation
  mathematics, distinct from an optional generic attachment. Color Balance
  RGB's Filmlight Yrg luminance opacities likewise stay internal.
- `DevelopParams` holds the typed graph and both supported operation attachments
  (including disabled/default instances). `recipe_from_develop` and
  `develop_from_recipe` preserve them through live preview, save, cache key,
  close/reopen, undo values, and ordinary field/section reset. Graph presence
  prevents baseline-elision; only the explicit Catalog `reset_recipe` clears
  it. QML has no graph/mathematics/authoring ownership.
- Strict legacy XMP remains unchanged: legacy mask/custom-blend/multi state,
  including Color Harmonizer C14 evidence, stays rejected. Canonical Ravo
  recipes do not infer acceptance for historical histories.

## Consequences

CLI direct render, Catalog preview/cache/save/reopen, and Studio's existing
typed Develop round trip consume one Recipe graph. Studio deliberately has no
mask drawing or editing UI in this tranche; it preserves a loaded graph while
other numeric edits occur.

S3.1 does not complete S3 or M1 and does not retire C14. Path/brush masks,
sampling, Studio authoring/preview, picker/histogram/harmony presentation,
historic Lab/HSL/Jz blend modes, GPU, C15, and legacy deletion remain separate
work. The frozen `mask_manager.c` and all old owners remain read-only evidence.

## Rejected alternatives

- Store free-form mask JSON in `ParameterValue::Object`, let services rewrite
  the graph, or let QML own alpha/math.
- Cache a full alpha plane for every DAG node, infer missing operation output,
  silently clamp invalid recipe state, or publish a partially mixed image.
- Treat generic all/zero masks as a reason to alter unmasked operation paths.
- Relax legacy-XMP import, port GTK/path/brush UI, copy dynamic pixelpipe state,
  or call this limited foundation M1/C14 acceptance.
