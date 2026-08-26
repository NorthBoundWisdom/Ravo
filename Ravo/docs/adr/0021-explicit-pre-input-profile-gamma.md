# ADR-0021: Correct profile transfer before input colour conversion

- Status: Accepted
- Date: 2026-08-26

## Context

The frozen `profile_gamma` module exists for input profiles that expect
non-linear RGB samples even though the decoded source is linear. Its enabled
CPU path is absent from all 158 frozen XMP histories, so there is no proven
parameter payload or historical look to import. The old RAW order places the
module before `colorin`; the mechanically reordered JPEG pipeline places it
after `colorin`. Ravo needs one explicit, cacheable meaning rather than a
media-dependent hidden scheduler rule.

The existing `ravo.core.gamma` is a separate one-parameter display adjustment
and is not an implementation of this operation.

## Decision

- `ravo.color.profilegamma` v1 stores `mode`, `linear`, `gamma`,
  `dynamic_range`, `grey_point`, `shadows_range`, and `security_factor`.
  `mode` is explicitly `logarithmic` or `gamma`; all fields remain present in
  either mode. Absence or a disabled operation is the identity. Enabled default
  logarithmic parameters are not an identity.
- The canonical operation immediately precedes `ravo.color.input`. For RAW it
  runs after white balance/demosaic on camera RGB; for raster it runs on decoded
  encoded RGB before input-profile conversion. This deliberately chooses the
  input-profile-correction meaning for both source types. Ravo does not infer a
  different order from media type or preserve the unproven old JPEG ordering.
- Logarithmic mode retains the CPU `2^-16` floors and the frozen float
  `fastlog2` approximation. Gamma mode retains the 65,536-entry float table,
  piecewise linear/power construction, clamped integer lookup below one, and
  frozen four-sample exponential extrapolation at and above one.
- The engine derives constants, LUT, and extrapolation coefficients for one
  synchronous render. It reads an immutable profiled RGB buffer, produces an
  owned buffer, preserves its exact `ColorProfileState`, and checks
  cancellation by row and while building the LUT. The operation participates
  in the scene-linear preprocess cache key and is never applied again to a
  cached working buffer.
- `security_factor` is retained because it belongs to the frozen schema, but it
  does not participate in the pixel transform. The old GTK picker/autotune and
  its parameter-coupling UI are unsupported as of 2026-08-26. A future version
  requires an explicit engine/service analysis request with an immutable
  pre-operation pixel source, normalized ROI, statistics version,
  cancellation, recipe revision, and atomic save. Display scopes or QML
  histograms cannot supply that result.
- Studio exposes manual enable, mode, and the parameters that directly affect
  the selected mode. QML only presents values and forwards intents; it owns no
  LUT, source-profile, picker, or statistics logic.

## Consequences

Missing/unknown modes, missing or out-of-range parameters, invalid dimensions,
non-finite input/LUT/coefficients/output, allocation failure, and cancellation
return structured failure without mutating the input or publishing partial
pixels. No logarithmic/gamma fallback is allowed.

Because no frozen enabled history exists, legacy XMP that names
`profile_gamma` is rejected explicitly rather than decoded from an invented
payload. Tagged raster and RAW references are Ravo-owned evidence, not claims
of a historical golden.

## Rejected alternatives

- Reuse `ravo.core.gamma`: its schema, position, exponent convention, LUT, and
  extrapolation do not match the frozen owner.
- Run after `ravo.color.input`: that cannot correct a source profile that
  expects non-linear input and changes the RAW operation order.
- Select pre- or post-input order from media type: the recipe would no longer
  describe one stable operation and cache identity would depend on hidden
  scheduler state.
- Recreate picker/autotune from the displayed preview: it observes the wrong
  pixel stage and has no stable ROI or revision contract.
