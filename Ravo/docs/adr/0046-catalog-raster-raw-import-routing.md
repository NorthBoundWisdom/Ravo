# ADR-0046: Catalog raster import validates pixels and preserves RAW routing

- Status: Accepted
- Date: 2026-08-28
- Extends: [ADR-0023](0023-jpeg-input-adapter-contract.md)

## Context

ADR-0023 requires Catalog to fully decode a recognized JPEG before inserting
an asset so truncated or malformed pixels cannot become a published row. PNG
and TIFF probe already refuse corrupt pixels, but Catalog still decoded only
JPEG before publication and treated every other `unsupported` raster probe as
a LibRaw candidate.

That second rule is required for TIFF-wrapped camera files without a RAW
suffix (`unsupported_tiff_raw_container`). It is wrong for recognized but
unimplemented raster layouts such as float, tiled, or multi-page TIFF: those
must stay structured unsupported rasters instead of falling through to LibRaw.

This decision is the P1 JPEG/PNG/TIFF catalog-publication slice of I5/I6/I7.
It does not retire the frozen imageio wrappers, implement float/multi-page
TIFF decode, or replace ADR-0047's first-frame LibRaw/DNG contract.

## Decision

- Catalog import fully decodes recognized JPEG, PNG, and TIFF sources at the
  browse thumbnail edge before `commit_imported_asset`. The owned RGB8 buffer
  is reused for the imported preview. A decode failure publishes no asset,
  preview, or cache file.
- A raster probe/decode error whose `format` is a recognized raster
  (`jpeg`/`png`/`tiff`/`bmp`/`gif`/`webp`/`qoi`/`rgbe`) never becomes a RAW
  inspect. Validation errors stay `failed`; unimplemented layouts stay
  `unsupported`.
- The only recognized-TIFF exception is `reason=unsupported_tiff_raw_container`.
  That file may still be inspected by LibRaw so a camera TIFF/DNG without a
  RAW suffix can import as `image/x-raw`.
- Unrecognized content may still fall through to LibRaw. Wrapper retirement
  for `imageio_jpeg.c` / `imageio_png.c` / `imageio_tiff.c` remains a later
  zero-consumer gate.

## Consequences

Corrupt PNG/TIFF imports now match the JPEG catalog contract. Float and
multi-page TIFF stay unsupported rasters. Disguised camera TIFFs continue to
import as RAW. Tests cover truncated PNG/TIFF publication failure, float-TIFF
non-routing, and an ARW copied to `.tif`.

## Rejected alternatives

- Probe-only PNG/TIFF publication: probe already refuses corrupt pixels, but
  Catalog would still skip the JPEG-owned thumbnail reuse path and could
  diverge again.
- Never fall through from TIFF to LibRaw: that would reject real camera files
  whose suffix is `.tif`.
- Decode float/multi-page TIFF in this tranche: that is later I7 work, not
  the catalog routing/publication contract.
