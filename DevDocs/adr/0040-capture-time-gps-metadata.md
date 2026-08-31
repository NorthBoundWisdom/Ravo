# ADR-0040: Catalog-owned capture time and GPS metadata

- Status: Accepted
- Date: 2026-08-27
- Owners: domain (`CaptureDateTime` / `CaptureAltitude` / `CaptureLocation`),
  engine-private Exiv2 reader, SQLite catalog schema v5,
  `PreparedExportMetadata`

## Context

ADR-0038 already publishes one Catalog-owned public metadata snapshot into
rendered JPEG/PNG/TIFF. That snapshot omitted capture time and GPS because
`captured_unix_s` is unzoned and old catalogs cannot invent a timezone. Import
already inspected camera/exposure fields through LibRaw/Exiv2, but it did not
persist a local Exif datetime, source offset, or GPS location.

Users expect rendered exports to keep the photo's original capture time and
location when those values were embedded in the file. There is no privacy
toggle, map editor, or metadata-refresh workflow in this tranche.

## Decision

1. **Domain.** `CaptureMetadata` owns one optional `CaptureDateTime` and one
   optional `CaptureLocation`. Local time is the exact Exif `YYYY:MM:DD HH:MM:SS`
   spelling plus optional 1–9 subsecond digits and an optional minute-resolution
   UTC offset in `[-14:00, +14:00]`. Offset `-00:00` is rejected. Location is
   signed microdegrees plus optional `CaptureAltitude` `{ magnitude_mm,
   above_sea_level | below_sea_level }`. A location requires both coordinates;
   altitude cannot exist without that pair. Above-sea-level magnitude is bounded
   at 100_000_000 mm; below-sea-level at 12_000_000 mm. Both altitude zeros keep
   their stored reference; ref-1 zero is not rewritten to ref-0.
   `captured_unix_s` stays for its existing unzoned purpose and is never
   exported as local time or `Z`.

2. **Rational conversion.** Source unsigned 32-bit numerator/denominator pairs
   convert with checked integer arithmetic. The exact source value is compared
   against the legal bound **before** rounding. Accepted values then use
   nearest-integer rounding with **ties away from zero**. The reverse path emits
   reduced DMS/altitude rationals without floating point. LibTIFF's public GPS
   lat/lon/altitude setters are double; TIFF converts the already-derived
   rationals to double only at that API boundary. JPEG/PNG write the owned
   rationals exactly.

3. **Private read.** Engine-private Exiv2 resolves precedence, duplicates, types,
   calendar, and GPS and returns one owned semantic engine value. Public domain
   and Engine headers do not expose tag-status DTOs. CatalogService copies the
   semantic value and validates; it does not know Exif keys or tag statuses.
   RAW/JPEG/TIFF use a path-based Exiv2 reader. PNG uses an incremental chunk
   walk that validates length, CRC, and required order, rejects duplicate/empty
   `eXIf` and the JPEG-only `Exif\0\0` prefix, and caps the TIFF payload.
   Allocation and library exceptions die at the Engine boundary.

4. **Schema v5.** `asset_metadata` gains seven additive nullable columns:
   `captured_local_exif`, `captured_subsecond_digits`,
   `captured_utc_offset_minutes`, `gps_latitude_e6`, `gps_longitude_e6`,
   `gps_altitude_magnitude_mm`, `gps_altitude_ref`. Both altitude columns are
   NULL or both present. Existing v4 rows stay NULL. Migration never opens an
   original and never backfills from `captured_unix_s`. Import publishes the
   new asset, optional capture row, and revision in one transaction. Wrong
   SQLite storage classes and partial datetime/altitude pairs are rejected on
   read.

5. **Export.** One `PreparedExportMetadata` derivation writes DateTimeOriginal /
   OffsetTimeOriginal / SubSecTimeOriginal and a GPS IFD (`[2,3,0,0]` plus
   lat/lon and optional altitude) plus matching XMP `exif:DateTimeOriginal` and
   GPS latitude/longitude/optional altitude. Exif/XMP reproduce the stored
   altitude reference. Missing values omit only those properties. Malformed
   present Catalog state rejects the whole export. Original-copy export remains
   exact bytes.

6. **Default preserve.** Valid imported capture time and GPS are preserved on
   rendered JPEG/PNG/TIFF. There is no strip-location control in this tranche.
   A future opt-out remains a product decision.

## Consequences

- CLI `capture.captured_at` is ISO local time with exact subseconds and the
  source offset only when present; unzoned times never gain `Z`.
- CLI `capture.gps` is null without a complete pair; coordinates are
  scaled-decimal JSON numbers. `altitude_m` is the signed physical value; both
  zeros encode JSON `0`.
- TIFF writes EXIF and optional GPS custom directories, retains both offsets,
  restores the main IFD once, then links both directories.
- Sidecar/history interchange, metadata refresh, privacy stripping, PNG `pHYs`,
  TIFF multipage masks, and old-owner retirement remain unfinished. S9/J6 and
  I11/I12/I13 are not complete.

## Rejected alternatives

- Inferring offset or UTC from `captured_unix_s`, host timezone, filename, GPS,
  or filesystem time.
- Copying arbitrary source Exif/XMP/IPTC, maker notes, or GPS extras.
- Database REAL coordinates or platform date/time objects.
- Per-encoder reinterpretation of coordinates.
- Silent field omission when a present value is malformed.
- Enabling a new Exiv2/PNG dependency in this tranche.
- A signed `gps_altitude_mm` column that cannot distinguish ref-0 zero from
  ref-1 zero.
