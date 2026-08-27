# ADR-0037: High-precision export pixel contract

- Status: Accepted
- Date: 2026-08-27

## Context

ADR-0033 and ADR-0034 already own typed PNG 8/16 and TIFF
uint8/uint16/float16/float32 requests, plus bounded private encoders. Product
export still rendered only RGB8, so PNG 16-bit and TIFF high-precision names
failed closed rather than inventing precision from packed 8-bit samples.

The engine already owns a finite `ProfiledOutputBuffer` after recipe and
output-colour conversion. That buffer is the only legitimate source of new
precision. Preview, browse cache, and QML must remain RGB8.

This is an I12/I13 product-export tranche. It does not construct EXIF/IPTC/XMP
packets, PNG pHYs, TIFF multipage masks, Studio option controls, or retire the
legacy PNG/TIFF output plugins.

## Decision

- Keep `RenderedImage` RGB8-only for preview and cache. Export uses a tagged
  `RenderedExportImage` / domain `ExportPixelBuffer` whose `std::variant`
  holds exactly one of RGB8, RGB16, or finite RGB float.
- The engine owns `RenderSampleKind { kRgb8, kRgb16, kRgbFloat }` and packs
  one shared profiled-output stage. Integer packing is
  `round(clamp(sample, 0, 1) * 65535)` for uint16 and the existing 255-scale
  rule for uint8. Float32 preserves finite post-output-colour values.
- CatalogService maps format and options to a sample kind. JPEG, PNG8, and
  TIFF uint8 stay RGB8. PNG16 and TIFF uint16 request RGB16. TIFF float16 and
  float32 request finite RGB float. Original copy still does not render.
  Format-to-sample mapping never enters the engine or domain.
- The raster port adds a tagged encode with no default virtual fallback.
  QtRasterDecoder and every test double implement it explicitly. RGB8 plus a
  PNG16 or TIFF high-precision request still returns the existing structured
  unsupported reasons. Matching high-precision sources dispatch to
  `encode_png_rgb16` or the shared TIFF sample encoder.
- The TIFF adapter owns one checked sample descriptor, shared LibTIFF
  lifecycle, precision-specific grayscale (dims both > 4, ignore 1 px border,
  abs channel diff > 2 uint8 / > 165 uint16, 1.01 ratio with 0.001 floor for
  float; grayscale stores the red sample), and an owned IEEE-754 binary16
  converter. Conversion is round-to-nearest-even, preserves signed zero, and
  fails structurally when a finite source would become Inf/NaN (threshold
  65520). No Imath, copied half implementation, or new dependency is added.
- Memory estimates use 3, 6, or 12 output bytes per pixel and saturate instead
  of wrapping when a requested size exceeds `uint64_t`. There is no silent
  0→3 fallback and no RGB8×257 or RGB8-to-float expansion.

## Consequences

Product PNG 16-bit and TIFF uint16/float16/float32 export now succeed from
engine-owned samples. Preview/QML remain RGB8. Mismatched sources continue to
fail closed. I12/I13 remain incomplete: S9/J6 metadata packets, PNG pHYs,
TIFF multipage masks, explicit Studio TIFF/PNG controls, shared
imageio/storage/job consumers, and plugin retirement are unchanged.

## Rejected alternatives

- Expand RGB8 by 257 or promote packed 8-bit samples to float: that fabricates
  precision the engine never rendered.
- Put high-precision variants on `RenderedImage` or the preview cache: that
  forces unused sample width into QML and browse PNG.
- Add a default virtual raster encode that silently rejects unknown sources:
  that hides unimplemented test doubles.
- Depend on Imath or copy a third-party half implementation: the adapter owns
  a small reviewed binary16 policy instead.
