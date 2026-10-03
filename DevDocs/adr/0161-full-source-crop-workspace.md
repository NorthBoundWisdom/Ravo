# ADR-0161: Full-source crop workspace

Date: 2026-09-27
Status: Accepted

## Decision

During crop rotation, show the complete transformed source rather than the
automatic safe crop. Fit retains half of the diagonal-safe surround toward
the contained transformed source, preserving aspect ratio. The crop frame still
addresses the canonical constrained output coordinates used by saved recipes
and export.

**2026-10-03 presentation amendment:** Fit now places the photo halfway between
the diagonal-safe size and the contained transformed-source size, halving the
extra rotation surround while preserving aspect ratio. The contained extent
may change with rotation; the crop coordinate and pixel contracts stay fixed.
While crop is active, common crop controls remain expanded below scopes and
above the independent Develop scroller. A short inspector scrolls the pinned
controls independently rather than hiding the remaining Develop stack. The
shared crop controls also serve the ordinary Geometry section outside crop.
Auto Level uses Engine's `kLevel` analysis mode: the existing horizontal-line
detector/objective fits rotation alone, with no shift/shear fit. It commits only its
rotation, preserving authored perspective and crop parameters.
Level detection widens normal-guided Hough voting to eight degrees to recover
shallow tilt from quantized staircase edges; perspective modes keep two degrees.
Supporting-pixel covariance refines level line directions beyond the Hough
accumulator's one-degree bins; the existing robust fit still rejects outliers.
Level uses a half-maximum dominant-line vote threshold to reject inter-edge
chords, and a single valid horizontal guide suffices for its one-variable fit.
Existing worker supersession, selection cancellation, and explicit no-solution
errors apply.

Engine `plan_crop_preview` reuses the existing Canvas and Perspective layout
owners to produce a nonpersistent recipe and its canonical crop-frame mapping.
Output frame decoration is excluded from the crop source.
The Engine also derives source coverage using the same inverse homography as
Perspective sampling. Pixels outside the source receive a neutral `#767676`
workspace surround after rendering; real black photo pixels are preserved.
The crop overlay dims this surround along with the rest of the exterior.
This presentation uses owned CPU bytes, not an uncomposited native surface.

Services admit this plan only for uncached, nonpersistent, non-ROI crop previews.
Ordinary preview and export keep their existing recipes and output geometry.

The desktop presenter publishes geometry with the matching accepted image.
QML binds the photo extent and crop overlay to that immutable result; it owns
no homography or pixel algorithm. Existing request cancellation, selection
generation checks and worker destruction apply. Invalid requests/layouts fail
explicitly; no alternate renderer or fallback is added.

## Validation

Verify full source extent, aspect-preserving half-surround Fit through successive
rotations, canonical crop-frame mapping, release/save and crop exit/reentry.
Check that preview planning leaves the source recipe unchanged and cannot
publish a persisted crop-workspace cache. Run Engine/service contracts, desktop
command tests, QML smoke, code-quality checks and the complete Ravo suite.
