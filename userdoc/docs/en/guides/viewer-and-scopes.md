# Viewer and Scopes

## Goal

Inspect a selected photo at useful zoom levels, navigate large images, and use
the available RGB scopes without changing the source.

**Last verified:** 2026-08-27 against the current Studio viewer and scope
presenter.

## Applies to

- Ravo Studio with an open library and selected photo.
- Gallery, Loupe, and the image surface in Edit.

## Prerequisites

- A library is open.
- A photo is selected and its original is readable for a full preview.

## Gallery

**Gallery** is the thumbnail grid. Each tile can show:

- A thumbnail or a Loading, Failed, or Missing state.
- Its sequence number in the current visible order.
- The media type and dimensions when available.
- Rating stars, a color-label dot, a reject flag, and an edited indicator.

Use the **Size** slider in the Gallery review bar to change thumbnail size. The
current range is 120–320 for the grid control. Select a tile to make it active;
double-click it to open Loupe.

## Loupe

Loupe displays the active photo on the center image surface. Use **View →
Loupe**, double-click a thumbnail, or press `2` to enter it. A single selected
photo is required.

The left navigator shows the whole preview and the current viewport rectangle.
At a zoom that is larger than the available surface, drag the image or drag the
navigator rectangle to pan.

Hovering the photo (not the surrounding letterbox) shows a magnifying glass.
Click the photo to animate to **1:1** with the clicked point kept under the
cursor. Click again to animate back to the Fit, Fill, or custom zoom that was
current before 1:1. Wheel, Fit, Fill, and the 1:1 control still jump to the
requested zoom. Crop mode keeps pointer ownership for crop gestures.
Double-clicking the Loupe image returns to Gallery.

## Zoom modes

The left panel provides these modes:

| Mode | Behavior |
| --- | --- |
| Fit | Shows the complete preview inside the available surface. |
| Fill | Fills the available surface while preserving the image aspect ratio; edges may be outside the viewport. |
| 1:1 | Shows one image pixel at the actual preview scale; panning is expected for large images. |

The mouse wheel adjusts a custom zoom around the current view. The command
palette and View menu expose the same zoom actions. Zoom changes affect the
preview surface, not the original or the stored recipe.

Changing the active photo, browse mode, or zoom mode recenters the viewport.
Changing rating, metadata, or another review value on the same active photo
keeps the current pan. Every navigator seek and direct drag stops at the image
bounds; crop mode temporarily gives drag ownership to the crop overlay.

## Preview scopes

The top of the right Inspector contains the scope panel. Histogram is the
default. The triangle control at the top-left of the plot opens a menu to
switch between:

- **Histogram**, with red, green, and blue channel distributions.
- **Waveform**, with overlaid RGB intensity across image width.
- **Parade**, with separate RGB channel columns across the image width.
- **Vectorscope**, with a fixed linear D50 CIE u*v* chromaticity plot.
- **Split**, with Waveform and Vectorscope together.

Scopes are calculated from the current processed preview. In Edit, they update
after a committed or interactive Develop change. They are diagnostic displays;
Ravo does not treat them as a legacy picker, mask, or automatic exposure
decision.

The Vectorscope is diagnostic-only. It does not expose the old AzBz/RYB,
logarithmic, harmony, profile-selection, picker, or exposure-drag modes.

## Preview loading and missing originals

When a preview is being generated, Studio shows **Loading…** in the relevant
panel and the status bar. A RAW Gallery thumbnail may come from an embedded
camera JPEG, while Loupe and Edit use the processed CPU preview path.

If the source path is no longer readable, the thumbnail and image surface show
**Missing** or **Original file is missing**. The catalog record, rating, tags,
and recipe are retained, but a new preview or export cannot be produced until
the original is available at its recorded path.

## Result

You can move from a library-wide view to a pixel-level review without changing
the source file. Fit/Fill/1:1, click-to-1:1, pan, the navigator, and all five
scopes operate on the current preview.

## Common questions

### Why is 1:1 not the same as the camera's original resolution?

1:1 refers to the current Ravo preview. Preview generation may fit an image to
the requested preview size; it is not an unbounded raw sensor viewer.

### Why do the scopes change after I switch from Gallery to Loupe?

Gallery can use a lightweight RAW embedded preview for thumbnails. Loupe and
Edit request the processed CPU preview, so the displayed pixels and scopes can
change to match the actual Develop path.

### Can I zoom while crop mode is active?

Crop mode owns the image surface for frame manipulation. Finish cropping or
leave crop mode before using normal panning and zoom interaction.
