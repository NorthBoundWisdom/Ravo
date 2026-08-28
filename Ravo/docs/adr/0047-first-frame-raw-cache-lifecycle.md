# ADR-0047: First-frame RAW decode, DNG routing, and preview-cache miss

- Status: Accepted
- Date: 2026-08-28
- Extends: [ADR-0046](0046-catalog-raster-raw-import-routing.md)

## Context

P1's user outcome is to open and view common photos. ADR-0046 already requires
Catalog to fully decode JPEG/PNG/TIFF before publication. Remaining first-frame
work is LibRaw/DNG input, the Bayer prepare/demosaic subset that produces a
viewable frame, corrupt preview-cache recovery, and close/reopen lifecycle.

The leftover `imageio_libraw` wrapper, `dng_opcode` GainMap parser, and
`imageio_dng` writer still have frozen consumers. Frozen `demosaic.c` defaults
to RCD/PPG/X-Trans Markesteijn; Ravo's 3×3 Bayer interpolator is only a
first-frame subset. Those leftovers must not be deleted or described as
complete ALG retirement.

## Decision

- LibRaw inspect and first-frame decode own structured `reason` values:
  `empty_raw_path`, `raw_not_found`, `raw_not_regular_file`,
  `libraw_unsupported_file`, `libraw_open_failed`, `libraw_unpack_failed`,
  `unsupported_raw_sensor`, `invalid_raw_dimensions`, and
  `oversized_raw_frame`. Missing files are `not_found`; directories and empty
  paths are `invalid_argument`; unidentified content is `unsupported`; unpack
  and dimension failures are `validation`; X-Trans/non-Bayer and oversized
  frames are `unsupported`.
- First-frame decode requires a 16-bit Bayer CFA (`filters != 0` and not
  `LIBRAW_XTRANS`) plus `raw_image`. Prepare uses LibRaw crop
  (`left_margin`/`top_margin`), black/white, 2×2 CFA, and flip. DNG GainMap
  OpcodeList2/3 stays with later R1 work.
- First-frame demosaic is the existing 3×3 Bayer interpolator. RCD, PPG, dual
  demosaic, green matching, and X-Trans Markesteijn remain later R2 work.
- `.dng` and other RAW suffixes go through LibRaw. A TIFF probe that returns
  `unsupported_tiff_raw_container` may still inspect via LibRaw. Catalog
  publishes a RAW asset only after inspect succeeds and either an embedded JPEG
  browse thumbnail exists or `decode_raw_frame` succeeds. A failed first frame
  publishes no asset.
- `FilesystemPreviewCache::existing_png` treats a missing 8-byte PNG signature
  as a miss and removes the corrupt file so the next request rebuilds it.
  Catalog `close` releases repository, raster, cache, and decoded RAW/working
  buffers; reopen rebuilds preview from the read-only source.
- I2/I4 leftover wrappers remain until a freeze census reaches zero consumers.
  I1 dispatcher deletion, explicit cache-byte LRU, and leftover GTK jobs are
  outside this first-frame contract.

## Consequences

Common Bayer RAW, including `.dng` suffixes and TIFF-wrapped camera files, can
import and view. X-Trans may browse from an embedded JPEG but full decode stays
`unsupported_raw_sensor`. Corrupt, missing, unrecognized, oversized, and
cancelled inputs fail structurally. Tests cover those LibRaw reasons, DNG-suffix
import of a Bayer fixture, X-Trans rejection, RAW import cancellation, corrupt
PNG cache rebuild, and close/reopen preview.

## Rejected alternatives

- Treating 3×3 Bayer as complete demosaic retirement, or GainMap-less LibRaw
  crop as complete rawprepare retirement.
- Publishing a RAW asset whose only path to a first frame is a later failed
  unpack.
- Deleting `imageio_libraw`, `dng_opcode`, or `imageio_dng` while I1/S9/R1/J2
  consumers remain.
- Implementing an explicit preview-cache byte LRU as a P1 blocker; corrupt-miss
  plus close/reopen is the first-frame bound.
