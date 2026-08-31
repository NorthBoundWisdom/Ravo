# ADR-0038: Catalog-owned embedded export metadata

- Status: Partially superseded by ADR-0040
- Date: 2026-08-27

## Context

ADR-0032 requires a complete in-memory encoded vector before atomic
no-replace publication. ADR-0036 already embeds TIFF baseline
DocumentName/description/creator/copyright, but JPEG, PNG, and TIFF still
lacked the bounded public Exif/XMP/IPTC packets that rendered exports need.
The frozen legacy owner builds an Exif blob, lets each format write it, then
reopens the destination to attach source/sidecar/DB XMP. Ravo cannot copy that
reopen, swallowed-error, or arbitrary-source-merge path.

Catalog already owns writable metadata, capture values, and unique tags. The
product needs one immutable snapshot of those public values, one private
serializer, and container framing that still fits JPEG APP marker lengths for
every format. Capture timezone and GPS remain undefined in the accepted
schema, so this tranche must not invent DateTimeOriginal or a GPS IFD.

## Decision

- `ExportMetadataSnapshot` owns destination DocumentName (TIFF only), writable
  values, capture values, and already-canonical unique tags sorted by UTF-8.
  Counts that cannot fit canonical XMP are rejected before copying or sorting.
  Generic `validate_export_metadata` is shared; TIFF additionally validates
  DocumentName.
- CatalogService snapshots once after asset lookup and path normalization for
  every rendered JPEG/PNG/TIFF export. Original copy receives no generated
  snapshot and remains exact source bytes.
- One adapter-private prepared-metadata value stores semantic capture numbers
  plus owned Exif TIFF-profile, XMP, and optional IPTC-IIM bytes. JPEG, PNG,
  and TIFF consume that value; they do not remap optionals or rationals.
- Never copy arbitrary original Exif/IPTC/XMP. Never read, merge, create,
  replace, or delete an XMP sidecar. Do not embed recipe, history, snapshots,
  rating/reject/color-label, private tags, source/catalog paths, or random IDs.
- `captured_unix_s` is not exported. No DateTimeOriginal, OffsetTime, PNG
  `tIME`, GPS IFD, or location XMP. Both omissions remain explicit S9/J6 work.
- Output is reproducible: no current time, filesystem time, locale numbers,
  host path, random padding, or unordered iteration.
- Field matrix:
  - output width/height → Exif PixelX/YDimension and matching XMP
  - oriented output → orientation 1 / `tiff:Orientation=1`
  - built-in sRGB (`kBuiltin` + `identifier=="srgb"`) → ColorSpace 1, else
    `0xffff`; ICC/cICP remain the color-profile contract
  - make/model, ISO, aperture, focal length, shutter → matching Exif/XMP
  - title → XMP `dc:title` and IPTC 2:5 only; TIFF DocumentName stays the
    destination
  - description/creator/copyright → Exif ImageDescription/Artist/Copyright,
    XMP language alternatives or one-item `dc:creator` Seq, IPTC 2:120/2:80/2:116
  - sorted tags → XMP `dc:subject` bag and one IPTC 2:25 dataset each
  - one deterministic `xmp:CreatorTool` = `Ravo`
- Three-state optionals: absent omits, present-empty emits an empty field,
  present-nonempty emits exact UTF-8. IPTC is omitted only when all four
  writables are absent and tags are empty. Present-empty still emits a packet
  with its empty dataset.
- XMP text must contain only XML 1.0 characters; carriage return is emitted as
  `&#xD;` so parsing preserves the Catalog value. IPTC-IIM includes the UTF-8
  coded-character-set dataset and mandatory 2:00 RecordVersion 4. IIM byte
  limits are 64 for title/keyword, 32 for creator, 128 for copyright, and 2000
  for description. Unsupported controls or oversized mapped values fail before
  render; values are never truncated.
- Capture numbers reject non-finite, zero, or negative values. ISO must be an
  exact uint16 integer in 1–65535. The three positive rationals use one
  deterministic unsigned 32-bit continued-fraction approximation shared by
  Exif and XMP `num/den`.
- Packet limits are the JPEG APP maximum of 65533 including identifiers:
  Exif TIFF profile 65527, XMP 65504, IPTC-IIM 65506. Oversized values fail
  with `export_exif_packet_too_large`, `export_xmp_packet_too_large`, or
  `export_iptc_packet_too_large`. No truncation, tag dropping, or extended XMP.
- JPEG writes Exif APP1, XMP APP1, existing ICC APP2, then optional Photoshop
  APP13 after `jpeg_start_compress` and before the first scanline.
- PNG writes one `eXIf` TIFF profile without `Exif\0\0` and one uncompressed
  `iTXt` with keyword `XML:com.adobe.xmp`. It writes no IPTC and no `pHYs`.
- TIFF keeps the ADR-0036 baseline, then uses public
  `TIFFCreateEXIFDirectory` / `TIFFWriteCustomDirectory` / `TIFFSetDirectory(0)`
  / `TIFFTAG_EXIFIFD` plus XMP 700 and IPTC 33723. LibTIFF stores EXIF
  rationals through its public float setter; JPEG/PNG keep the exact owned
  rational bytes. There is no filesystem reopen.
- Every RasterDecoder implementer explicitly owns or rejects the metadata
  overload. There is no default virtual fallback: an otherwise empty Catalog
  snapshot still requires baseline Exif/XMP packets.

## Consequences

Rendered JPEG/PNG/TIFF exports now carry the bounded Catalog-owned public
metadata before ADR-0032 publication. Capture timezone/GPS schema, general
sidecar read/write/conflict/rollback, history/recipe interchange, Studio/CLI
metadata flags, batch presets, PNG pHYs, JPEG extended XMP, TIFF multipage
masks, shared old consumers, and legacy owner retirement remain unfinished.
S9 and J6 are not complete.

## Subsequent decision

[ADR-0040](0040-capture-time-gps-metadata.md) supersedes only this ADR's
explicit capture-time/timezone/GPS omission. The Catalog-owned snapshot,
packet limits, deterministic framing, source immutability, and sidecar/history
exclusions remain in force.

## Rejected alternatives

- Reopening a published file with Exiv2 or enabling Exiv2 XMP in
  `ravo_adapters`
- Copying original or sidecar packets, or inventing DateTimeOriginal from
  `captured_unix_s`
- Format-specific packet limits that would let PNG/TIFF encode values JPEG
  cannot frame
- Silent truncation, extended XMP, multiple Photoshop resources, or PNG IPTC
- A CLI/Studio compatibility flag or second export pipeline
