# ADR-0023: Make JPEG input a strict adapter contract

- Status: Accepted
- Date: 2026-08-26

## Context

The Qt raster adapter already decoded JPEG files and embedded JPEG previews,
but it delegated content recognition, APP2 profile interpretation, corruption
classification, and orientation-aware scaling to different `QImageReader`
call paths. That left several observable ambiguities: file names could
influence recognition, EXIF orientations 6 and 8 supplied transformed
dimensions to a pre-transform scaling API, malformed ICC segment sets could be
silently ignored, and a header-only catalog probe could publish an asset before
pixel corruption was discovered.

[ADR-0018](0018-explicit-input-color-profiles.md) requires an untagged raster to
remain explicitly untagged. JPEG input therefore needs a codec-specific
ownership boundary before Qt creates a decoded image.

This decision covers migration row I5, whose frozen JPEG input reference owner
is `legacy/src/imageio/imageio_jpeg.c`. It does not cover I11, whose owner is
`legacy/src/imageio/format/jpeg.c` and whose contract includes export quality,
subsampling, output metadata, profile writing, and disk-full publication.

## Decision

- JPEG recognition is content-based and begins with the two-byte SOI marker.
  File and memory inputs then use the same bounded marker parser. A suffix does
  not select or veto JPEG decoding.
- The parser requires a valid frame header, one- or three-component input, at
  least one valid scan header, bounded marker payloads, and EOI. Grayscale is
  expanded to RGB8. Four-component CMYK/YCCK and other component layouts are
  explicit unsupported inputs; no colour-model guess or conversion fallback
  is allowed.
- APP2 `ICC_PROFILE` chunks are adapter-owned. Their count must be non-zero and
  consistent, every one-based sequence number must occur exactly once, and
  physical marker order is irrelevant. The reassembled bytes must be one valid
  RGB ICC profile. Ravo preserves those exact bytes in `ColorProfileState`.
  Missing APP2 ICC state remains `missing`; Qt-inferred or sRGB fallback state
  is not published.
- `DecodedRaster` declares `RasterPixelFormat::kRgb8` and
  `RasterAlphaMode::kOpaque`. JPEG success explicitly publishes those values
  and an owned width-by-height-by-three byte buffer. JPEG has no inferred alpha
  channel.
- `QImageReader::setScaledSize` receives native encoded dimensions. EXIF
  auto-transformation runs afterwards, so transpose orientations 5 through 8
  preserve the requested maximum edge and aspect ratio. The additional
  embedded-preview quarter rotation remains an explicit caller contract.
- A recognized SOI followed by malformed, truncated, or undecodable structure
  is `validation` with stable JPEG reason context. Random or otherwise
  unrecognized content and structurally valid but unimplemented JPEG colour
  layouts are `unsupported`. Empty paths are `invalid_argument`, missing paths
  are `not_found`, non-files are `invalid_argument`, and open/read failures are
  `io`.
- Decode checks cancellation before reading and at bounded marker/entropy-scan
  intervals, then by output row. File and memory sources are read-only and are
  never rewritten or exposed through a mutable decode view.
- Catalog import fully decodes a recognized JPEG before inserting its asset.
  That decoded thumbnail is reused for preview generation. A corrupt JPEG
  therefore returns an item-level structured error with no asset, ready
  preview, or partial publication. CLI catalog import serializes the complete
  item error, including code, message, and context.

## Consequences

Qt remains the private pixel decoder and EXIF transform implementation, while
Ravo owns recognition, structure, colour-state, error, cancellation, and
publication semantics. Tests use a non-symmetric generated JPEG and injected
markers to cover EXIF 1 through 8, scaling, ICC segment topology, grayscale,
four-component rejection, corrupt/truncated inputs, file/memory parity,
cancellation, and source immutability.

The old `legacy/src/imageio/imageio_jpeg.c` wrapper is not retired by this
decision. Deletion requires a separate consumer census proving that legacy
CLI dispatch, image I/O dispatch, DNG/thumbnail helpers, mipmap paths, EXIF
helpers, and every other frozen caller no longer depends on it. I11 export
work is an independent gate and cannot be claimed by the I5 input tests.

## Rejected alternatives

- Trust `QImage::colorSpace()` for JPEG input: it cannot prove APP2 sequence
  completeness and risks turning missing or malformed profile state into an
  implicit decoder choice.
- Recognize JPEG by extension or require the third marker-prefix byte: the
  former disagrees with memory decode, while the latter misclassifies a
  two-byte SOI or corrupt next byte as unsupported instead of recognized
  corruption.
- Scale transformed dimensions through `setScaledSize`: that API consumes
  encoded dimensions and swaps the aspect ratio for EXIF 6 and 8.
- Insert after a header-only probe and mark preview failure later: a corrupt
  input would become a catalog asset despite never satisfying the decoder
  contract.
- Fold output quality, subsampling, metadata, or atomic file writing into this
  change: those are I11 export responsibilities with different failure and
  publication surfaces.
