# ADR-0019: Publish explicit output colour profiles

- Status: Accepted
- Date: 2026-08-26

## Context

ADR-0018 established immutable input and working-profile state, but every
render still ended in fixed sRGB encoding. That could not represent the frozen
`colorout` matrix/shaper path, general output ICC transforms, soft proofing,
gamut warnings, or profile-consistent PNG/JPEG/TIFF publication.

## Decision

- Canonical recipe schema v3 adds exactly one enabled `ravo.color.output` v1
  boundary. It stores the output profile or file, four-way rendering intent,
  proof mode/profile/intent, and black-point compensation. Schema-v1/v2
  recipes upgrade by inserting the missing explicit colour boundaries.
- The engine consumes `LinearWorkingBuffer::color_profile`. RGB matrix/shaper
  outputs use the frozen 65,536-sample inverse shaper and power-law
  extrapolation above one. Matrix-free ICC, XYZ, Lab, soft-proof, and gamut-
  check paths use render-local LittleCMS profiles, contexts, and transforms.
- Output and proof profiles may be built-in or file ICC. Missing, corrupt,
  singular, non-finite, unsupported, or cancelled transforms fail before a
  pixel buffer or file is published. No display-profile or sRGB fallback is
  permitted.
- Gamut check uses a task-local LittleCMS alarm set to cyan. Soft-proof profiles
  are quantized through owned table curves where required; proof calculation
  remains in the engine and Studio only presents the result.
- `ColorProfileState` is the encoded-result contract. It carries owned ICC
  bytes and no LittleCMS or Qt handle. Ravo-generated built-in ICC bytes have a
  deterministic header. File ICC bytes remain byte-preserving, and output/
  proof file content participates in preview cache identity.
- The engine PNG writer emits one standard `sRGB` chunk for the built-in sRGB
  profile, otherwise one `iCCP` and no conflicting `sRGB` chunk. The private Qt
  raster adapter embeds the same state in PNG/JPEG/TIFF or rejects it. Only RGB
  output can be quantized into the current `RenderedImage`; direct XYZ/Lab
  transforms remain available to the private colour adapter and fail
  structurally at an RGB publication boundary.
- Preview contract v6 returns profile-labelled RGB memory pixels or a profiled
  cache file. Studio attaches the ICC to its `QImage` and exposes one Output &
  Soft Proof Inspector; QML owns no colour transform.

## Consequences

CLI PNG, processed preview, and CatalogService PNG/JPEG/TIFF export now use the
same recipe-owned output transform and declared profile. Changing external
output or proof ICC content invalidates the final preview without invalidating
the reusable scene-linear working buffer. Original-copy export remains byte-
preserving.

## Rejected alternatives

- Keep fixed sRGB for preview or CLI: it would make recipe output state
  target-dependent and break export consistency.
- Infer a monitor profile or legacy global pipeline mode: neither is explicit,
  portable, or safe for headless rendering.
- Pass LittleCMS or `QColorSpace` handles through services: their lifetime and
  thread ownership would cross target boundaries.
- Let encoders relabel pixels when a profile is unavailable: that is a silent
  colour fallback and can publish a trusted but incorrect file.
