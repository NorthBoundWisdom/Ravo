# ADR-0111: Tone Curve may carry one owned canonical mask

- Status: Accepted
- Date: 2026-09-01
- Extends: [ADR-0043](0043-canonical-mask-graph-foundation.md),
  [ADR-0044](0044-studio-canonical-mask-authoring.md),
  [ADR-0045](0045-studio-mask-overlay-group-path.md),
  [ADR-0108](0108-masked-color-balance-rgb.md),
  [ADR-0109](0109-masked-exposure.md),
  [ADR-0110](0110-masked-rgb-curve.md)

## Context

The canonical mask graph and Studio authoring already exist. RGB Curve may
attach one owned mask (ADR-0110). The Curves panel's Tone family
(`ravo.core.tonecurve`) remains global, so a radial or brush local Lab/XYZ
luminance reshape still cannot use that tool.

PRO-LOCAL asked which remaining grading operations may carry an owned mask.
Highlights/Shadows/Whites/Blacks, extra blend modes, multiple instances, and
picker/histogram-assisted authoring remain separate undecided work.

## Decision

- `ravo.core.tonecurve` v1 advertises `supports_mask`. One optional `mask_id`
  may attach to the single Tone Curve instance. Direct `apply_tone_curve`
  without a recipe graph does not evaluate a mask; `apply_recipe_ops` does.
- Evaluation is the existing normal mix: snapshot input, run the unmasked
  operation, evaluate alpha, `input + alpha * (operation_output - input)`.
  Exact alpha 0/1 keep source bits. Identity parameters remain a pixel no-op
  even when a mask is attached.
- Studio authors the attachment through the same MaskEditor and reserved IDs
  (`ravo.studio.mask.tone_curve.<n>`). The editor is shown with the Tone family.
  Circle, ellipse, gradient, parametric, path, brush, and groups are already
  graph kinds; this decision adds a consumer, not a new shape.
- Recipe/CLI/Catalog/Studio share DevelopParams `tone_curve_mask_id`. QML only
  displays the editor map and forwards numeric mask intents. Invalid graphs,
  unsupported attachments, and cancellation fail closed before publication.
  Unmasked Tone Curve pixels are unchanged. A Curves section reset does not
  auto-detach the mask.

## Consequences

A radial or brush mask can grade Tone Curve through the same recipe path used
for a global Tone curve. Local Highlights/Shadows stay out of scope.

## Rejected alternatives

- Folding Tone and RGB Curve into one mask consumer. They remain separate
  operations and families.
- Waiting for extra blend modes or multi-instance Tone Curve.
- QML-owned mask pixels or a second Tone Curve recipe model.
