# ADR-0034: Own typed TIFF export options and a bounded encoder

- Status: Accepted
- Date: 2026-08-27

## Context

The frozen `legacy/src/imageio/format/tiff.c` owner stores sample precision,
compression, compression level, and conditional black-and-white output as
format parameters. Its defaults are unsigned 8-bit samples, Deflate with the
horizontal predictor, compression level 6, and RGB output. The main image is a
classic little-endian, top-left, contiguous-strip TIFF without alpha. It
embeds the resolved RGB ICC profile and records 300 dpi under the frozen
configuration used by this migration tranche.

Ravo previously delegated TIFF output to `QImageWriter`, so the public request
could not express the frozen options and codec configuration depended on the
installed Qt TIFF plugin. The current rendered result is explicitly RGB8;
claiming unsigned 16-bit, float16, or float32 output from that buffer would
invent a higher-precision source contract.

This is an I13 core adapter tranche. It does not complete metadata, mask-page,
storage, or legacy-owner retirement work.

## Decision

- Domain owns `TiffSampleType` with canonical `uint8`, `uint16`, `float16`, and
  `float32` states, and `TiffCompression` with canonical `none`, `deflate`, and
  `deflate_predictor` states. `TiffExportOptions` defaults to `uint8`,
  `deflate_predictor`, level 6, and disabled conditional grayscale. One strict
  validator accepts compression levels 1–9 and only the declared enum values.
- `ExportRequest` carries the complete value. `CatalogService` validates it
  only for TIFF and passes it by const reference through `RasterDecoder` to the
  Qt adapter. JPEG and PNG ignore TIFF-specific state.
- The output codec is an adapter-private static LibTIFF built only from the
  pinned `RAVO_LIBTIFF_SOURCE_ROOT`. Its isolated build enables classic TIFF
  plus the already-owned ZLIB Deflate codec and disables shared libraries,
  C++, tools, tests, documentation, and unrelated codecs. There is no
  host-selected TIFF fallback and no third-party type crosses the adapter.
- The RGB8 path writes classic little-endian TIFF with unsigned 8-bit samples,
  top-left orientation, contiguous scanline strips, no tiles or alpha, 300 dpi,
  and one exact resolved RGB ICC profile. Compression is uncompressed or Adobe
  Deflate with the requested level and either no predictor or horizontal
  differencing.
- Conditional grayscale exactly retains the frozen 8-bit decision: it runs
  only when both dimensions exceed four, ignores the one-pixel border, and
  keeps RGB when any interior pair of channels differs by more than two. A
  grayscale file stores the source red sample and `MINISBLACK`; it does not
  invent a luminance conversion.
- An RGB8 source still returns `reason=unsupported_tiff_high_precision_source`
  for `uint16`, `float16`, and `float32`. No 8-to-16 expansion or
  integer-to-float conversion is performed. ADR-0037 maps matching product
  requests to engine-owned RGB16 or finite RGB float and a shared TIFF sample
  encoder, including owned IEEE binary16 conversion.
- Dimensions, checked RGB products, source bytes, ICC bytes, LibTIFF
  allocations, and encoded output are bounded. `TIFFClientOpenExt` owns one
  checked in-memory read/write/seek/size/close client and per-handle
  error/warning state; the encoder does not mutate LibTIFF global handlers.
  Callback failures, finalization, allocation, and close failures return no
  encoded vector.
- Cancellation is checked at entry, every scanline, and before finalization.
  The synchronous call borrows pixels, ICC, options, cancellation state, and
  the private `noexcept` test observer only for the call. Success returns a
  separately owned byte vector and mutates none of the inputs.
- Encoded files continue to use the atomic no-replace publication contract in
  [ADR-0032](0032-encoded-byte-publication-contract.md). The codec neither
  publishes paths nor changes that owner.

## Consequences

TIFF defaults and the currently representable RGB8 behavior no longer depend
on Qt writer defaults. Service callers have one typed immutable request, and
tests independently parse tags, inflate strips, and compare exact pixels and
ICC bytes while covering propagation, cancellation, failure, bounds, and
source immutability.

I13 remains incomplete. ADR-0037 now supplies unsigned-16 and floating
rendered sources for matching product requests. This tranche does not
construct EXIF, IPTC, or XMP, write multipage mask IFDs, expose typed TIFF
options in Studio, own path templates or batch presets, or retire shared
image-I/O/storage/job consumers. The
`QTiffPlugin` remains required for the separate I7 input owner.
`legacy/src/imageio/format/tiff.c` and its registration therefore remain.

## Rejected alternatives

- Keep TIFF output in `QImageWriter`: plugin defaults do not expose the frozen
  schema, per-stage failures, or deterministic compression contract.
- Expand RGB8 into a nominal high-precision TIFF: that fabricates precision
  and hides the missing rendered-source owner.
- Use process-global LibTIFF handlers: concurrent codec calls would share
  mutable diagnostic state and violate adapter isolation.
- Add metadata, multipage masks, CLI options, or storage policy to this codec
  tranche: those owners require separate schema and publication evidence.
