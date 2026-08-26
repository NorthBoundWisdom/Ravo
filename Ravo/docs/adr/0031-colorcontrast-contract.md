# ADR-0031: Preserve Color Contrast as a versioned D50 Lab axis-affine operation

- Status: Accepted
- Date: 2026-08-27

## Context

The frozen `legacy/src/iop/colorcontrast.c` version-2 owner operates directly
on D50 Lab. It preserves L* and alpha, independently multiplies and offsets a*
and b*, and either publishes extended chromatic values or clamps each
chromatic axis to [-128, 128]. Its GTK sliders, aliases, tiling declaration,
blend UI, and `extended.cl` kernel surround that CPU contract but do not add
serialized mathematics.

The complete 158-XMP census contains one `colorcontrast` record, in 0038. Its
module record is enabled version 2, singleton, priority zero, unnamed, and has
the frozen blend-v10 payload. The complete 0038 document also contains a real
mask graph, so it remains a structured negative until the general mask graph
has a separate canonical owner; the same verbatim operation record in a
minimal default-unmasked document is positive import evidence. Repository
history independently proves a legacy version-1 four-float parameter layout
whose upgrade adds `unbound=0`.

Ravo previously reserved the same `ravo.color.colorcontrast` identifier for a
schema-v1 `amount` control. That implementation changed both Lab chromatic
slopes together, had no offsets or bounded branch, and skipped amount zero. It
is compatibility input, not a substitute for the five frozen legacy
parameters.

## Decision

- `ravo.color.colorcontrast` schema v2 contains exactly seven required fields:
  `working_space=lab_d50`, `algorithm=axis_affine_v2`, finite and
  float-representable `a_steepness`, `a_offset`, `b_steepness`, `b_offset`, and
  boolean `unbound`. The two steepness values are bounded to [0, 5]; offsets
  use the full finite float surface because the frozen parameter owner declares
  no narrower bound.
- Existing Ravo schema-v1 recipes normalize deterministically. `amount` in
  [-1, 1] becomes both slopes using the original float order
  `1.0F + static_cast<float>(amount)`, both offsets become zero, and `unbound`
  becomes true. A zero amount disables the upgraded operation to preserve the
  old exact skip; a nonzero enabled value remains enabled, and an originally
  disabled operation remains disabled. Validation, recipe load, and engine
  dispatch all use the same upgrade.
- Canonical schema-v2 defaults are not an identity shortcut. When explicitly
  enabled they retain the observable linear-Rec709-to-D50-Lab round trip. Only
  an absent/disabled operation or the normalized schema-v1 zero state skips.
- The engine narrows canonical numbers to float once, then evaluates the
  frozen expressions in source order: `input * steepness + offset`. Unbounded
  mode adds no clamp or repair. Bounded mode reproduces the frozen `CLAMPS`
  ternary independently for a* and b* with limits [-128, 128]. The private
  S1.1 bridge supplies the surrounding declared linear-Rec709 RGB conversion;
  no public profile or colour-science API is added.
- Dimensions, RGB buffer length, declared linear-Rec709 profile, parameters,
  and every input and output sample are validated before publication.
  Cancellation is observed before work, across input and output rows, and
  before return. Success owns a separate RGB/profile buffer and retains the
  immutable exposure-analysis snapshot. Allocation, arithmetic, validation,
  mask, and cancellation failures publish no partial result and leave the
  borrowed input unchanged. The operation needs one output-sized buffer and no
  operation-specific cache or analysis allocation.
- The strict legacy importer accepts module versions 1 and 2 only within the
  frozen enabled singleton, exact priority-zero, unnamed, default-unmasked
  envelope and exact blend-v10 tuple represented by 0038. Version 1 applies
  the repository-history `unbound=0` upgrade. History `num` remains a generic
  instance identifier, not an algorithm revision or processing-order rule.
  Disabled/malformed enabled state, duplicates, multi/name state, masks,
  custom blend, unknown attributes, malformed payloads, invalid integer flags,
  non-finite values, and unsupported versions fail structurally.

## Consequences

The core tranche now has one strict recipe schema, deterministic compatibility
for the former Ravo schema and the frozen legacy parameter version, one CPU
engine owner, canonical dispatch, and a fail-closed importer boundary. Fixed
bit goldens and an independent scalar/D50 oracle distinguish axis order,
float evaluation, bounded versus extended output, and the default round trip.

This ADR does not claim the complete C13 vertical slice or authorize retirement
of `legacy/src/iop/colorcontrast.c`. Develop presence/reset behavior,
CLI/Catalog/Studio persistence and controls, stable-document registration, and
atomic source retirement remain later reviewed tranches. The real 0038 mask,
general mask graph, GTK presentation, GPU path, shared order/modulegroup/manual
references, and legacy execution also remain outside this tranche.

## Rejected alternatives

- Keep the old one-amount schema as the canonical operation: it cannot express
  independent slopes, offsets, or the bounded branch.
- Map legacy version 1 to `unbound=true`: repository history explicitly adds
  the new field with value zero.
- Treat canonical defaults as absent: the frozen D50 Lab bridge makes an
  explicitly enabled default operation observably different from a skip.
- Use `std::clamp`, double arithmetic, reordered expressions, RGB saturation,
  or non-finite repair: each changes the frozen CPU boundary.
- Accept the complete 0038 document while ignoring its mask, or infer canonical
  order from history `num`: both would silently invent unsupported semantics.
