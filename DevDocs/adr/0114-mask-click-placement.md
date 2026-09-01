# ADR-0114: Click placement authors spatial mask geometry

- Status: Accepted
- Date: 2026-09-01
- Extends: [ADR-0044](0044-studio-canonical-mask-authoring.md),
  [ADR-0045](0045-studio-mask-overlay-group-path.md)

## Context

Everyday operations can attach a canonical mask, and Studio already authors
circle, ellipse, and linear-gradient leaves through numeric fields. A
photographer still cannot click the photo to place that shape. PRO-LOCAL asked
for C++-owned picker/histogram assistance. Histogram-assisted parametric
thresholds, path/brush stroking, and inverse mapping through Perspective,
Canvas, straighten, rotate, or flip remain separate work.

White-balance pick already forwards normalized preview coordinates through a
command; QML does not sample pixels.

## Decision

- Studio owns a session `maskPlaceActive` flag. When it is on, a left click on
  the displayed Develop photo forwards normalized preview coordinates
  (`0…1` in the visible photo rectangle) through
  `studio.edit.place_mask`. Crop tool and white-balance pick cannot be on at
  the same time.
- Placement is allowed only for an editable attached circle, ellipse, or
  linear-gradient on the current overlay target. Circle/ellipse write
  `CenterX`/`CenterY`; linear-gradient writes `AnchorX`/`AnchorY`. Both go
  through the existing strict mask-field helper and Develop preview/commit
  lifecycle. Overlay stays preview-only session state.
- Preview coordinates map into the attached frame as
  `crop_origin + preview * crop_size`. Canvas, Perspective, straighten,
  rotate, and flip reject with a structured reason rather than an approximate
  inverse. Invalid, out-of-range, unattached, or unsupported-kind clicks fail
  closed and do not mutate Develop.
- QML draws a Place-on-photo control, forwards the click, and does not own
  mask mathematics. Ellipse center and feather fields apply to ellipse leaves
  (they already appeared in the editor map).

## Consequences

A radial, elliptical, or gradient mask can be positioned by clicking the photo
when geometry is identity besides crop. Histogram pickers, path/brush drawing,
and placement after Canvas/Perspective remain out of scope.

## Rejected alternatives

- QML-owned mask pixels or writing Center/Anchor sliders from presentation math.
- Silently ignoring crop, or inventing a Perspective inverse in this tranche.
- Waiting for histogram-assisted parametric authoring before click placement.
