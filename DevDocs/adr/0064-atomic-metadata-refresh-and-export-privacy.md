# ADR-0064: Capture refresh is atomic and rendered export has typed privacy modes

- Status: Accepted
- Date: 2026-08-29
- Extends: [ADR-0040](0040-capture-time-gps-metadata.md)

## Context

Capture metadata was read once at import and rendered exports always embedded
the complete bounded public snapshot. S9 still required an explicit refresh
path and a privacy policy that removed location or all public metadata without
rewriting the original, copying opaque packets, or silently emitting partial
metadata.

## Decision

- Engine's private Exiv2 boundary reads bounded, uniquely typed Make, Model,
  ISO, FNumber, FocalLength, ExposureTime, DateTimeOriginal components, and GPS.
  Conflicting ISO tags, duplicate/wrong-type/multi/non-finite/non-positive
  values, malformed dates/GPS, missing files, and cancellation fail
  structurally.
- `CatalogService::refresh_capture_metadata` re-reads current source identity
  and capture values. RAW inspection supplies RAW-native camera/numeric/time
  state; embedded Exif supplies common fields/date/GPS. Missing fields clear
  stale catalog capture values rather than inheriting the previous snapshot.
- Repository publishes asset identity, capture row, and revision in one SQLite
  transaction. A source read, validation, cancellation, SQL, revision, or
  commit failure leaves the prior asset/capture/revision visible.
- Studio exposes **Refresh Capture Metadata** through its command registry;
  CLI exposes `catalog refresh-metadata --asset-id`. Neither writes the source
  or sidecar.
- Rendered export owns `ExportMetadataMode`: `full`, `no-location`, or `none`.
  `no-location` preserves writable/camera/time/tags while removing every GPS
  value. `none` passes an explicitly disabled and otherwise empty snapshot;
  JPEG writes no Exif/XMP/IPTC marker, PNG no eXIf/XMP iTXt, and TIFF no
  DocumentName/writable/Make/Model/XMP/IPTC/Exif/GPS IFD. ICC/color encoding
  remains because it describes pixels, not public catalog metadata.
- Original-copy rejects non-full modes because it must remain exact source
  bytes and cannot strip embedded source metadata. CLI `--metadata` and Studio
  use the same three canonical values.

## Consequences

S9's Ravo metadata-refresh and privacy contracts are accepted. Shared old Exif,
metadata/tag, image/crawler, storage, and sidecar writers remain until their
S7/S9/J2/I10–I14 consumers reach zero. Ravo adds no opaque packet copy,
automatic source update, or fallback privacy heuristic.

## Rejected alternatives

- Updating capture columns separately from asset identity/revision. Readers
  could observe mixed generations.
- Clearing only GPS Exif while retaining XMP GPS strings. Privacy must be
  semantic and format-independent.
- Calling `none` while still emitting minimal public Exif/XMP. The user-facing
  mode promises no public metadata packets; ICC is the only retained profile.
- Stripping original-copy bytes. That would no longer be original copy.
