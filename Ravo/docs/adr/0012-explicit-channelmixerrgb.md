# ADR-0012: Color calibration uses the explicit frozen V3 CPU contract

- Status: Accepted
- Date: 2026-08-26
- Relates to: ADR-0006, root `TODO_LEGACY_MIGRATION.md` L1

## Context

Ravo's temperature/tint control did not replace the frozen
`channelmixerrgb.c` color-calibration path. A plain RGB 3×3 multiply would omit
the module's row normalization, chromatic adaptation, XYZ gamut compression,
V3 saturation/lightness adjustment and monochrome projection. Reusing its
implicit pipeline/camera state would also duplicate RAW white balance already
owned by Ravo's decoder and recipe.

## Decision

- `ravo.color.channelmixerrgb` v1 stores six three-element coefficient arrays,
  six normalization flags, explicit adaptation, illuminant xy, gamut
  compression and clipping. `working_space=linear_srgb_d50` and `algorithm=v3`
  are required versioned fields.
- The C++20 CPU operation reproduces the frozen default V3 stages: normalized
  mixing in RGB/CAT16/Bradford/XYZ space, adaptation to D50, uvY gamut
  compression, V3 Euclidean-norm saturation/lightness and optional grey mix.
- Ravo converts its linear-sRGB D65 working definition through a fixed CAT16
  D50 profile matrix for this operation. No legacy ICC, GTK, global chroma
  owner or camera state crosses the engine boundary.
- Studio exposes the safe 3×3 matrix in its Color Calibration section and uses
  `adaptation=rgb` by default. Advanced canonical recipes may explicitly select
  CAT16, linear/full Bradford or XYZ with a source illuminant.
- Singular finite matrices are valid forward transforms. Unknown fields,
  unsupported modes, non-finite/out-of-range coefficients, invalid xy and
  zero-sum normalized rows fail before pixel execution.
- The operation is synchronous and stateless; row-boundary cancellation is
  mandatory. OpenCL, chart/color-checker GUI, presets and legacy XMP ABI are not
  migrated.

## Consequences

- CLI render, CatalogService, export and Studio execute one operation
  implementation and persist one canonical schema.
- The frozen `0085-channelmixerrgb` parameter blobs and `mire1.cr2` provide
  static synthetic/RAW references without running the old application.
- `legacy/src/iop/channelmixerrgb.c`, its CMake registration and private chart
  helper are retired after the automated acceptance gate passed.

## Rejected alternatives

- A bare 3×3 linear-sRGB multiply: it does not implement the frozen CPU path.
- Automatic camera illuminant lookup inside the operation: it creates hidden
  profile state and can apply white balance twice.
- Keeping the GTK chart profiler as a second implementation: Ravo does not
  retain the legacy UI or dynamic IOP lifecycle.
