# ADR-0018: Own input colour profiles at the engine boundary

- Status: Accepted
- Date: 2026-08-26

## Context

The RAW path previously multiplied demosaiced camera samples by a fixed
LibRaw camera-to-sRGB matrix. The raster path converted a `QImage` to PNG and
later decoded that PNG as sRGB, losing the source profile. Both behaviours
contradicted ADR-0006 and made profile changes invisible to the scene-linear
cache.

The frozen `colorin` CPU path has distinct matrix-only, matrix plus one-
dimensional shaper, unbounded extrapolation, gamut-normalization, RAW blue-
mapping, and general ICC-transform branches. A fixed matrix is not an
acceptable replacement.

## Decision

- Canonical recipe schema v2 inserts one explicit `ravo.color.input` v1 when
  upgrading a Ravo schema-v1 recipe. The operation stores the input profile, optional file name,
  rendering intent, gamut-normalization target, RAW blue-mapping flag, and
  working profile. New Develop recipes always contain this operation.
- Decode boundaries carry an immutable `ColorProfileState`. It owns stable
  identifiers, ICC bytes, or a camera-to-XYZ D50 matrix, but no Qt, LibRaw,
  LittleCMS, or legacy handle. `LinearWorkingBuffer` carries the resulting
  working-profile matrix through cache and render boundaries.
- RAW decode publishes the selected LibRaw camera matrix as an enhanced-matrix
  source state. Demosaic produces camera RGB; the input-colour adapter performs
  the camera-to-working conversion. Embedded RAW JPEG previews explicitly
  declare sRGB.
- The Qt raster adapter preserves a valid embedded profile. An untagged raster
  remains `missing` and rendering fails structurally unless the recipe names a
  concrete built-in or file profile. It is never silently reinterpreted as
  sRGB.
- Matrix/shaper inputs use the frozen 65,536-sample LUT, power-law
  extrapolation above one, target-gamut clipping, and blue-mapping equations.
  Complex RGB/XYZ/Lab ICC inputs use a render-local LittleCMS transform.
  LittleCMS 2.19.1 is a pinned FreeCM source root linked privately by the
  engine; handles never cross a target boundary.
- Built-in RGB inputs include sRGB, Adobe RGB, linear Rec709/Rec2020, Rec709,
  linear ProPhoto RGB, Display P3, HLG/PQ Rec2020, and HLG/PQ P3. File ICC input may
  be matrix/shaper or a general transform. A file working profile must be an
  RGB matrix/shaper profile because the working buffer is linear RGB.
  Requested enhanced, embedded, standard, vendor, or alternate matrices are
  accepted only when decode supplied that exact matrix kind.
- Profile parameters and external ICC content participate in the scene-linear
  and preview cache keys. Preview contract v5 invalidates caches created before
  explicit profile ownership.
- C5 owns selectable output profiles. Until C5, render/export output is
  explicitly sRGB and the PNG/JPEG/TIFF encoder embeds that declaration.

## Consequences

Missing/corrupt profiles, unsupported colour models or matrix kinds, singular
or non-finite matrices, invalid LUTs, allocation failure, and cancellation
return structured errors before a pixel buffer or cache file is published.
Working profiles other than linear Rec709 are bridged explicitly into the
declared workspace of existing RGB operations rather than being treated as
unlabelled sRGB.

## Rejected alternatives

- Keep `DecodedRaw::camera_to_srgb`: it leaves profile ownership inside the
  decoder and cannot represent ICC, shaper, normalization, or working-profile
  state.
- Use `QColorTransform` in the engine: it would move Qt Gui across the raster-
  adapter boundary and still leave the CLI/core colour owner ambiguous.
- Treat missing raster metadata as sRGB: it is an unauditable fallback and is
  explicitly rejected by ADR-0006.
- Expose LittleCMS handles or cache them globally: their allocator and thread
  lifetime would leak into public contracts and recreate legacy global state.
