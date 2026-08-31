# ADR-0041: Color Harmonizer smoothing-zero vertical slice

- Status: Accepted
- Date: 2026-08-28
- Extends: [ADR-0035](0035-colorharmonizer-core-contract.md)

## Context

ADR-0035 accepted the exact 17-field `ravo.color.colorharmonizer` schema and
the enabled, unmasked, `smoothing == 0` CPU core. That core already reproduces
frozen 0176 records 12 and 13 as parameter and bit-golden evidence, but it did
not yet give the product a persistent Develop/CLI/Catalog/Studio surface or a
strict legacy history mapping.

The authorized follow-on tranche is only that product translation layer. The
mathematics, operation order, float contraction, D50/dt-UCS bridges, and RYB
tables stay unchanged. Positive smoothing, S2.2, a canonical ROI-scale
contract, masks/presentation, C15, and legacy-owner retirement remain
unauthorized.

## Decision

1. **Strict v1 import.** The private legacy-XMP adapter decodes the exact
   little-endian 60-byte payload: one 32-bit rule, the source-order floats, one
   32-bit custom-node count, four node-saturation floats, and the final
   smoothing float. Floats are decoded by bits. The accepted envelope is
   operation `colorharmonizer`, module v1, enabled `1`, priority `0`, empty
   `multi_name`, missing-or-zero `multi_name_hand_edited`, blend v14 with the
   exact default-unmasked payload evidenced by records 12/13, and no mask,
   custom blend, extra darktable attribute, or foreign namespace. Ordered
   revisions of that one singleton are history state: every captured revision
   is validated, reused positions conflict, and the greatest numeric history
   position wins. Record 12 alone maps the explicit default; 12 then 13 maps
   only 13. The mapped operation is `ravo.color.colorharmonizer` schema v1,
   instance id `legacy-colorharmonizer-0`, `enabled=true`, no mask, and the
   exact 17 registry-owned fields, inserted before Color Balance. Nonzero
   smoothing rejects as `unsupported_smoothing_requires_recursive_gaussian`.
   The complete 0176 document is not a compatibility claim.

2. **Develop presence.** `DevelopParams` owns `color_harmonizer_enabled` plus
   the existing `ColorHarmonizerParams`. Presentation fields use degrees for
   hues, integer rule index 0–9, and integer custom-node count 2–4. Changing a
   parameter enables the operation; changing enabled only changes presence.
   Reset of one field restores that field's canonical default; section
   `colorHarmonizer` and section `color` disable the operation and restore
   defaults. `recipe_from_develop` emits exactly one `colorharmonizer-1`
   operation when enabled, including an explicit default. `develop_from_recipe`
   preserves presence, rejects masks/duplicates/wrong schema, and leaves
   positive smoothing intact. `clamp_develop` repairs only non-finite
   smoothing; it does not coerce a valid positive value to zero.

3. **Consumers.** CLI `catalog develop --set` uses the generic strict Develop
   field path. Catalog preview/save/reopen/export carry the same immutable
   recipe and cache identity as a direct engine apply. Studio exposes one
   Develop section: enable, C++-owned 10-rule selector, hues/strengths, custom
   nodes 2–4, and four saturations. There is no smoothing slider, auto-detect,
   histogram, picker, mask, or OpenCL control. QML displays and forwards
   intents only.

4. **Errors and lifecycle.** Failed validation publishes no recipe, cache
   identity, preview, or export. Existing TaskError codes stay; operation
   context uses stable `reason` / `legacy_operation` / `legacy_version` /
   `attribute` keys. No second parameter model, renderer, schema migration,
   source/sidecar mutation, hidden enablement, or fallback is added.

## Consequences

The already-accepted smoothing-zero core is now one usable persistent product
capability. Absence and explicit presence remain distinct; an explicit enabled
default survives save/reopen even when pull strength is zero. Direct canonical
recipes with positive smoothing remain valid schema and still fail at the
existing engine boundary.

This decision does not accept positive smoothing, S2.2, ROI-scale, masks, GPU,
full 0176 document import, cross-platform execution evidence, or retirement of
`legacy/src/iop/colorharmonizer.c`.

## Rejected alternatives

- A second Develop or renderer-specific parameter model.
- Silent clamp/erase of positive smoothing while parsing Develop or recipes.
- Importing only the winning history record while ignoring malformed earlier
  revisions.
- Treating the complete 0176 document as compatible because synthetic records
  12/13 succeed.
- Exposing a smoothing slider or other presentation that implies the
  recursive-Gaussian path is usable.
