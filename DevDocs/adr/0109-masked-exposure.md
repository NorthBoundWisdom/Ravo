# ADR-0109: Exposure may carry one owned canonical mask

- Status: Accepted
- Date: 2026-09-01
- Extends: [ADR-0043](0043-canonical-mask-graph-foundation.md),
  [ADR-0044](0044-studio-canonical-mask-authoring.md),
  [ADR-0045](0045-studio-mask-overlay-group-path.md),
  [ADR-0108](0108-masked-color-balance-rgb.md)

## Context

The canonical mask graph and Studio authoring already exist. Color Harmonizer,
Graduated ND, Velvia, Color Zones, Monochrome, Split Toning, and Color Balance
RGB can attach a mask. Everyday Exposure remains global, so a radial or brush
local dodge/burn still cannot use the photographer's primary Light control.

PW5 asked which remaining grading operations may carry an owned mask. Contrast,
Highlights, Shadows, Whites, Blacks, Curves, extra blend modes, multiple
instances of one operation, and picker/histogram-assisted authoring remain
separate undecided work. Highlights/Shadows/Whites/Blacks stay fused in one
unmasked Light pass; attaching a mask to that group would change mix order
and is not this tranche.

## Decision

- `ravo.core.exposure` v2 advertises `supports_mask`. One optional `mask_id`
  may attach to the single Exposure instance. Direct `apply_exposure` without a
  recipe graph still rejects a mask; `apply_recipe_ops` evaluates it.
- Evaluation is the existing normal mix: snapshot input, run the unmasked
  operation, evaluate alpha, `input + alpha * (operation_output - input)`.
  Exact alpha 0/1 keep source bits. Identity parameters remain a pixel no-op
  even when a mask is attached.
- Studio authors the attachment through the same MaskEditor and reserved IDs
  (`ravo.studio.mask.exposure.<n>`) as Color Harmonizer and Color Balance RGB.
  Circle, ellipse, gradient, parametric, path, brush, and groups are already
  graph kinds; this decision adds a consumer, not a new shape.
- Recipe/CLI/Catalog/Studio share DevelopParams `exposure_mask_id`. QML only
  displays the editor map and forwards numeric mask intents. Invalid graphs,
  unsupported attachments, and cancellation fail closed before publication.
  Unmasked Exposure pixels are unchanged. A Light section reset does not
  auto-detach the mask.

## Consequences

A radial or brush mask can grade Exposure through the same recipe path used
for a global EV change. Local Highlights/Shadows/Curves stay out of scope.

## Rejected alternatives

- Masking the fused Highlights/Shadows/Whites/Blacks pass in this tranche.
  That group is one engine kernel; a mask would require a new mix boundary.
- Waiting for extra blend modes or multi-instance Exposure.
- QML-owned mask pixels or a second Exposure recipe model.
