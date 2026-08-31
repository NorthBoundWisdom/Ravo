# ADR-0020: Adjust RGB primaries in the declared working profile

- Status: Accepted
- Date: 2026-08-26

## Context

The frozen `primaries` operation rotates and scales the three working-space
primary chromaticities and may tint the achromatic axis. Its result depends on
the active working profile. Applying the same parameters after Ravo's existing
linear-Rec709 compatibility bridge would silently change the look of recipes
whose declared working profile is Rec2020, ProPhoto RGB, Display P3, or a file
ICC matrix profile.

## Decision

- `ravo.color.primaries` v1 stores achromatic tint hue/purity and red, green,
  and blue hue/purity. Canonical hue values are radians; Studio projects them to
  degrees only at the presenter/command boundary.
- The engine derives primary xy coordinates from the columns of the immutable
  working RGB→XYZ D50 matrix and derives the white point from their sum. It
  retains the frozen forward ray/triangle-edge intersection, primary rotation
  and scaling, custom RGB→XYZ construction, and
  `XYZ-to-working × custom-to-XYZ` adjustment matrix.
- Primaries runs immediately after `ravo.color.input`, before channel mixer and
  all operations that Ravo currently bridges to linear Rec709. The result keeps
  the exact input `ColorProfileState`; the operation changes pixel values, not
  the declared working space.
- A render owns its derived 3×3 matrix and output buffer. It publishes only
  after complete finite validation and checks cancellation by row. No legacy,
  LittleCMS, Qt, or display-profile handle participates in the operation.
- The old GTK slider gamut painting, dynamic IOP lifecycle, blend UI, and
  OpenCL implementation are not migrated. Studio exposes neutral controls and
  the general mask graph remains a separate queued owner.

## Consequences

Default parameters are an exact identity. Missing/non-RGB working state,
non-finite or singular matrices, degenerate chromaticities, backwards or
parallel-only intersections, invalid parameters, float overflow, allocation
failure, and cancellation return structured failure without mutating the input
or publishing partial pixels.

## Rejected alternatives

- Apply primaries after the linear-Rec709 bridge: it changes working-profile
  semantics and makes the result depend on a scheduler implementation detail.
- Store custom primaries as a replacement output profile: the frozen operation
  is a pixel adjustment within the existing working profile.
- Recreate the GTK/display-profile slider colors in QML: that would move color
  calculation into presentation and reintroduce monitor-dependent state.
- Keep `common/custom_primaries.*` as production code: Ravo owns an independent
  C++ implementation; the frozen helper remains evidence only while `agx` still
  consumes it.
