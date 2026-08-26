# ADR-0022: Separate final display packing from legacy diagnostic presentation

- Status: Accepted
- Date: 2026-08-26
- Relates to: ADR-0015, ADR-0019, root `TODO_LEGACY_MIGRATION.md` C8

## Context

The frozen [`gamma.c`](../../../legacy/src/iop/gamma.c) is named “display
encoding,” is hidden, mandatory, and single-instance, but it combines two
different responsibilities. Its normal `_copy_output` branch quantizes the
already output-profiled float buffer. Its other branches render old channel and
mask inspection state for the GTK darkroom.

The module serializes two floats named `gamma` and `linear`, yet `process()`
never reads either parameter. All 158 frozen XMP histories contain exactly one
enabled schema-v1 instance with the zeroed eight-byte payload, default instance
state, and one of 12 exact historical default-blend serialization tuples. This
is evidence for a mandatory pipeline boundary, not for either
`ravo.core.gamma` or `ravo.color.profilegamma`.

## Decision

- Ravo's engine owns the normal `_copy_output` responsibility as a private
  final-packing boundary after `ravo.color.output`: clamp negative profiled RGB,
  multiply by 255, round, clamp super-white to 255, and publish owned RGB8 with
  the exact `ColorProfileState`. It applies no additional transfer curve.
- The old BGR byte order is only the GTK/pixelpipe destination layout. It is not
  a colour transform and does not enter Ravo's RGB `RenderedImage` contract.
- Legacy XMP import absorbs `gamma` only when version, enabled state, zero
  payload, singleton state, default multi-instance state, absence of mask
  attributes, and the exact versioned blend tuple match the frozen census. The
  importer emits no recipe operation for this boundary. Unknown or modified
  state fails structurally.
- `_channel_display_monochrome` is unsupported. It interprets a component
  placed in an otherwise unlabelled input lane according to external
  `mask_display` state, rather than consuming a self-describing image contract.
- `_channel_display_false_color` is unsupported, including its Lab, RGB, LCh,
  HSL, and JzCzhz branches. Their channel choice comes from the old mutable
  pixelpipe display-mask enum, and their result is immediately coupled to the
  same GTK BGR writer and optional yellow mask overlay.
- `_mask_display` is unsupported. It combines an unlabelled fourth-channel mask
  with RGB luminance using mutable `darkroom/ui/develop_mask_mix` configuration.
  The yellow overlay in `_write_pixel` is likewise presentation state rather
  than an edit or reusable analysis result.
- None of these unsupported diagnostic branches enters recipe, services, or
  QML. A future channel or mask inspection feature must start with a typed
  engine/service analysis contract that identifies channel semantics, mask
  ownership, colour state, cancellation, and immutable inputs; it must not
  resurrect `gamma` as an operation.

This respects ADR-0015: the retired code is a presentation-coupled adapter whose
calculations cannot be consumed independently without implicit GTK/pixelpipe
state. The independently consumable normal calculation is retained by the
engine packing contract. No supported image algorithm is silently replaced by
UI code or discarded.

## Consequences

Output-profile conversion remains solely owned by `ravo.color.output`, while
RGB8 packing has one engine owner shared by CLI and Catalog publication. Exact
legacy histories remain readable without inventing an edit. Modified payload,
blend/mask, duplicate, disabled, version, or multi-instance state is rejected
instead of being silently normalized.

After the C8 engine and import contracts pass their acceptance gates,
`legacy/src/iop/gamma.c` can be deleted with its registration. No diagnostic
compatibility shell or QML fallback is retained.

## Rejected alternatives

- Interpret the two zero floats as a gamma edit: they do not drive the frozen
  CPU path and would collide with two distinct Ravo operations.
- Apply an sRGB curve in the final packer: output colour already owns transfer
  encoding and profile state, so this would double-encode pixels.
- Preserve BGR as an engine contract: it would leak an obsolete GTK memory
  layout into profile-labelled RGB output.
- Port channel and mask branches directly into recipe or QML: their inputs and
  behavior depend on mutable, non-self-describing presentation state.
