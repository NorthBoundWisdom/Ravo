# ADR-0108: Color Balance RGB may carry one owned canonical mask

- Status: Accepted
- Date: 2026-09-01
- Extends: [ADR-0016](0016-filmlight-colorbalancergb.md),
  [ADR-0043](0043-canonical-mask-graph-foundation.md),
  [ADR-0044](0044-studio-canonical-mask-authoring.md),
  [ADR-0045](0045-studio-mask-overlay-group-path.md)

## Context

The canonical mask graph and Studio authoring already exist. Color Harmonizer,
Graduated ND, Velvia, Color Zones, Monochrome, and Split Toning can attach a
mask. Everyday Color Balance RGB remains global, so a radial or brush local
grade still cannot use the photographer's primary color tool.

PW5 asked which grading operations may carry an owned mask. Extra blend modes,
multiple instances of one operation, picker/histogram-assisted authoring, and
AI subject detection remain separate undecided work.

## Decision

- `ravo.color.colorbalancergb` v1 advertises `supports_mask`. One optional
  `mask_id` may attach to the single Color Balance RGB instance. The Filmlight
  Yrg three-zone luminance masks stay internal operation math and are not the
  canonical graph.
- Evaluation is the existing normal mix: snapshot input, run the unmasked
  operation, evaluate alpha, `input + alpha * (operation_output - input)`.
  Exact alpha 0/1 keep source bits. Identity parameters remain a pixel no-op
  even when a mask is attached.
- Studio authors the attachment through the same MaskEditor and reserved IDs
  (`ravo.studio.mask.color_balance_rgb.<n>`) as Color Harmonizer and Graduated
  ND. Circle, ellipse, gradient, parametric, path, brush, and groups are
  already graph kinds; this decision adds a consumer, not a new shape.
- Recipe/CLI/Catalog/Studio share DevelopParams `color_balance_rgb_mask_id`.
  QML only displays the editor map and forwards numeric mask intents. Invalid
  graphs, unsupported attachments, and cancellation fail closed before
  publication. Unmasked Color Balance RGB pixels are unchanged.

## Consequences

A radial or brush mask can grade Color Balance RGB through the same recipe
path used for a global grade. Multiple Color Balance RGB instances and named
blend modes stay out of scope.

## Rejected alternatives

- Waiting for extra blend modes or multi-instance Color Balance RGB. Local
  color on the everyday stack does not require those contracts.
- QML-owned mask pixels or a second Color Balance RGB recipe model.
- Treating the Filmlight Yrg luminance opacities as canonical drawn masks.
