# ADR-0116: Histogram-assisted parametric mask authoring

- Status: Accepted
- Date: 2026-09-03
- Extends: [ADR-0044](0044-studio-canonical-mask-authoring.md),
  [ADR-0061](0061-engine-owned-preview-scopes.md),
  [ADR-0109](0109-masked-exposure.md),
  [ADR-0114](0114-mask-click-placement.md)

## Context

Accepted everyday operations may attach one owned canonical mask, and Studio
already authors parametric leaves through numeric Source, Channel, and
Threshold fields. A photographer still cannot pick a tone on the photo and have
C++ write those thresholds. PRO-LOCAL asked for histogram-assisted parametric
authoring without QML-owned mask pixels. Click placement of spatial geometry is
already accepted (ADR-0114). Path/brush stroking, multi-instance grading, AI
masks, and PERF work remain separate.

Studio already owns the ADR-0061 display RGB histogram on the processed
preview. Parametric evaluation already owns a four-keyframe channel ramp.

## Decision

- C++ owns histogram-assisted parametric authoring. QML may draw an Assist
  control and forward a click; it must not sample pixels or invent thresholds.
- Studio owns a session `maskParametricAssistActive` flag. When it is on, a left
  click on the displayed Develop photo forwards normalized preview coordinates
  (`0…1` in the visible photo rectangle) through
  `studio.edit.assist_parametric_mask`. Crop tool, white-balance pick, and
  mask click placement cannot be on at the same time.
- Assistance is allowed only for an editable attached parametric leaf on a
  consumer this ADR authorizes. Channel and Source stay those of the attached
  leaf; the pick writes only Threshold0…3 through the existing strict
  mask-field helper and Develop preview/commit lifecycle.
- The pick samples the current processed preview RGB8 buffer that feeds scopes.
  Channel extraction matches the ADR-0061 histogram contract (display codes;
  luminance uses the same Rec.709 display weights as `collect_rgb_histogram`).
  When the live scope histogram for that channel is available, C++ expands a
  contiguous density band around the sampled bin; otherwise it uses a fixed
  soft band around the sample. Thresholds remain finite, normalized, and
  monotonic.
- Every accepted masked everyday consumer may use this path (Exposure, Color
  Balance RGB, RGB Curve, Tone Curve, Highlights, Shadows, Whites, Blacks).
  Color Harmonizer and Graduated ND reject with a structured reason.
- Canvas, Perspective, straighten, rotate, and flip reject with a structured
  reason rather than an approximate inverse into the parametric source plane.
  Invalid, out-of-range, missing-preview, unattached, non-parametric, or
  unsupported-target clicks fail closed and do not mutate Develop. Domain
  fitting of thresholds at 0/1 uses equal adjacent keys (documented hard edge),
  not silent out-of-range publication.
- No multi-instance Exposure or other multi-instance grading is introduced.
  Path, brush, group-child parametric assist, AI masks, and PERF remain out of
  scope.

## Consequences

A photographer can click the photo to author parametric thresholds for every
authorized everyday consumer through the same recipe CLI/Studio path used for
numeric Threshold edits. Unmasked pixels stay unchanged. Color Harmonizer and
Graduated ND remain fail-closed without a second picker owner.

## Rejected alternatives

- QML-owned preview sampling or writing Threshold sliders from presentation
  math.
- Inventing a Perspective/Canvas inverse, or clamping unsupported geometry into
  a guessed channel sample.
- Waiting for multi-instance, path/brush, or AI masks before everyday assist.
- A second parametric threshold model disconnected from ADR-0061 scopes.
