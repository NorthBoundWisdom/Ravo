# ADR-0039: Explicit CLI and Studio export-option controls

- Status: Accepted
- Date: 2026-08-27

## Context

ADR-0030, ADR-0033, and ADR-0034 already own typed JPEG, PNG, and TIFF export
options on `ExportRequest`. ADR-0032 still publishes complete encoded bytes
without replacement. CLI already exposed most format-qualified flags, but JPEG
quality was silently accepted on other formats and JPEG subsampling plus TIFF
resolution were not CLI-visible. Studio inferred format from a localized
name-filter string or filename suffix and always posted domain defaults.

The public control surfaces needed one translation layer over those existing
typed values. A second codec-settings model, remembered UI state, presets,
batch jobs, path templates, or a localized-filter fallback would reopen
ownership that CatalogService and the private encoders already settle.

## Decision

- CLI and Studio construct the same `ExportFormat` plus `JpegExportOptions`,
  `PngExportOptions`, and `TiffExportOptions` values. CatalogService and the
  private encoders remain the only semantic validators and consumers.
- Domain defaults stay authoritative: JPEG quality 95/`auto`, PNG 8-bit/
  compression 5, TIFF uint8/Deflate-predictor/level 6/RGB/300 dpi. Every Studio
  invocation resets to those defaults. This ADR adds no preset, last-value
  memory, database row, settings key, batch job, or path template.
- CLI adds exactly `--jpeg-subsampling auto|444|440|422|420` and
  `--tiff-resolution-dpi 72..9600`. `--quality` and `--jpeg-subsampling` are
  JPEG-only. PNG- and TIFF-qualified flags keep their format scope, including
  the new resolution flag. Value flags last-value-wins;
  `--tiff-grayscale-if-neutral` remains one-shot. Format isolation runs before
  the Catalog opens.
- Studio replaces filter-string inference with one explicit format intent. An
  app-owned options dialog collects the selected format and only that format's
  controls, then the existing native save dialog chooses a path. QML displays
  state and forwards one intent; it does not parse formats, choose codec
  defaults, clamp invalid values, or construct domain objects.
- A desktop-private conversion helper consumes a strict Qt presentation map and
  returns an owned `ExportFormat` plus the three typed option values. Canonical
  payload keys are `quality`, `jpegSubsampling`, `pngBitDepth`,
  `pngCompression`, `tiffSampleType`, `tiffCompression`,
  `tiffCompressionLevel`, `tiffGrayscaleIfNeutral`, and `tiffResolutionDpi`.
  Only the selected format's keys are accepted; original copy accepts none.
- The selected format, not a translated filter or suffix, is authoritative.
  C++ path normalization appends `.jpg`, `.png`, or `.tif` when a rendered
  destination has no suffix; JPEG accepts `.jpg`/`.jpeg`, TIFF accepts
  `.tif`/`.tiff`, PNG accepts `.png`; any other rendered suffix fails with
  `studio_export_extension_mismatch`. Original copy keeps the chosen name.
- After validation, the presenter copies the normalized path, asset id, format,
  and typed options by value into the existing serial executor closure, then
  makes one `CatalogService::export_asset` call with the existing shutdown
  token. Later QML edits cannot mutate an in-flight request.
- `export_format_from_ui(path, filter)` is removed. There is no hidden default
  PNG choice from an empty suffix and no locale-sensitive filter parsing.

## Consequences

- Default CLI commands and default Studio exports remain byte-compatible with
  the previous domain defaults, except the already accepted TIFF DocumentName
  destination difference between distinct paths.
- Wrong-format, wrong-subcommand, malformed, missing, unknown, and
  path/format-mismatch inputs fail closed before render and publish no
  destination.
- Existing encoded metadata, ICC/cICP, pixels, precision, compression,
  cancellation, resource destruction, and ADR-0032 publication stay unchanged.
- Capture timezone/GPS, sidecars/history, batch presets/path templates, TIFF
  multipage masks, shared consumers, and legacy JPEG/PNG/TIFF owner retirement
  remain later I11/I12/I13/S9/J6 work. This ADR does not complete those rows.

## Subsequent decision

[ADR-0040](0040-capture-time-gps-metadata.md) later extends the shared export
metadata snapshot with validated capture time/offset/GPS. It does not change
this ADR's explicit format/options intent or add a metadata control surface.
