# ADR-0161: Full-source crop workspace

Date: 2026-09-27
Status: Accepted

## Decision

During crop rotation, show the complete transformed source rather than the
automatic safe crop. Fit uses the source diagonal, independent of angle, so
rotation does not continually zoom the remaining photo. The crop frame still
addresses the canonical constrained output coordinates used by saved recipes
and export.

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

Verify full source extent and angle-independent scale through successive
rotations, canonical crop-frame mapping, release/save and crop exit/reentry.
Check that preview planning leaves the source recipe unchanged and cannot
publish a persisted crop-workspace cache. Run Engine/service contracts, desktop
command tests, QML smoke, code-quality checks and the complete Ravo suite.
