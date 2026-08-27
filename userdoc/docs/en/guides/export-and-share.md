# Export and Sharing

## Goal

Create a local rendered output or an exact original copy while preserving the
catalog recipe and refusing unsafe destination conflicts.

**Last verified:** 2026-08-27 against the current CatalogService export path and
CLI options.

## Applies to

- Ravo Studio's **Export Photo** dialog.
- The `ravo catalog export` CLI command.

## Prerequisites

- A library is open and the asset exists in it.
- The active original is readable for rendered export.
- The destination directory is writable and the destination path is new.

## Export from Studio

1. Open a library and select the photo to export.
2. Choose **File → Export Photo**, or use **Export…** in the left panel.
3. Choose **JPEG**, **PNG**, **TIFF**, or **Original copy** and the matching
   encoder options. The dialog always starts from the domain defaults.
4. Confirm the native save dialog and wait for the status bar to report
   completion.

The selected format is authoritative. If the destination has no suffix, Ravo
adds `.jpg`, `.png`, or `.tif` for rendered formats. JPEG also accepts
`.jpeg`, and TIFF also accepts `.tiff`. A conflicting suffix is rejected.
Original copy keeps the chosen filename and any suffix.

## Output formats

| Format | What Ravo writes | Current options and boundary |
| --- | --- | --- |
| PNG | Opaque rendered RGB pixels with resolved color metadata when supported. | Default is 8-bit compression 5. Studio and CLI expose bit depth 8|16 and compression 0–9. `--png-bit-depth 16` writes engine-owned 16-bit samples. An 8-bit source is rejected rather than padded with invented precision. |
| JPEG | Opaque rendered RGB pixels through the pinned JPEG encoder. | Quality 5–100, default 95. Subsampling `auto`, `444`, `440`, `422`, or `420`. Studio and CLI expose both. |
| TIFF | Classic little-endian, top-left, contiguous rendered output. | Default is unsigned 8-bit, Deflate with horizontal predictor, level 6, RGB, and 300 DPI. Studio and CLI can request uint16, float16, or float32, compression, level, conditional grayscale, and 72–9600 DPI. |
| Original copy | The original source bytes copied to a new destination. | No Develop rendering or re-encoding occurs. The source is never rewritten. |

Rendered export uses the active catalog recipe. A RAW export uses the processed
CPU RAW path; an original copy remains byte-for-byte source content.

Supported output profiles are resolved from the recipe. Ravo retains the
declared RGB profile in supported PNG, JPEG, and TIFF output paths. It does not
infer a monitor profile. Rendered JPEG/PNG/TIFF keep validated capture time and
GPS from the Catalog. Sidecar and history-attachment policy is not a current
general export contract.

For TIFF, the current catalog title/description/creator/copyright values can be
written into bounded baseline directory fields. The title field is deliberately
not mapped to a TIFF title tag; absent values are omitted and an explicitly empty
value has defined empty-field behavior.

## CLI encoder options

The CLI form is:

```text
ravo catalog export --catalog <library.sqlite> --asset-id <id> \
  --output <file> --format png|jpeg|tiff|tif|original \
  [--quality 5..100] [--jpeg-subsampling auto|444|440|422|420] \
  [--png-bit-depth 8|16] [--png-compression 0..9] \
  [--tiff-sample-type uint8|uint16|float16|float32] \
  [--tiff-compression none|deflate|deflate_predictor] \
  [--tiff-compression-level 1..9] \
  [--tiff-grayscale-if-neutral] [--tiff-resolution-dpi 72..9600] --json
```

Common options:

- `--quality 5..100` for JPEG only; default `95`.
- `--jpeg-subsampling auto|444|440|422|420` for JPEG only; default `auto`.
- `--max-edge N` to fit a rendered result within a positive maximum edge.

PNG-only options:

- `--png-bit-depth 8|16`; default `8`.
- `--png-compression 0..9`; default `5`.

TIFF-only options:

- `--tiff-sample-type uint8|uint16|float16|float32`.
- `--tiff-compression none|deflate|deflate_predictor`.
- `--tiff-compression-level 1..9`.
- `--tiff-grayscale-if-neutral`.
- `--tiff-resolution-dpi 72..9600`; default `300`.

PNG `16` and TIFF `uint16` / `float16` / `float32` requests render
engine-owned higher-precision samples from the active recipe. An 8-bit source
still fails closed instead of inventing precision. JPEG-, PNG-, and
TIFF-qualified flags are rejected outside their matching export format and
outside `catalog export`.

## Destination conflict behavior

Ravo uses atomic no-replace publication for rendered files and original copies.
If the destination already exists, the command returns a `conflict` error and
does not overwrite it. Choose a new path or move the existing file yourself.

If rendering, encoding, cancellation, or the final write fails, no partial
published output is treated as a successful export. Original files and sidecar
files are not modified by a rendered export.

## Result

The status bar or CLI result reports the output path, format, dimensions, and
bytes written. Reopen the output with an independent viewer when the file is
part of a delivery workflow.

## Common questions

### Why can I not export without selecting a photo?

Studio's export action is defined for the active photo. Multi-selection is
useful for review state and catalog metadata, but it does not create an implicit
batch export.

### Why did the output have a different size from the source?

Rendered output follows the current preview/render size and optional
`--max-edge`; original-copy output keeps the source bytes and dimensions.

### Why did a TIFF request fail when the sample type is valid?

The option is valid in the typed contract, but the current rendered source is
RGB8. Higher-precision TIFF publication is intentionally not fabricated yet.

### Can export repair a missing original?

No. Rendered export needs the original at its recorded path. Original-copy also
needs the source to be readable. See [File paths and recovery](../troubleshooting/file-paths-and-recovery.md).
