# ADR-0070: Canvas owns the content frame and Frame owns final output margins

- Status: Accepted
- Date: 2026-08-29
- Extends: [ADR-0043](0043-canonical-mask-graph-foundation.md), [ADR-0069](0069-post-output-dither-posterize.md)

## Context

The frozen Enlarge Canvas and Borders IOPs both grow image dimensions, but they
operate in different colour and coordinate domains. Canvas is scene-linear
geometry whose added area must stay outside later canonical masks. Frame is an
encoded-output decoration whose border, aspect, position, and optional line
must be applied after Output Color and Dither. Treating both as an encoder
padding option would lose recipe state and make mask coordinates ambiguous.

## Decision

- `ravo.geometry.canvas` schema v1 declares `linear_rec709`,
  `frozen_enlargecanvas_v1`, four independent 0–100 percent extents, and one of
  the five frozen solid colours. Integer truncation, the 5-pixel minimum, the
  three-times dimension cap, asymmetric placement, and opaque RGB fill follow
  the frozen CPU path.
- Canvas publishes an immutable attached content frame. Canonical masks and
  Retouch source coordinates continue to evaluate against the original photo
  inside that frame, then pad the added area with alpha zero. The current
  evaluator accepts this transform only for a full-frame ROI; sub-ROI
  evaluation, a nested Canvas, masks on Canvas itself, and enabled geometry
  after Canvas fail explicitly. G1/G4/G6 must define composed geometry before
  those combinations become supported.
- `ravo.output.frame` schema v1 declares `encoded_output_rgb`,
  `frozen_borders_v4`, border and line colours, constant/image/custom aspect,
  orientation, width/height/shorter/longer/automatic basis, size, image
  position, line size, and line offset. It is the final recipe operation after
  Output Color and optional Dither and before sample packing.
- Frame retains the frozen float narrowing, round/truncate order, three-times
  dimension cap, and border-helper line endpoints, including their asymmetric
  integer edge behavior. Preview mask overlays are padded with zero alpha to
  the framed output. The encoded profile and Catalog metadata snapshot remain
  unchanged; JPEG, PNG, and TIFF encoders only consume the resulting dimensions
  and pixels.
- Both operations validate schemas, dimensions, buffers, profiles, every
  finite sample, derived sizes, and allocation before publication. Input
  validation and output rows are cancellable; a final cancellation checkpoint
  prevents publication. Raw resource estimation includes the peak Canvas/Frame
  pixel ratio. No GPU path or silent repair is added.
- Strict XMP import accepts only the exact enabled singleton evidence: Canvas
  v1 from 0157, Borders v3 from 0030, and Borders v4 from 0154/0155, with their
  exact compressed layout, unused/reserved fields, `max_border_size=true`, and
  versioned default blends. Other versions, masks, custom blends, duplicate or
  multi-instance state, and modified payloads reject.
- Recipe/Develop/CLI/Catalog/Studio/styles share the same state. Studio places
  Canvas in Geometry and Frame in Effects; no QML pixel or layout mathematics
  is authoritative.

## Consequences

G5 and G8 are accepted. The old `iop/enlargecanvas.c`, `iop/borders.c`, their
shared `develop/borders_helper.{c,h}`, registrations, exclusive
`borders_fill` OpenCL kernel, and darkroom icons are removed. `basic.cl`
remains for other frozen kernels. The Overlay literal reference plus old
order, module-group, and manual names remain owned by M3/D0 until those shared
files reach their own deletion gates.

Canvas followed by another geometry operation remains an explicit unsupported
state, not a compatibility fallback. Frame is not transparent padding and does
not invent an alpha-capable export contract.

## Rejected alternatives

- Merge Canvas and Frame into one operation. Their colour domains, ordering,
  mask behavior, and user intent differ.
- Recompute masks over the enlarged dimensions. That changes normalized
  spatial masks and would select newly added pixels.
- Put Frame in each encoder. That duplicates layout behavior and makes preview,
  high-precision export, and style persistence disagree.
- Silently run later geometry while retaining stale attached-frame coordinates.
  Unsupported composition must fail until its transform is explicitly owned.
