# ADR-0027: Keep Radiance RGBE input behind a dedicated float contract

- Status: Accepted
- Date: 2026-08-27

## Context

The frozen Radiance RGBE input owner decodes high-dynamic-range samples to
linear floating-point RGB. Ravo's existing `RasterDecoder` and Qt raster path
publish only display-referred RGB8, so routing RGBE through that port would
silently quantize or clamp HDR input before a consumer can apply an explicit
colour and display contract.

This decision covers only the first contract tranche of migration row I9. It
establishes an independently testable decoder boundary while product import,
preview, recipe, and export remain deliberately disconnected. The legacy
wrapper still has a live image-I/O dispatcher consumer and cannot yet be
retired.

## Decision

- `HdrDecoder` is a synchronous domain port that returns owned
  `DecodedHdrRaster` values. Its path view and encoded-byte reference are
  borrowed only for the duration of the call and must never be retained.
  Results own their float pixels and metadata.
- `DecodedHdrRaster` explicitly declares linear RGB float32 and opaque alpha.
  `RadianceRgbeMetadata` contains only owned value state: the program token,
  parsed gamma/exposure provenance, primary chromaticities, and explicit
  RGB-to-XYZ and XYZ-to-RGB matrices. It contains no Qt, legacy, libjpeg,
  libtiff, or other codec handle. Domain owns this semantic result schema;
  Radiance parsing, RLE, resource limits, and file I/O stay private to the
  adapter.
- The adapter accepts the two frozen magic tokens, exact
  `FORMAT=32-bit_rle_rgbe`, canonical `-Y +X` resolution, flat pixels, and the
  new per-channel RLE form. It reproduces the frozen conversion
  `mantissa * 2^(E-136)`, zero exponent handling, final `[0,10000]` clamp,
  non-effective `GAMMA`/`EXPOSURE` provenance, and the legacy `PRIMARIES`
  matrix construction and inverse evaluation order.
- Non-canonical orientation, XYZE, and old-RLE markers are structured
  unsupported states. Rejecting old-RLE markers also rejects marker-like legal
  flat pixels; that is a deliberate product incompatibility, not a claim of
  bit-exact acceptance for that subdomain. Duplicate format declarations,
  malformed headers, invalid primaries, broken packets, truncation, and
  resource-limit failures fail closed with stable structured context.
- Decode applies fixed upper bounds to encoded input, header/line size, and
  decoded allocation, checks cancellation at bounded intervals, and leaves
  path and memory sources unchanged. Path and memory decoding share the same
  parser and pixel contract.
- The Qt RGB8 raster adapter recognizes either RGBE magic and returns
  `unsupported` with `format=rgbe` and
  `reason=unsupported_rgbe_input`. Catalog import preserves that result,
  bypasses RAW fallback, and publishes no asset or preview. The dedicated
  float decoder is not yet a product import or preview route.

## Consequences

Tranche 1 can verify frozen float decode behavior without misrepresenting HDR
as ordinary RGB8 or prematurely expanding the engine. Dedicated tests pin
float bits, matrices, metadata provenance, RLE/flat boundaries, errors,
cancellation, resource bounds, path/memory parity, and source immutability.
Qt and Catalog tests pin both magic tokens and zero publication.

This decision does not complete I9. A later product tranche must explicitly
define HDR import identity, persistence, engine/preview colour handling, and
display/export ownership before Catalog may publish RGBE assets. The dynamic
consumer census must reach zero and that replacement evidence must be accepted
before `legacy/src/imageio/imageio_rgbe.c` or its registration can be retired.

## Rejected alternatives

- Decode RGBE through `RasterDecoder`: its RGB8 result cannot preserve the
  frozen linear HDR samples.
- Treat a recognized RGBE file as RAW after Qt rejects it: LibRaw owns camera
  RAW, and fallback would replace a precise unsupported result with an
  unrelated error or preview.
- Apply standards-oriented half-step conversion, orientation expansion,
  `GAMMA`/`EXPOSURE` correction, or old-RLE support in this tranche: those are
  future product decisions outside the frozen default CPU contract.
- Put Qt images, decoder objects, legacy structs, or third-party colour handles
  in domain state: that would leak adapter implementation and lifetime into a
  public port.
- Retire the legacy wrapper after decoder tests alone: the live dispatcher and
  separate shared identities remain independent acceptance gates.
