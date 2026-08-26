# ADR-0025: Preserve legacy Color Balance as an independent full operation

- Status: Accepted
- Date: 2026-08-27

## Context

The frozen `legacy/src/iop/colorbalance.c` is a display-referred colour
operation with two complete CPU modes: lift/gamma/gain and
slope/offset/power. It converts Lab D50 through XYZ into ProPhoto RGB, derives
corrected RGBL controls at commit time, applies input saturation, the selected
mode, output saturation, and grey-fulcrum contrast, then converts back through
Lab. Its mode plus 16 numeric values form 17 legacy fields. The old Ravo
three-parameter approximation did not preserve this path and was removed.

`ravo.color.colorbalancergb` is a different scene-referred Filmlight Yrg
operation with DT UCS or JzAzBz gamut handling. Sharing a name, schema, cache
identity, or simplified implementation between the two would silently change
recipe and pixel semantics.

The 158 frozen XMP files contain four enabled colorbalance revisions, all in
0033 and 0034. They are version 3, but each real history includes either a
custom blend, a parametric mask, or a named priority-one instance. They are
therefore negative compatibility evidence. Exact default-unmasked singleton
v3 and v4 payloads can be frozen synthetically without treating XML history
position as canonical processing order.

## Decision

- `ravo.color.colorbalance` v1 is independent from
  `ravo.color.colorbalancergb`. Its schema, equality, reset, serialization, and
  cache identity include mode and all 16 numeric legacy values. Develop state
  also records explicit operation presence: an absent operation is skipped,
  while an explicitly present operation with default values executes and
  survives recipe, Catalog, and Studio round trips.
- The declared workspace and algorithm are `linear_srgb_d50` and
  `lab_d50_prophoto_v4`. The CPU path reproduces the frozen Lab D50 to XYZ to
  ProPhoto conversion and inverse, commit-time RGB chroma correction, input
  and output saturation around ProPhoto Y, and grey-fulcrum contrast. Mode
  `lift_gamma_gain` uses the frozen lift/gamma/gain derivation;
  `slope_offset_power` uses the frozen ASC-CDL derivation. Their contrast gates
  remain distinct: LGG tests `contrast_power`, while SOP tests
  `contrast_amount`, each against one with the frozen epsilon.
- An explicit default operation is not an identity shortcut because the
  colour-space round trip is observable in float pixels. Every source,
  parameter, denominator, power domain, and result is checked before an owned
  output is published. Source pixels/profile state are immutable, cancellation
  is checked before and during rows, and any validation, allocation,
  cancellation, or arithmetic failure publishes no partial result.
- The strict legacy decoder accepts only exact v3/v4 parameter payloads with
  lexical enabled flag `0` or `1` and default-unmasked singleton presentation
  state. It accepts exactly one Color Balance entry: `num` contributes only to
  its deterministic instance ID, while any duplicate is a conflict rather than
  a revision-selection rule. Missing/unknown fields, bad lexical or numeric
  state, masks, custom blend state, named/multiple instances, and unsupported
  versions fail with stable structured diagnostics.
  Synthetic v3/v4 cases prove positive mapping; 0033/0034 and the complete
  158-fixture census prove only the negative boundary.
- Picker sampling, HSL colour derivation, and auto-optimisation are GTK
  presentation helpers rather than serialized CPU mathematics. Studio exposes
  the complete numeric RGBL surface across the schema hard bounds. CLI,
  Catalog preview/save/reopen/export, and
  Studio consume the same recipe, engine operation, and cache identity.
- The general mask graph is not expanded for this migration. Exact default
  unmasked state is supported and all other mask/blend/multi presentation state
  rejects structurally. Shared legacy ordering, proxy, and module-name strings,
  plus Color Balance kernels in `extended.cl`, remain D0.4/S4/S14 retirement
  work and are not Ravo runtime owners.

## Consequences

The complete frozen default CPU behaviour has a versioned owner distinct from
Color Balance RGB. Independent scalar/matrix reference tests validate fixed
SOP and LGG goldens, including a perturbation that demonstrates channel-order
drift is detected. Mode-specific finite/domain/cancellation tests, immutable
source/profile tests, RAW regression, strict importer census, recipe presence,
and CLI/Catalog/Studio persistence make the old source and exact registration
safe to retire.

Real 0033/0034 histories remain structured unsupported rather than being
misrepresented as positive compatibility. Future masks, picker analysis, GPU,
or shared registry cleanup require their separately owned contracts and do not
change this operation's accepted CPU boundary.

## Rejected alternatives

- Reuse `ravo.color.colorbalancergb` or the removed three-parameter
  approximation: neither reproduces the frozen colour space, controls, or two
  mode equations.
- Treat default parameters as absent or a no-op: the frozen Lab/ProPhoto round
  trip changes float output and must survive persistence.
- Infer processing order from XMP history order or silently choose a masked or
  named instance: those states have no canonical graph mapping.
- Port GTK picker/HSL/auto helpers or OpenCL kernels into Studio as a side
  effect: that would create new presentation, analysis, and GPU owners outside
  this migration.
