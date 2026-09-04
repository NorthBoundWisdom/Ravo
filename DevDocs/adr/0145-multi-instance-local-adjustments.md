# ADR-0145: Multi-instance local adjustments and professional masks

- Status: Accepted
- Date: 2026-09-04
- Relates: LOCAL-01 in [TODO.md](../TODO.md)
- Extends: [ADR-0043](0043-canonical-mask-graph-foundation.md),
  [ADR-0044](0044-studio-canonical-mask-authoring.md),
  [ADR-0045](0045-studio-mask-overlay-group-path.md),
  [ADR-0108](0108-masked-color-balance-rgb.md),
  [ADR-0109](0109-masked-exposure.md),
  [ADR-0116](0116-histogram-assisted-parametric-mask.md)

## Context

Everyday Exposure and Color Balance RGB may each carry one owned canonical mask
(ADR-0108/0109). Photographers still need multiple ordered local instances
(dodge/burn stacks, stacked colour grades) with named bypass, reorder, and
professional mask leaves. Legacy darktable multi-instance XMP and unsupported
CRS multi-instance forms must remain fail-closed rather than approximated.

## Decision

### Multi-instance Develop operations

- A Develop operation may have multiple **ordered** instances. First consumers:
  `ravo.core.exposure` and `ravo.color.colorbalancergb`.
- Each instance has: stable `instance_id`, optional `name`, `enabled`,
  `bypass`, parameters, and optional `mask_id`.
- **Bypass** skips evaluation while retaining serialized parameters (distinct
  from deleting the instance). Disabled (`enabled=false`) also skips evaluation.
- Order in `Recipe.operations` (among instances of that operation id) is the
  evaluation order. Reorder is a recipe edit.
- QML never owns mask pixels or instance math; C++ owns serialization and
  evaluation.

### Mask leaves and composition

- C++-owned leaves remain the ADR-0043 graph: brush, path, linear gradient,
  radial/ellipse, plus parametric luminance/colour range (ADR-0116 authoring
  reuse). No new leaf kinds in this ADR.
- Group composition stays Add/Union, Subtract/Difference, Intersect, Invert
  (via leaf/group inversion) with opacity, as already defined in ADR-0043/0045.
- Coordinate mapping through orientation, lens, Perspective, crop, Canvas,
  preview scale, 1:1 ROI, and export remains the existing attached-frame
  contract; no QML remap.

### XMP

- Unsupported multi-instance legacy forms continue to fail closed (existing
  `unsupported_legacy_exposure_multi_state` and peer reasons). Ravo does not
  invent singleton approximations for named/priority multi-state.
- CRS/XMP interchange that cannot represent Ravo multi-instance Exposure
  without approximation fails closed on export/import of those forms.

### First Ready

- Recipe/`DevelopParams` serialization for **multi-instance Exposure**
  (`exposure_instances`) with optional instance `name`/`bypass` on
  `OperationInstance`.
- CLI `recipe inspect` plus existing validate/render apply paths covering
  multi-instance Exposure with linear gradient, radial/ellipse, and one
  parametric leaf.
- Color Balance RGB multi-instance and Studio polish are residual under this
  ADR (authorized consumers; Ready may ship Exposure-first).

### Migration

- Existing single-Exposure recipes remain bit-compatible: empty
  `exposure_instances` keeps the legacy DevelopParams exposure fields and
  emits at most one `exposure-1` operation as today.
- When `exposure_instances` is non-empty it is the ordered authority; the
  legacy single fields mirror `exposure_instances.front()` for thin Studio
  compatibility.

## Non-goals (explicit)

- Shipping every Develop operation as multi-instance in this tranche.
- Importing darktable/CRS multi-instance by approximation.
- QML-owned masks or a second parallel mask model.
- Full Studio instance chrome (thin is enough for Ready).

## Consequences

LOCAL-01 gains a dated multi-instance contract. Engine evaluation already walks
ordered operations; Develop round-trip and CLI inspect become the Ready gate.
Further consumers (Color Balance RGB instances, Studio UI) extend the same
model without a second serialization dialect.

## Rejected alternatives

- Overloading a single Exposure with multiple masks instead of instances.
- Silent collapse of legacy multi-priority histories into one instance.
- Storing instance lists only in Studio session state.
