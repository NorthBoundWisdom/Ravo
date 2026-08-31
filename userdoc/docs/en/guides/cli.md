# Command-Line Client

## Goal

Use `ravo` for inspection, catalog automation, recipe validation, preview
diagnostics, recovery/backup, and local export with machine-readable results.

**Last reviewed:** 2026-08-31 against the current `ravo-cli/v1` implementation
and committed CLI contract tests.

## Applies to

- The `ravo` executable built from this repository.
- Scripts and headless workflows that need the same engine and services as
  Ravo Studio.

## Prerequisites

- A prepared Ravo build and a local path to the `ravo` executable.
- A writable catalog/output location for commands that create or export files.

## Locate and run the CLI

For the macOS Debug build:

```text
./build/mac_clang_debug/Ravo/cli/ravo --version --json
```

The corresponding executable is under `build/<preset>/Ravo/cli/` on Windows and
Linux. The CLI is local and does not start the old application.

## JSON protocol

Add `--json` to any supported command. A successful response has this shape:

```json
{
  "data": {},
  "diagnostics": [],
  "ok": true,
  "type": "ravo.cli.result",
  "version": 1
}
```

A failure keeps the same outer protocol and includes a structured error:

```json
{
  "diagnostics": [],
  "error": {
    "code": "conflict",
    "context": {},
    "message": "..."
  },
  "ok": false,
  "type": "ravo.cli.result",
  "version": 1
}
```

Without `--json`, success is written as JSON data and failures are written to
stderr in human-readable form. Service logging stays in the per-user log file
so it cannot corrupt a machine-readable stdout stream.

## Start-to-finish catalog example

Replace the placeholders with paths from your workspace:

```text
RAVO=./build/mac_clang_debug/Ravo/cli/ravo
CATALOG="/work/Ravo Library.sqlite"

"$RAVO" catalog create --path "$CATALOG" --json
"$RAVO" catalog import --catalog "$CATALOG" --input "/photos/2026" --json
"$RAVO" catalog list --catalog "$CATALOG" --json
"$RAVO" catalog preview --catalog "$CATALOG" --asset-id <asset-id> --json
"$RAVO" catalog export --catalog "$CATALOG" --asset-id <asset-id> \
  --output "/exports/photo.png" --format png --json
```

Directory import is recursive and returns item-level statuses. Keep the asset
ID returned by `catalog list` or an imported item for later commands.

If Ravo Studio already has that library open, committed CLI writes appear in
the window within about one second. For selection-relative work, the same CLI
can use Studio's owner-only local `ravo-studio-control/v1` endpoint; it is not a
network listener and does not expose Assistant credentials.

## Top-level commands

| Command | Purpose |
| --- | --- |
| `--version` | Return the Ravo version and `ravo-cli/v1` protocol name. |
| `operations` | List registered versioned engine operations and their descriptors. |
| `develop-fields` | List every closed Develop `--set` field name, kind, and range. |
| `inspect <input>` | Inspect a supported RAW input's format, dimensions, and camera identity. |
| `lut inspect <file.cube>` | Validate a bounded 3D LUT and report its canonical path, size, domain, title, and content fingerprint. |
| `noise calibrate ...` / `noise inspect ...` | Fit or validate a deterministic camera-noise profile artifact without changing a photo or catalog. |
| `recipe validate <recipe>` | Parse and validate a recipe without rendering it. |
| `recipe import-xmp <xmp> ...` | Convert a strictly supported leftover darktable XMP subset, or a Lightroom CRS preset, into a versioned recipe file. |
| `recipe style-create ...`, `style-validate ...`, or `style-apply ...` | Create, validate, or apply complete and selective `.rstyle.json` artifacts. |
| `render <input> ...` | Render a validated recipe to an atomic PNG output using CPU. |
| `catalog ...` | Create, query, edit, preview, history-manage, synchronize recovery, back up, verify, and export a Ravo catalog. |
| `studio ...` | Discover a live Studio session, inspect its selected recipe, commit strict Develop fields, and publish its latest effect. |

## Control the selected photo in a running Studio

List live sessions and inspect the one associated with the current checkout:

```text
ravo studio sessions --json
ravo studio state --json
```

`state` reports the process/session and state revisions, catalog path/revision,
primary and selected asset IDs, browse mode, current and saved recipes,
baseline-relative `modified_operations`, pending changes, and the displayed
preview identity. If more than one Studio session matches the checkout, pass
the `session_id` explicitly:

```text
ravo studio state --session-id <session-id> --json
```

Commit one ordered parameter batch and optionally obtain its exact rendered
effect:

```text
ravo studio develop --session-id <session-id> \
  --set exposure=0.6 --set saturation=-0.1 \
  --output "/work/studio-result.png" --json
```

Without explicit `--expect-session-revision` and
`--expect-selection-revision`, the CLI observes a fresh state and carries those
revisions in the mutation. It retries only when incidental session state moved
while the asset, selection revision, and recipe remained identical. Supplying
either expectation makes the binding strict and any mismatch returns
`conflict`; a request is never redirected to a newly selected photo.

To render the current recipe without saving or changing it, including a
pending in-memory slider value:

```text
ravo studio preview --session-id <session-id> \
  --output "/work/current-effect.png" --max-edge 1600 --json
```

Image bytes do not cross the control socket. The CLI renders the snapshot's
canonical recipe through CatalogService and the existing CPU preview path,
rechecks selection and recipe revisions, then atomically publishes a new PNG.
The JSON result identifies its MIME type, dimensions, color profile, byte size,
SHA-256, and caller-owned lifecycle. Existing output paths return `conflict`.

## Inspect an input

```text
ravo inspect "/photos/source.cr2" --json
```

The current inspection command is the RAW inspection path. For a supported RAW
file it reports `format`, display dimensions, `is_raw`, camera identity, CFA
family/size, the sensor-default demosaic mode, white-balance coefficients, DNG
OpcodeList2/3 support and optional-skip state, and the normalized input URI.
Raster files should be checked through catalog import and preview instead.
Inspection does not import, edit, or write the source.

## Inspect available operations

```text
ravo operations --json
```

The command is useful for tooling that wants the current versioned operation
descriptors instead of hard-coding an assumed registry. Operation availability
does not make an unsupported legacy mask, blend, or history state importable.

## Discover Develop `--set` fields

```text
ravo develop-fields --json
ravo catalog fields --json
```

Both commands return the same inventory. Neither needs a catalog. Each closed
numeric or toggle field includes `name`, `kind`, `minimum`, and `maximum`.
Text fields currently include `watermarkText` and `lut3dFile`; set advertised
text fields with `--set-text name=value`. `--watermark-text` remains a
convenience spelling for the watermark value.
Canonical-mask names are not a closed list; the result includes `prefixes` for
`colorHarmonizerMask` and `graduatedMask`. Unknown, duplicate, non-finite, or
out-of-range `--set` values fail closed.

## Validate or import a recipe

Validate an existing recipe:

```text
ravo recipe validate "/work/photo.recipe.json" --json
```

Convert an XMP sidecar into a new recipe file:

```text
ravo recipe import-xmp "/work/photo.xmp" \
  --asset-id asset-123 \
  --input "file:///photos/photo.cr2" \
  --output "/work/photo.recipe.json" --json
```

The converter accepts only strict, evidenced leftover darktable state, or a
Camera Raw Settings (`crs:`) document. Empty leftover history and specific
default-unmasked operation records have supported mappings. A CRS preset maps
onto accepted Develop owners. Unknown `crs:` keys, Kelvin/tint white balance,
custom DCP profiles, and mixed darktable+CRS documents return `unsupported`.
Adobe Standard is not applied and is listed in `omitted`. Masks, custom leftover
blend state, multiple or conflicting instances, unknown data, malformed
payloads, and unsupported history combinations also return `unsupported` or
`validation`; they are not silently approximated.

## Create and list a catalog

```text
ravo catalog create --path "/work/Ravo Library.sqlite" --json
ravo catalog list --catalog "/work/Ravo Library.sqlite" --json
```

`create` refuses to replace an existing database. `list` returns asset IDs,
media types, normalized URIs, review state, tags, metadata, capture values, and
whether an asset has edits. `capture.captured_at` is ISO local time with exact
source subseconds and the source UTC offset only when the file supplied one;
unzoned times never gain `Z`, and the field is `null` when no local capture
time exists. `capture.gps` is `null` without a complete latitude/longitude pair;
coordinates are scaled-decimal numbers with at most six fractional digits, and
`altitude_m` is omitted when altitude is absent. Filter by a catalog tag with:

```text
ravo catalog list --catalog "/work/Ravo Library.sqlite" \
  --tag landscape --json
```

## Inspect recovery state and create a catalog backup

Every durable asset, review, metadata, recipe, or history mutation advances an
asset-local recovery generation. The derived checksummed JSON is stored under
`<catalog>.ravo/sidecars/`, never beside an original and never as a second
live edit authority.

List pending generations for the catalog, or inspect one asset even when it is
already synchronized:

```text
ravo catalog sidecar-status --catalog "/work/Ravo Library.sqlite" --json
ravo catalog sidecar-status --catalog "/work/Ravo Library.sqlite" \
  --asset-id asset-123 --json
```

Synchronize all pending generations, or one named asset:

```text
ravo catalog sidecar-sync --catalog "/work/Ravo Library.sqlite" --json
ravo catalog sidecar-sync --catalog "/work/Ravo Library.sqlite" \
  --asset-id asset-123 --json
```

`sidecar-status` without an asset returns only pending states. A sync result
reports the support root, pending counts before/after, and each published
artifact's asset ID, generation, path, byte count, and SHA-256. A catalog write
can be committed even when recovery publication fails; structured context then
reports that recovery remains pending for a later open, close, sync, or backup.

Create a backup at a directory that does not yet exist, then verify it without
opening its snapshot as a live catalog:

```text
ravo catalog backup --catalog "/work/Ravo Library.sqlite" \
  --backup "/backups/Ravo-2026-08-31" --json
ravo catalog backup-verify --backup "/backups/Ravo-2026-08-31" --json
```

Creation drains pending recovery, snapshots and integrity-checks the database,
removes rebuildable preview rows, copies the exact recovery generations, and
publishes the directory without replacement. The backup contains
`catalog.sqlite`, `manifest.json`, and `sidecars/`; its JSON result reports
hashes, byte counts, schema/revision identity, and `verified: true`.
`backup-verify` accepts no `--catalog` argument and is read-only.

Catalog backups deliberately exclude originals and preview files. Back up the
referenced originals separately. Restore to a new absent path, rebuild previews,
or configure verified retention with:

```text
ravo catalog backup-restore --backup "/backups/Ravo-2026-08-31" \
  --output "/work/Restored Ravo Library.sqlite" --json
ravo catalog preview-rebuild --catalog "/work/Restored Ravo Library.sqlite" --json
ravo catalog backup-policy --catalog "/work/Ravo Library.sqlite" \
  --schedule-dir "/backups/Ravo" --interval-minutes 1440 \
  --retention-count 7 --enabled true --json
ravo catalog backup-run --catalog "/work/Ravo Library.sqlite" --json
```

`backup-restore` never overwrites or merges a destination. Scheduled retention
removes only canonical artifacts that reverify as the current catalog.

List stable direct folders and explicitly relink a missing one:

```text
ravo catalog folders --catalog "/work/Ravo Library.sqlite" --json
ravo catalog folder-relink --catalog "/work/Ravo Library.sqlite" \
  --folder-id <folder-id> --replacement "/new/photo/folder" --json
```

Relink is identity-checked and transactional; it does not search by a similar
filename or write an original.

## Import files or directories

```text
ravo catalog import --catalog "/work/Ravo Library.sqlite" \
  --input "/photos/one.jpg" --input "/photos/raw" --json
```

The `--input` option can be repeated. Each item is reported as `imported`,
`duplicate`, `unsupported`, or `failed`. The command stores references and
rebuildable previews; it does not copy the source files into the catalog.

## Generate a preview

```text
ravo catalog preview --catalog "/work/Ravo Library.sqlite" \
  --asset-id asset-123 --max-edge 1600 --json
```

The result contains the preview cache path, dimensions, and an
`original_missing` flag. `--max-edge` must be a positive integer when supplied.

## Run a non-persistent Develop probe

`catalog probe` renders the current recipe without changing the recipe or
preview records:

```text
ravo catalog probe --catalog "/work/Ravo Library.sqlite" \
  --asset-id asset-123 --set exposure=0.5 --set toneEqMidtones=0.2 \
  --max-edge 512 --json
```

Use `--baseline` to probe the synthesized product baseline instead of the
stored edit:

```text
ravo catalog probe --catalog "/work/Ravo Library.sqlite" \
  --asset-id asset-123 --baseline --set contrast=0.1 --json
```

The result includes output profile, dimensions, channel statistics, clipping
counts, and display-luma mean. Unknown, duplicate, non-finite, or out-of-range
Develop fields fail before publication. The command guarantees
`recipe_unchanged: true` and `preview_records_unchanged: true` on success.

Optional `--output` writes a throwaway display PNG of the in-memory probe
pixels. It is not a catalog preview record, must end in `.png`, and never
replaces an existing file:

```text
ravo catalog probe --catalog "/work/Ravo Library.sqlite" \
  --asset-id asset-123 --baseline --set exposure=0.5 \
  --output "/tmp/probe.png" --json
```

## Save Develop values

`catalog develop` applies values and saves the resulting recipe:

```text
ravo catalog develop --catalog "/work/Ravo Library.sqlite" \
  --asset-id asset-123 --exposure-ev 0.5 \
  --set highlights=-0.2 --set toneEqMidtones=0.15 \
  --set watermarkEnabled=1 --watermark-text "RAVO {stem}" --json
```

Apply a Lightroom CRS preset onto the current Develop recipe:

```text
ravo catalog develop --catalog "/work/Ravo Library.sqlite" \
  --asset-id asset-123 --from-xmp "/presets/look.xmp" --json
```

That overlays mapped look groups and keeps crop, masks, Retouch, and profiles.
`--from-xmp` can be combined with later `--set` values.

Convenience flags are available for `--exposure-ev`, `--saturation`, and
`--contrast`. Use repeated `--set name=value` for the numeric Develop fields
exposed by the current recipe contract, including geometry, profiles, white
balance, color, RAW repair, lens, tone, and effect fields. Values must be finite
and each field keeps its own validation bounds. Discover the current names and
ranges with `ravo develop-fields --json`.

Texture uses `texture` in `[-2,2]`, `textureDetailThreshold` in
`[0.01,100]` original-input pixels, and integer `textureIterations` in `[1,5]`.
It is ordered before Sharpen; `texture=0` is identity and is omitted from a
canonical recipe.

Use repeated `--set-text name=value` for advertised text fields. For example,
the following selects a profile-explicit 3D LUT; space index `3` is Linear
Rec709 and interpolation index `0` is tetrahedral:

```text
ravo catalog develop --catalog "/work/Ravo Library.sqlite" \
  --asset-id asset-123 --set-text lut3dFile="/looks/portrait.cube" \
  --set lut3dInputSpaceIndex=3 --set lut3dOutputSpaceIndex=3 \
  --set lut3dInterpolationIndex=0 --set lut3dStrength=1 --json
```

Run `ravo lut inspect "/looks/portrait.cube" --json` first when diagnosing a
file. An enabled LUT is validated even at zero strength; resource errors never
become a hidden identity fallback.

## Offline camera-noise calibration

Calibration consumes an explicit version-1 sample document. Means and
variances use black-subtracted uint16 sensor code values; they are not display
RGB or normalized `[0,1]` values. Each sample also carries the number of sensor
observations represented by that estimate:

```json
{
  "identity": {"iso": 800, "make": "Sony", "model": "Example Camera"},
  "samples": [
    {"count": 4096, "signal_mean": 256, "variance": 153},
    {"count": 4096, "signal_mean": 2304, "variance": 1177},
    {"count": 4096, "signal_mean": 4352, "variance": 2201},
    {"count": 4096, "signal_mean": 6400, "variance": 3225},
    {"count": 4096, "signal_mean": 8448, "variance": 4249},
    {"count": 4096, "signal_mean": 10496, "variance": 5273},
    {"count": 4096, "signal_mean": 12544, "variance": 6297},
    {"count": 4096, "signal_mean": 14592, "variance": 7321}
  ],
  "schema": "ravo.camera-noise-samples",
  "units": "black_subtracted_uint16_code_values",
  "version": 1
}
```

Fit and inspect a profile with:

```text
ravo noise calibrate samples.json --output camera-iso800.rnoise.json --json
ravo noise inspect camera-iso800.rnoise.json --json
```

The fitter requires 8–1024 samples spanning at least 256 code values, rejects
malformed/non-finite or insufficient inlier data, and never invents fallback
coefficients. Output is deterministic, versioned and SHA-256 protected. The
destination must not already exist; the command never modifies the sample
file, a photo, a catalog, or an implicit profile directory. Current denoisers
do not automatically load this artifact.

`--watermark-text` sets the one bounded text field. The current fixed-font
contract accepts its documented ASCII subset plus newline and the `{stem}` and
`{asset_id}` tokens; arbitrary SVG paths, system fonts, and EXIF variables are
not CLI compatibility inputs.

To read the stored versioned recipe without changing it:

```text
ravo catalog recipe --catalog "/work/Ravo Library.sqlite" \
  --asset-id asset-123 --json
```

## Review state, tags, and metadata

Set a rating from 0 to 5:

```text
ravo catalog rate --catalog "/work/Ravo Library.sqlite" \
  --asset-id asset-123 --rating 4 --json
```

Add or remove tags:

```text
ravo catalog tag --catalog "/work/Ravo Library.sqlite" \
  --asset-id asset-123 --add "landscape, dusk" --json
ravo catalog tag --catalog "/work/Ravo Library.sqlite" \
  --asset-id asset-123 --remove dusk --json
```

Write catalog metadata:

```text
ravo catalog metadata --catalog "/work/Ravo Library.sqlite" \
  --asset-id asset-123 --title "Evening ridge" \
  --description "Captured after sunset" --creator "A. Photographer" \
  --copyright "2026 A. Photographer" --json
```

Without write flags, `tag` and `metadata` return the current values. These
operations do not rewrite the original file.

## History and snapshots

List recipe history:

```text
ravo catalog history --catalog "/work/Ravo Library.sqlite" \
  --asset-id asset-123 --json
```

Create and restore a labeled snapshot:

```text
ravo catalog snapshot --catalog "/work/Ravo Library.sqlite" \
  --asset-id asset-123 --label "Client proof" --json
ravo catalog restore --catalog "/work/Ravo Library.sqlite" \
  --asset-id asset-123 --history-id 7 --json
```

Restore validates the stored recipe before making it current. An invalid or
foreign history ID returns `not_found` or a structured validation error.

## Export from the catalog

Render a selected asset:

```text
ravo catalog export --catalog "/work/Ravo Library.sqlite" \
  --asset-id asset-123 --output "/exports/photo.jpg" \
  --format jpeg --quality 92 --jpeg-subsampling auto \
  --metadata no-location --json
```

Export an exact original copy:

```text
ravo catalog export --catalog "/work/Ravo Library.sqlite" \
  --asset-id asset-123 --output "/exports/original.cr2" \
  --format original --json
```

PNG example:

```text
ravo catalog export --catalog "/work/Ravo Library.sqlite" \
  --asset-id asset-123 --output "/exports/photo.png" --format png \
  --png-bit-depth 8 --png-compression 5 --json
```

TIFF example:

```text
ravo catalog export --catalog "/work/Ravo Library.sqlite" \
  --asset-id asset-123 --output "/exports/photo.tif" --format tiff \
  --tiff-compression deflate_predictor \
  --tiff-compression-level 6 --tiff-sample-type uint8 \
  --tiff-grayscale-if-neutral --tiff-resolution-dpi 300 --json
```

Batch example with deterministic portable names:

```text
ravo catalog export-batch --catalog "/work/Ravo Library.sqlite" \
  --asset-id asset-123 --asset-id asset-456 \
  --output-dir "/exports/delivery" \
  --filename-template '{stem}-{sequence}{ext}' \
  --format jpeg --quality 92 --metadata no-location --json
```

The output directory must already exist. Batch preflight rejects duplicate
asset IDs, duplicate expanded names, missing sources, and any existing target
before writing the first item. Runtime failure or cancellation stops at the
failed item and reports any earlier completed outputs in structured context.

Accepted format spellings are `png`, `jpeg`/`jpg`, `tiff`/`tif`, and
`original`/`copy`/`original-copy`. Existing output files return `conflict` and
are never overwritten implicitly.

Refresh capture metadata from the current original without touching the file:

```text
ravo catalog refresh-metadata --catalog "/work/Ravo Library.sqlite" \
  --asset-id asset-123 --json
```

## Create and apply a Recipe Style

```text
ravo recipe style-create source.recipe.json --name "Warm repair" \
  --output warm-repair.rstyle.json --json
ravo recipe style-validate warm-repair.rstyle.json --json
ravo recipe style-apply warm-repair.rstyle.json --asset-id target-asset \
  --input file:///photos/target.jpg --output target.recipe.json --json
```

Styles are versioned Recipe templates. Output paths must be new;
unknown/newer/malformed state and legacy `.dtstyle` fail instead of dropping
operations. The commands above create and apply schema-v1 complete-replacement
styles.

Studio can also create schema-v2 selective presets. To apply either schema to
an existing target recipe, use the explicit target form:

```text
ravo recipe style-apply selected.rstyle.json \
  --target-recipe target.recipe.json --output merged.recipe.json --json
```

A schema-v2 style overlays only its sorted selected logical fields and
preserves unselected target state. It requires `--target-recipe`; the older
`--asset-id` / `--input` form remains valid only for schema-v1 complete
styles and will not silently widen a selective preset.

## Render a standalone recipe to PNG

```text
ravo render "/photos/source.cr2" \
  --recipe "/work/photo.recipe.json" \
  --output "/exports/photo.png" --backend cpu \
  --width 1600 --height 1067 --json
```

The standalone render path accepts only `--backend cpu`. `--width` and
`--height` are optional positive integers. The recipe must validate, and the
output is published atomically as PNG. Use catalog export when you need the
catalog's current saved recipe and format-specific encoder options.

## Exit codes

With or without JSON, a failed command returns a non-zero exit status:

| Error code | Exit status | Meaning |
| --- | ---: | --- |
| `invalid_argument` | 2 | Command syntax or argument shape is invalid. |
| `not_found` | 3 | The catalog, asset, original, or history entry is not found. |
| `validation` | 4 | Data or an option violates a contract. |
| `unsupported` | 5 | The input or requested capability is outside the current product boundary. |
| `io` or `conflict` | 6 | A file/database operation failed or the destination exists. |
| `cancelled` | 7 | The operation was cancelled. |
| `internal` | 70 | An unexpected internal failure occurred. |

## Common questions

### Can I parse stdout safely in automation?

Yes. Use `--json` and read the single protocol object. Diagnostics are an empty
array in the current protocol, and service logs stay out of stdout.

### Why did `catalog probe` not create a preview file?

Probe does not write a catalog preview record or change the stored recipe.
Pass `--output /path/to/probe.png` when you want a throwaway display PNG of the
in-memory result. That file is still not a catalog cache entry, and an existing
path returns `conflict`.

### Why did an unknown `--set` field fail?

Develop fields are versioned and validated. Discover the current names, kinds,
and ranges with `ravo develop-fields --json`. `ravo operations --json` lists
engine operation descriptors, not `--set` field names. Ravo does not ignore a
typo.

### Can the CLI export high-precision PNG and TIFF files?

Yes. PNG 16-bit and TIFF uint16/float16/float32 requests use engine-owned
higher-precision samples from the active recipe. Mismatched low-level RGB8
sources still fail closed instead of fabricating precision.
