# ADR-0017: Temperature owns explicit pre-demosaic channel scaling

- Status: Accepted
- Date: 2026-08-26
- Relates to: ADR-0006, ADR-0012, ADR-0015, `DevDocs/TODO_LEGACY_MIGRATION.md` C3
- Supersedes: the `ravo.color.white_balance` Kelvin/tint approximation and
  ADR-0012's decoder-owned white-balance wording

## Context

Ravo previously multiplied LibRaw as-shot values inside demosaic and could
also apply a Kelvin/tint approximation afterward on linear RGB. That split
white-balance ownership, could apply the intent twice and did not implement
the frozen `temperature.c` four-channel CFA/RGB scaling. The frozen
late-reference mode also relied on mutable global chroma state shared with
`channelmixerrgb`.

## Decision

- `ravo.color.temperature` v1 requires
  `working_space=camera_cfa_or_linear_rgb`,
  `algorithm=channel_scale_v4` and an explicit mode:
  `as_shot`, `camera_reference`,
  `as_shot_to_reference` or `manual`.
- An optional four-element coefficient array stores resolved R/G1/B/G2 or
  CYGM channel multipliers in `(0, 8]`. Manual mode requires it. Automatic
  RAW modes resolve absent coefficients from immutable decode metadata:
  LibRaw `cam_mul` for as-shot and `pre_mul` for daylight
  camera reference.
- Missing, zero, non-finite or out-of-range metadata fails structurally.
  Ravo does not retain the frozen 2/1/1.5 generic fallback. A non-finite unused
  fourth value in a three-color legacy blob is canonicalized to neutral 1;
  four-channel fixtures preserve their real fourth coefficient.
- RAW scaling is owned by the pre-demosaic engine path and participates in the
  scene-linear cache key. It scales each CFA sample by its own channel before
  camera-to-working conversion. The source `DecodedRaw` and original
  file remain immutable.
- Non-mosaiced input uses the same three visible coefficient multiplications
  on an owned RGB output buffer. Automatic modes on raster input fail because
  no camera metadata owner exists.
- `as_shot_to_reference` applies as-shot once and is valid only when
  followed by an explicit non-RGB `ravo.color.channelmixerrgb`
  adaptation. No runtime global state silently modifies that operation.
- Temperature must precede highlights, chromatic-aberration correction and hot
  pixels in a canonical recipe. More than one enabled temperature operation is
  rejected.
- The old Kelvin/tint operation and Studio controls are removed as a hard cut.
  Studio exposes the four modes and manual coefficients; QML only forwards
  numeric intents.
- GTK pickers, colored sliders, camera preset UI, OpenCL, dynamic IOP ABI and
  legacy binary preset/XMP ABI are not migrated.

## Consequences

- Default RAW output remains the pinned as-shot baseline while manual and
  camera-reference modes have separate real-RAW references.
- Static schema-v3/v4 fixtures, four-Bayer and X-Trans synthetic patterns,
  RGB/cancellation/error tests, late-CAT equivalence, cache invalidation and
  catalog reopen define the automated contract.
- `legacy/src/iop/temperature.c` and its CMake registration are
  retired after the automated gate passed. Shared color/profile consumers stay
  frozen until their own queue items are accepted.

## Rejected alternatives

- Keep Kelvin/tint as the canonical schema: its display-RGB approximation
  cannot reproduce camera-channel coefficients.
- Apply automatic white balance in both decode and recipe: the result depends
  on call path and doubles the intent.
- Carry the legacy global chroma pointer into color calibration: it violates
  explicit recipe ownership and makes operation order stateful.
