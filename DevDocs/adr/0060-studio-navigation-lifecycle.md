# ADR-0060: Studio owns bounded photo navigation and viewport reset lifecycle

- Status: Accepted
- Date: 2026-08-29
- Extends: [ADR-0007](0007-first-usable-catalog-viewer.md)

## Context

The old `libs/navigation.c` combined a GTK thumbnail, global view/develop
proxies, pan gestures, and editable zoom presets. Studio already had one
Flickable photo surface, Fit/Fill/100%/custom zoom, wheel zoom, and a Library
navigator, but its pan reset timing was implicit.

## Decision

- `StudioPresenter` is the sole zoom-state owner. Fit, Fill, Actual, and Custom
  are canonical modes; custom factor clamps to 0.1–8 and wheel steps by 1.1.
- QML owns only presentation geometry. Its Flickable stops at bounds, and the
  navigator derives a normalized visible rectangle and clamps seek requests to
  current content extents.
- The presenter publishes a stable viewport extent with each accepted preview.
  A lower-resolution interactive preview keeps the preceding settled maximum
  edge while adopting any changed aspect ratio; the settled preview replaces
  that extent. QML never derives pan geometry from an `Image` resource's
  transient implicit size while its source is changing.
- A changed active asset ID, browse mode, or zoom state recenters the viewport
  after layout. Rating, metadata, or other `selectionChanged` notifications for
  the same active asset do not interrupt the user's pan.
- Grid selection remains independently revealed only when it leaves the grid
  viewport. Crop mode disables photo panning so crop gestures keep ownership.
- Commands remain the only external zoom intent path. No pan/zoom state is
  persisted to the catalog or inferred from the removed GTK configuration.

## Consequences

The old GTK navigation module and registration are removed. Shared old view
proxies, zoom configuration keys, paint code, and global Develop state remain
for their other D0/U consumers. Studio's left navigator is not a compatibility
wrapper; it is a projection of the current QML photo surface.

## Rejected alternatives

- Keeping pan coordinates across different photos. Equal dimensions would
  silently show a stale region of a new asset.
- Resetting on every `selectionChanged` signal. Review edits reuse that signal
  and must not disrupt inspection.
- Porting the GTK thumbnail/proxy or catalog-persisting viewport telemetry.
