# ADR-0033: Own typed PNG export options and a bounded encoder

- Status: Accepted
- Date: 2026-08-27

## Context

The frozen `legacy/src/imageio/format/png.c` owner stores bit depth and
compression as format parameters. It accepts 8-bit or 16-bit RGB, defaults to
8-bit, accepts compression levels 0–9 with a default of 5, writes no alpha or
interlace, embeds the resolved RGB ICC profile, and writes identity/full-range
cICP for recognized colour encodings.

Ravo previously sent PNG output through `QImageWriter` with an untyped fixed
compression value. `ExportRequest` could not carry the frozen options and the
raster port could not distinguish an intentional 16-bit request. The current
engine result is explicitly RGB8, so treating that buffer as a 16-bit source
would invent precision rather than migrate the old contract.

This is an I12 core adapter tranche. It does not complete export metadata or
retire the legacy PNG output plugin.

## Decision

- Domain owns `PngBitDepth` with stable values `8` and `16`, canonical names,
  and strict parsing. `PngExportOptions` defaults to 8-bit and compression 5.
  One validator accepts exactly both declared depths and compression 0–9.
- `ExportRequest` owns the complete options value. `CatalogService` validates
  it only for PNG and passes it by const reference through `RasterDecoder` and
  the Qt adapter. JPEG and TIFF ignore PNG-specific state.
- The adapter-private encoder uses the already required `PNG::PNG` and
  `ZLIB::ZLIB` targets. It writes non-interlaced RGB without alpha, fixes the
  frozen zlib configuration, applies the requested compression, embeds one
  resolved RGB ICC profile, and emits identity/full-range cICP only for an
  exact recognized built-in profile state. A valid custom RGB ICC is preserved
  without invented cICP even when its display identifier collides with a
  built-in name; an unknown built-in or non-RGB profile fails closed.
- The current RGB8 source accepts only `bit_depth=8`. `bit_depth=16` remains a
  legal request value but returns
  `reason=unsupported_png_16bit_source`; no 8-to-16 expansion is performed.
- Dimensions, RGB source bytes, ICC bytes, and encoded output are bounded
  before publication. The synchronous call borrows pixels, profile bytes,
  options, cancellation state, and the private test observer only for the
  call; success returns a separately owned byte vector and mutates none of the
  inputs.
- Cancellation is checked at entry, every row, and before finalization.
  Validation, resource, cancellation, injected, and libpng failures return no
  encoded result. A private synchronous `noexcept` function-pointer observer
  freezes compression propagation and deterministic row failure/cancellation
  tests without entering the public port.
- All C++ objects with non-trivial destruction are created before the libpng
  `setjmp` boundary. The code reachable through libpng callbacks uses only POD
  locals and raw bounded storage; callbacks contain allocation failure and do
  not let C++ exceptions cross the C ABI or a `longjmp` skip object lifetimes.
- Encoded files continue to use the atomic no-replace publication contract in
  ADR-0032. Codec success alone never makes partial bytes visible.

## Consequences

PNG defaults and compression now match the frozen owner, service callers have
one typed immutable request, and PNG output no longer depends on Qt writer
defaults. The adapter emits exact RGB8 pixels, a resolved ICC profile, and only
evidenced cICP declarations while preserving explicit failure and cancellation
state.

I12 remains incomplete. Ravo has no 16-bit rendered source contract, and this
tranche does not construct EXIF/IPTC/XMP, resolution metadata, sidecars, path
templates, batch presets, or parent-directory durability. The input wrapper
owned by I6 is separate. `legacy/src/imageio/format/png.c`, its registration,
and shared dynamic image-I/O/storage/job consumers therefore remain.

## Rejected alternatives

- Keep PNG options in Qt writer properties: that would leave no stable public
  value contract and would continue to depend on plugin-specific defaults.
- Expand RGB8 values into a 16-bit container: that would satisfy a file header
  while fabricating precision and hiding the missing rendered-source owner.
- Emit cICP for every ICC profile: arbitrary file profiles do not provide a
  proven exact CICP tuple, so doing so could create conflicting colour state.
- Add metadata or storage policy to the encoder: those owners need independent
  schema, failure, and publication evidence and are not codec primitives.
