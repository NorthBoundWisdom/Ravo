# ADR-0112: Highlights, Shadows, Whites, and Blacks may each carry one owned canonical mask

- Status: Accepted
- Date: 2026-09-01
- Extends: [ADR-0043](0043-canonical-mask-graph-foundation.md),
  [ADR-0044](0044-studio-canonical-mask-authoring.md),
  [ADR-0045](0045-studio-mask-overlay-group-path.md),
  [ADR-0088](0088-lightroom-response-calibration.md),
  [ADR-0091](0091-monotonic-whites-blacks-response.md),
  [ADR-0109](0109-masked-exposure.md),
  [ADR-0111](0111-masked-tone-curve.md)

## Context

The canonical mask graph and Studio authoring already exist. Exposure, RGB
Curve, and Tone Curve may each attach one owned mask. Everyday Highlights,
Shadows, Whites, and Blacks remain a fused unmasked Light pass, so a radial or
brush local highlight recovery or shadow lift still cannot use those sliders.

`apply_recipe_ops` already stops absorbing a ranked Light control into that
fused pass when the candidate carries a `mask_id`. Until a named consumer
exists, a masked attachment fails closed as unsupported.

PRO-LOCAL asked which remaining grading operations may own a mask. Extra blend
modes, multiple instances, Contrast, Gamma, RGB levels, and picker/histogram
authoring remain separate undecided work.

## Decision

- `ravo.core.highlights`, `ravo.core.shadows`, `ravo.core.whites`, and
  `ravo.core.blacks` v1 advertise `supports_mask`. Each single instance may
  carry one optional `mask_id`. There is no public direct apply that evaluates
  a mask; `apply_recipe_ops` does.
- An enabled masked Light control is not absorbed into the fused Highlights →
  Shadows → Whites → Blacks row pass. It runs as a single-amount
  `apply_light_controls` plus the existing normal mix: snapshot input, apply
  the unmasked envelope, evaluate alpha,
  `input + alpha * (operation_output - input)`. Exact alpha 0/1 keep source
  bits. Identity amounts remain a pixel no-op even when a mask is attached.
- Unmasked contiguous ranked neighbours still fuse among themselves. Unmasked
  pixels stay bit-identical to the current fused kernel. Inserting a mix
  boundary between a masked control and a later unmasked neighbour is sequential
  composition, not the fused one-pass EV accumulation.
- Studio authors each attachment through the same MaskEditor and reserved IDs
  (`ravo.studio.mask.highlights.<n>` and the shadows/whites/blacks equivalents).
  Circle, ellipse, gradient, parametric, path, brush, and groups are already
  graph kinds; this decision adds consumers, not a new shape.
- Recipe/CLI/Catalog/Studio share DevelopParams `highlights_mask_id`,
  `shadows_mask_id`, `whites_mask_id`, and `blacks_mask_id`. QML only displays
  the editor maps and forwards numeric mask intents. Invalid graphs, unsupported
  attachments, and cancellation fail closed before publication. A Light section
  reset does not auto-detach these masks.

## Consequences

A radial or brush mask can grade Highlights, Shadows, Whites, or Blacks through
the same recipe path used for a global Light slider. Contrast, Gamma, RGB
levels, extra blend modes, and multi-instance remain out of scope.

## Rejected alternatives

- One mask for the whole fused HSWB pass. Photographers need independent local
  Highlights versus Shadows.
- Masking only Highlights and Shadows in this tranche. The four operations
  share the mix boundary; splitting the kernel twice would duplicate the owner.
- Waiting for extra blend modes or a second Light recipe model.
- QML-owned mask pixels.
