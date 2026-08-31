# ADR-0030: Own typed JPEG export options

- Status: Accepted
- Date: 2026-08-27

## Context

The frozen `legacy/src/imageio/format/jpeg.c` owner stores JPEG quality and
chroma subsampling as format parameters. Its configured quality range is
5–100 with a default of 95. Subsampling uses stable values for automatic,
4:4:4, 4:4:0, 4:2:2, and 4:2:0, but the write path rereads mutable global
configuration instead of consuming the captured subsampling value.

Ravo already owns a pinned private libjpeg-turbo encoder with the frozen
quality-dependent DCT, smoothing, optimized-Huffman, automatic sampling, ICC
APP2, JFIF density, resource, and cancellation contracts. Its public request
and raster port still carried a naked integer with a default of 90 and an
accepted range of 1–100, and there was no explicit subsampling value to pass
from the service to the encoder.

This is an I11 adapter tranche. It closes the typed quality/subsampling value
and propagation boundary. It does not complete JPEG metadata or encoded-file
publication, and it does not retire the legacy format plugin.

## Decision

- Domain owns `JpegSubsampling` as a handle-free value enum with stable numeric
  values: `auto=0`, `444=1`, `440=2`, `422=3`, and `420=4`. Canonical name and
  parse functions accept exactly those strings; unknown values fail closed.
- `JpegExportOptions` owns quality and subsampling with a default of quality 95
  and automatic subsampling. One validator accepts quality 5–100 and the five
  declared enum values. Structured validation errors retain `format=jpeg` and
  stable quality or subsampling reasons.
- `ExportRequest` owns the complete options value. `CatalogService` validates
  it before rendering a JPEG and passes it by const reference through the
  raster port and Qt adapter to the private encoder. PNG and TIFF ignore the
  JPEG-specific value.
- Automatic subsampling keeps the frozen thresholds: quality at most 90 uses
  4:2:0, 91–92 uses 4:2:2, and at least 93 uses 4:4:4. Explicit modes replace
  only the Y sampling factors. Baseline quantization, DCT selection, smoothing,
  optimized Huffman coding, and 1×1 chroma factors remain quality-owned.
- The synchronous encode call only borrows pixels, resolved RGB ICC bytes,
  options, and cancellation state for the call. Success returns an owned byte
  vector; validation, resource, libjpeg, or cancellation failure publishes no
  vector and does not mutate source pixels or profile state.
- The existing CLI quality flag writes the typed quality field. Explicit
  subsampling has no CLI or Studio intent in this tranche; Studio receives the
  typed default.

## Consequences

JPEG quality defaults and bounds now match the frozen owner, and service-level
requests can select all five frozen sampling modes without mutable global
configuration or libjpeg types crossing the adapter boundary. The pinned
encoder remains the sole JPEG output implementation; Qt remains part of the
JPEG input path.

I11 is not complete. EXIF/IPTC/XMP selection and writing remain a metadata
owner, while unique temporary ownership, atomic no-replace publication,
durability, and disk-full behavior remain the I14 storage owner. The current
generic encoded-byte publisher is not accepted as that final contract.
`legacy/src/imageio/format/jpeg.c`, its registration, and the shared dynamic
image-I/O, storage, CLI, and job consumers therefore remain.

## Rejected alternatives

- Keep a naked integer plus a separate subsampling argument: that would retain
  two loosely coupled public values and allow defaults to diverge again.
- Re-read mutable configuration in the encoder: asynchronous callers would not
  have an immutable request snapshot and would reproduce the frozen lifecycle
  defect.
- Expose libjpeg sampling structures through domain or services: codec handles
  and layout belong to the private adapter.
- Add EXIF/XMP or file publication to this tranche: those contracts require
  separate metadata and storage ownership and deterministic failure tests.
