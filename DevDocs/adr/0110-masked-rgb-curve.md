# ADR-0110: RGB Curve may carry one owned canonical mask

- Status: Accepted
- Date: 2026-09-01
- Extends: [ADR-0043](0043-canonical-mask-graph-foundation.md),
  [ADR-0044](0044-studio-canonical-mask-authoring.md),
  [ADR-0045](0045-studio-mask-overlay-group-path.md),
  [ADR-0108](0108-masked-color-balance-rgb.md),
  [ADR-0109](0109-masked-exposure.md)

## Context

The canonical mask graph and Studio authoring already exist. Color Harmonizer,
Graduated ND, Velvia, Color Zones, Monochrome, Split Toning, Color Balance RGB,
and Exposure can attach a mask. Everyday RGB Curves remain global, so a radial
or brush local contrast/tone reshape still cannot use the photographer's
primary curve tool.

PRO-LOCAL asked which remaining grading operations may carry an owned mask.
Tone Curve, Highlights/Shadows/Whites/Blacks, extra blend modes, multiple
instances, and picker/histogram-assisted authoring remain separate undecided
work.

## Decision

- `ravo.color.rgbcurve` v1 advertises `supports_mask`. One optional `mask_id`
  may attach to the single RGB Curve instance, including a display-sRGB
  instance written later in the recipe. Direct `apply_rgb_curve` without a
  recipe graph does not evaluate a mask; `apply_recipe_ops` does.
- Evaluation is the existing normal mix: snapshot input, run the unmasked
  operation, evaluate alpha, `input + alpha * (operation_output - input)`.
  Exact alpha 0/1 keep source bits. Identity parameters remain a pixel no-op
  even when a mask is attached.
- Studio authors the attachment through the same MaskEditor and reserved IDs
  (`ravo.studio.mask.rgb_curve.<n>`). Circle, ellipse, gradient, parametric,
  path, brush, and groups are already graph kinds; this decision adds a
  consumer, not a new shape. The editor is shown with the RGB family.
- Recipe/CLI/Catalog/Studio share DevelopParams `rgb_curve_mask_id`. QML only
  displays the editor map and forwards numeric mask intents. Invalid graphs,
  unsupported attachments, and cancellation fail closed before publication.
  Unmasked RGB Curve pixels are unchanged. A Curves section reset does not
  auto-detach the mask.

## Consequences

A radial or brush mask can grade RGB Curves through the same recipe path used
for a global curve. Local Tone Curve stays out of scope.

## Rejected alternatives

- Masking `ravo.core.tonecurve` in this tranche. RGB is the default Curves
  family; Tone remains a second consumer.
- Waiting for extra blend modes or multi-instance RGB Curve.
- QML-owned mask pixels or a second RGB Curve recipe model.
