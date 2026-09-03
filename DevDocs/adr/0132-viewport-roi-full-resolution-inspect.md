# ADR-0132: Viewport ROI full-resolution inspect

- Status: Accepted
- Date: 2026-09-03
- Extends: [ADR-0060](0060-studio-navigation-lifecycle.md),
  [ADR-0076](0076-photo-inspect-toggle-actual-size.md),
  [ADR-0087](0087-progressive-develop-preview.md)
- Relates to: [MIGRATION.md](../MIGRATION.md) GPU row

## Context

Studio Actual Size previously displayed the settled 1600px preview at one CSS
pixel per preview pixel. That is not sensor 1:1. A full-sensor CPU render of a
60MP RAW is tens of seconds. RapidRAW's 100% is screen 1:1; its viewport ROI
applies only while dragging, and settled HiFi still processes a whole preview
frame on the GPU. Ravo has no GPU adapter. The only CPU path that can look like
1:1 and stay interactive is to demosaic the visible crop at native density.

## Decision

- `PreviewRequest` may carry a normalized ROI in the current cropped, display-
  oriented photo (`x,y,width,height` in `[0,1]`). The CLI is the baseline
  client: `catalog preview --roi x,y,w,h` with optional no-replace `--output`.
  ROI previews never write the catalog preview-cache PNG.
- CatalogService maps that rectangle through the enabled crop onto uncropped
  display pixels, then through decode rotation onto the CFA, expands a demosaic
  border, and asks Engine for a 1:1 window. Point operations and Sharpen run on
  that window at canonical ROI scale 1. Crop/rotate/flip in the RGB recipe are
  disabled because the window is already the visible crop.
- Identity crop, EXIF/decode rotation, and Bayer CFA are supported. Enabled
  lens, perspective, canvas, straighten, non-identity recipe rotate/flip,
  vignette, dehaze, retouch, color reconstruction, X-Trans, and raster sources
  reject with a structured reason. Studio then keeps the 1600px settled preview
  rather than inventing a second algorithm.
- An ROI that covers the full frame (≥0.8 on both edges) is rejected so callers
  use the settled 1600 path instead of a 60MP CPU PNG.
- Studio Actual Size sizes the inspect stage from working pixels / device pixel
  ratio (screen 1:1). Fit/Fill still use the 1600 preview. While Actual, desktop
  C++ requests the visible viewport as ROI, places the result on the stage, and
  cancels in-flight ROI work on pan, selection change, leaving Actual, or close.
- GPU display remains deferred. This ADR does not add a GPU adapter.

## Consequences

Loupe 100% can show true sensor pixels in the viewport without rendering the
whole frame. Recipes with spatial geometry keep today's 1600 Actual Size. Cache
identity, export, and settled 1600 PNG are unchanged.

## Rejected alternatives

- Rendering the full sensor then cropping. That is the 30s Debug path.
- Copying RapidRAW's settled full-frame GPU HiFi. Ravo has no GPU adapter yet.
- Upsampling the 1600 preview to screen 1:1 and calling it native.
- Inferring the ROI from the foreground window or process list.
