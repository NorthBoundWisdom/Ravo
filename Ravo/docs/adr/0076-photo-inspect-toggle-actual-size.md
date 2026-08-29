# ADR-0076: Photo inspect click toggles 1:1 and restores the current view

- Status: Accepted
- Date: 2026-08-29
- Extends: [ADR-0060](0060-studio-navigation-lifecycle.md)

## Context

Studio already owned Fit, Fill, Actual, and custom zoom through commands, with
wheel zoom and a left navigator. Inspecting a large photo still required the
zoom control or wheel to reach 1:1. The inspect surface needed a pointer
affordance that zooms to the clicked pixel and returns to the view the user
was already using.

## Decision

- Hovering the decoded photo (not the surrounding letterbox) shows a
  magnifying-glass pointer. Plus means the next click goes to Actual; minus
  means the next click restores the last non-Actual mode.
- A click on that photo is a zoom intent. `StudioPresenter::toggleActualSize`
  remains the zoom-state owner: leaving Actual restores the last Fit, Fill, or
  custom factor; the 1:1 control still sets Actual absolutely.
- QML owns only the pointer geometry and a short GPU scale/pan animation for
  inspect click. Zoom-in keeps the clicked image point under the cursor;
  zoom-out restores the pan captured before entering Actual. The preview
  stage size is locked for the duration of that animation so the Image is not
  relaid out every frame; the real Fit/Fill/Actual/custom layout is applied
  once when the animation finishes. Asset, browse-mode, wheel, and Fit/Fill/1:1
  changes abort the animation and still recenter as in ADR-0060.
- Crop mode keeps pointer ownership. Loupe double-click still returns to
  Gallery; a delayed single click distinguishes the two gestures there.
- `studio.view.toggle_actual_size` is the only new command. It is not a fourth
  zoom radio and is not persisted.

## Consequences

The inspect surface can reach 1:1 without changing the Fit/Fill/1:1 owner or
catalog state. Shared old GTK zoom configuration remains untouched.

## Rejected alternatives

- Always returning to Fit. Fill and custom views are the user's current view.
- Replacing Fit/Fill/1:1 with a toggle. Those remain explicit modes.
- Porting GTK zoom-in/out cursors or persisting the pre-1:1 pan in the catalog.
