# Known Limitations

## Goal

Identify current product boundaries before treating an explicit unsupported
result as a regression.

**Last reviewed:** 2026-08-31 against the current migration status and product
baseline.

## Current boundaries

### Local-only library

The current library is a local SQLite catalog with local source references and a
local preview cache. Catalog-owned recovery mirrors and verified CLI backups
are local filesystem artifacts. There is no cloud catalog, remote publishing,
or automatic cross-machine relinking.

### No legacy catalog migration

Ravo's catalog schema is independent. It does not open the old application's
catalog in place. Use source-file import and the strict CLI XMP conversion path
where the particular legacy state is representable.

### Strict legacy XMP compatibility

The XMP importer accepts only a versioned, evidenced subset. Empty history and
some default-unmasked singleton operation records have mappings. Masks, custom
blend state, multiple instances, conflicting revisions, malformed data, and
unknown operations reject structurally. A successful import does not imply that
an entire historical editing session is replayable. Color Harmonizer maps only
the evidenced zero-smoothing singleton records; canonical Ravo recipes may use
positive smoothing, but synthetic positive legacy payloads, masks, and other
unsupported history in the same document still reject.
Color Reconstruction maps only the single evidenced 0052 enabled-v3,
default-unmasked singleton. Disabled state, other versions, masks, custom
blend state, and multiple instances reject rather than inheriting the old GTK
preview-grid lifecycle.
Sharpen maps only the three evidenced enabled-v1 default-unmasked singleton
records. Demosaic capture sharpening is a different RAW-stage capability, and
unsupported masks or blend state do not fall back to the old approximation.
Haze Removal maps only the evidenced v1/v2 default-unmasked singleton records.
The accepted algorithm requires source-linear RAW; JPEG/PNG/TIFF Develop does
not run it after input-profile conversion, and no constant-airlight fallback is
used.
Retouch maps only the five evidenced v1 revisions and their v6 circle, ellipse,
path, brush, group, and source payloads from the four frozen fixture families.
Their complete documents still reject when unrelated `rawprepare` or
`basecurve` state is outside its own accepted mapping. Studio currently authors
circle regions; imported canonical path/brush regions remain renderable but are
not reshaped by the Retouch panel.
Canvas maps only the evidenced 0157 v1 singleton, and Output Frame maps only
the 0030 v3 and 0154/0155 v4 singletons with their exact default blend and
reserved fields. Other versions, masks, custom blend, multi-instance, or
modified payloads reject.
The sole Watermark record names `promo.svg`, which is absent from the frozen
repository. Ravo rejects it instead of reproducing the old silent no-op.
Color Zones maps only the exact enabled 0022 v5 singleton with its default
unmasked blend. The complete 0022 document still rejects because its FilmicRGB
operation is outside the accepted mapping.
Monochrome maps only the exact enabled 0017 v2 singleton with its default
unmasked blend. The complete historical document still rejects when unrelated
operations have no accepted mapping.
Split Toning maps only the exact enabled 0062 v1 singleton with its default
unmasked blend; other payload, mask, blend, or multi-instance states reject.
Velvia maps only the exact enabled 0063 v2 singleton with its default unmasked
blend; other payload, enabled state, mask, blend, or multi-instance state
rejects.
The three frozen 3D-LUT histories contain mutable machine-local external paths,
not the referenced LUT bytes or a content checksum. Ravo does not guess or
relocate those resources: legacy import returns
`unsupported_legacy_lut3d_resource`. Select the original `.cube` explicitly in
Ravo and declare the colour spaces it was authored for. The initial adapter
supports 3D `.cube` files with tetrahedral or trilinear interpolation; 1D,
pyramid, Hald image, OCIO, and CTL inputs are unsupported.

### CPU render backend

The supported standalone CLI render backend is `cpu`. Ravo does not expose the
old OpenCL path, and the current GPU work is not a user-selectable fallback.

### Original-safe editing

Import, review, Develop, preview, and rendered export do not write back to the
source file. The explicit **Delete from Disk** command is the one destructive
source-file action and requires confirmation.

### Missing originals are not relinked by search

Ravo retains the record and recipe for a missing source. A missing stable direct
folder can be explicitly relinked in Studio or by folder ID in the CLI, but
Ravo does not search for a similar filename, accept changed identity, or infer
a new hierarchy-only parent. Restore an individual renamed file to its recorded
path.

### Catalog backup is not original-media backup

The CLI and Studio can synchronize catalog-owned recovery generations, create
or verify an immutable backup, restore it to a new absent catalog, rebuild
previews, and schedule verified retention. The artifact still excludes
originals and rebuildable previews, and there is no cloud target. Do not open
the internal snapshot directly or treat catalog restore as recovery of missing
RAW/raster originals.

### Export scope

Studio exports one active photo through a save-file dialog or an explicit
multi-selection through a folder plus filename template. CLI exposes the same
bounded batch contract. There is no persistent background export queue,
remembered last codec value, or reusable export-option preset.

### Output precision and metadata

PNG 16-bit and TIFF uint16/float16/float32 product export use engine-owned
samples from the active recipe. An 8-bit source still fails closed rather than
fabricating precision. Validated capture time and GPS from the Catalog are
embedded on rendered JPEG/PNG/TIFF under explicit full, no-location, or none
privacy.
Ravo intentionally does not automatically read, attach, generate, watch, or
merge adjacent interchange sidecars. Use the explicit strict CLI XMP conversion
when needed; rendered XMP is embedded in the destination. Catalog-owned
`.ravo.json` recovery mirrors are generated under the catalog support
directory, but they are durability artifacts and never implicit edit input.
Full historic edit-history packet attachment remains outside the current
interchange contract.

### Format-specific input layouts

A recognized extension is not a guarantee of decodability. Unsupported TIFF
pages/layouts, PNG encodings or color metadata, invalid ICC profiles, and RAW
sensor/container combinations return explicit results. The [format coverage
matrix](../qa/format-coverage.md) lists the baseline expectations.

### Media-specific Develop controls

RAW repair, camera metadata modes, and some profile operations require the
corresponding source state. Ravo rejects an invalid or unsupported combination;
it does not replace it with a hidden generic algorithm.

Canvas is opaque solid growth. Perspective/straighten and crop can follow it
because pixels and preview-mask alpha share the same transform. Post-Canvas
rotate, flip, or lens geometry, attached sub-ROI mask evaluation, and another
masked operation after composed geometry return a structured unsupported
result. Output Frame is final encoded-output decoration, not transparent
padding.

Text Watermark uses a built-in 5×7 ASCII font and only `{stem}` and
`{asset_id}` tokens. Arbitrary SVG/PNG watermark files, system fonts, Unicode
glyphs, and EXIF/tag variable templates are not supported in schema v1.

Studio directly edits eight-band Color Zones curves. Canonical or imported
2–20-node curves and attached masks remain preserved but read-only in that
panel; use reset to create a new eight-band identity rather than silently
reshaping custom nodes.

Studio preserves an attached Monochrome mask but does not edit that mask in the
Monochrome panel. Use a canonical mask-capable owner or recipe tooling rather
than expecting the colour-filter controls to reshape it.

Studio preserves an attached Split Toning mask but does not edit that mask in
the Split Toning panel.

Studio preserves an attached Velvia mask but does not edit that mask in the
Velvia panel.

### Platform acceptance

The repository contains macOS, Windows, and Linux presets. Build, Qt runtime,
packaging, and manual Studio acceptance remain platform-specific. A result
validated on one host is not evidence that every packaged platform has passed.

## Cases to report as bugs

Report a reproducible issue when:

- a clearly supported input crashes or corrupts the catalog;
- a valid destination is overwritten or a conflict is ignored;
- a committed edit, review value, tag, or history entry disappears after reopen;
- a late preview replaces a newer edit;
- a supported Studio command has no visible reason when unavailable;
- a source file changes during a normal non-destructive workflow.

Include the platform, build preset, source format, exact command or UI path,
message text, and whether the issue reproduces after restarting with a copy of
the catalog.
