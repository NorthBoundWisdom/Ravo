# ADR-0036: Own TIFF baseline directory metadata before publication

- Status: Accepted
- Date: 2026-08-27

## Context

ADR-0034 moved the currently representable TIFF RGB8 output into a bounded
adapter-private LibTIFF encoder, but froze resolution at 300 dpi and left all
other TIFF metadata for later work. The frozen legacy output owner writes a
small set of main-directory fields before its shared post-encode metadata
path. Reusing that later Exiv2 path after publication would require reopening
and mutating the destination, bypassing ADR-0032's complete-bytes and atomic
no-replace contract.

Ravo already owns catalog `WritableMetadata` as values rather than Exiv2
objects. It can therefore express the baseline TIFF directory fields without
adding a dependency or claiming the much larger EXIF, IPTC, XMP, history, and
sidecar policy owned by S9/J6.

## Decision

- `TiffExportOptions::resolution_dpi` defaults to 300 and accepts 72 through
  9600 inclusive. Both X and Y resolution use that value and
  `RESUNIT_INCH`.
- Domain owns `ExportMetadataSnapshot`, containing the destination document
  name and one owned `WritableMetadata` value. The value contains no LibTIFF,
  Exiv2, Qt, or filesystem handle.
- For TIFF only, `CatalogService` creates the snapshot once immediately after
  successful asset lookup. It copies the current writable metadata and the
  destination string produced by the existing output-normalization path. The
  metadata owner performs no second `realpath` lookup. JPEG, PNG, and original
  copy do not receive TIFF metadata state.
- `DocumentName` tag 269 is the normalized destination string, not the catalog
  title. It must be at most 16 KiB, contain no NUL, and be well-formed UTF-8.
  The bound limits the LibTIFF ASCII allocation and `strlen`-based C boundary
  while leaving room for the product's normalized local paths.
- Present writable values map as follows: description to `ImageDescription`
  270, creator to `Artist` 315, and copyright to `Copyright` 33432. A present
  empty value writes exactly one terminating NUL; an absent value omits its
  tag. Title is deliberately unmapped.
- The main IFD omits `EXIFIFD` 34665, IPTC 33723, and XMP 700. Capture/timezone/
  GPS state, complete EXIF/IPTC/XMP packets, XMP attach/history, and sidecar
  policy remain the shared S9/J6 contract. The encoder does not recreate them
  from partial catalog fields.
- The metadata snapshot is synchronously borrowed through `RasterDecoder` and
  the Qt adapter. Existing encoders fail closed on nonempty TIFF metadata until
  they explicitly own the overload; unrelated formats retain their previous
  path. Values are validated before LibTIFF receives them.
- Cancellation is checked at entry and around each emitted metadata tag, every
  scanline, and finalization. Invalid resolution, document name, or writable
  text and a tag-write failure have stable structured reasons. Failure returns
  no encoded vector and mutates no source pixels, ICC bytes, metadata values,
  original file, or sidecar.
- Pixel layout, compression, conditional grayscale, and exact RGB ICC remain
  ADR-0034 owners. Only after the complete in-memory TIFF byte vector succeeds
  does ADR-0032 atomically publish it with no replacement. No post-publication
  Exiv2 rewrite is permitted.

## Consequences

TIFF RGB8 export now owns its bounded baseline main-directory metadata and
resolution at the same deterministic encode boundary as pixels and ICC.
Catalog tests prove that the normalized destination and current writable
values are copied once, non-TIFF formats receive an empty snapshot, and source
plus sidecar hashes, sizes, and modification times remain unchanged.

This is still an I13 tranche, not I13 completion. Real uint16/float16/float32
rendered sources, complete EXIF/IPTC/XMP packets, capture/timezone/GPS mapping,
XMP attach/history and sidecar policy, multipage masks, Studio options, shared
imageio/storage/job consumers, and legacy plugin retirement remain open. The
work adds no Exiv2 or other dependency, and it does not authorize another
algorithm-migration tranche during the feature-convergence pause.

## Rejected alternatives

- Reopen the published TIFF with Exiv2: this breaks the complete-byte atomic
  publication boundary and introduces a second failure window after success.
- Map catalog title to `DocumentName`: the frozen baseline field identifies the
  destination document; title is a distinct metadata value awaiting packet
  policy.
- Serialize every capture field into ad-hoc TIFF tags: S9/J6 must define exact
  packet ownership, namespaces, precedence, bounds, and sidecar behavior first.
- Treat this baseline IFD as full metadata migration or retire the old plugin:
  high precision, packets, multipage output, consumers, and retirement gates
  remain unresolved.
