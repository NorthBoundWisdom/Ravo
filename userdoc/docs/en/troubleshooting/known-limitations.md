# Known Limitations

## Goal

Identify current product boundaries before treating an explicit unsupported
result as a regression.

**Last verified:** 2026-08-27 against the current migration status and product
baseline.

## Current boundaries

### Local-only library

The current library is a local SQLite catalog with local source references and a
local preview cache. There is no cloud catalog, remote publishing, or automatic
cross-machine relinking.

### No legacy catalog migration

Ravo's catalog schema is independent. It does not open the old application's
catalog in place. Use source-file import and the strict CLI XMP conversion path
where the particular legacy state is representable.

### Strict legacy XMP compatibility

The XMP importer accepts only a versioned, evidenced subset. Empty history and
some default-unmasked singleton operation records have mappings. Masks, custom
blend state, multiple instances, conflicting revisions, malformed data, and
unknown operations reject structurally. A successful import does not imply that
an entire historical editing session is replayable.

### CPU render backend

The supported standalone CLI render backend is `cpu`. Ravo does not expose the
old OpenCL path, and the current GPU work is not a user-selectable fallback.

### Original-safe editing

Import, review, Develop, preview, and rendered export do not write back to the
source file. The explicit **Delete from Disk** command is the one destructive
source-file action and requires confirmation.

### Missing originals are not automatically relinked

Ravo retains the record and recipe for a missing source, but the current Studio
has no relink-by-search workflow. Restore the original at its recorded path.

### Export scope

Studio exports the active photo only. Multi-selection can update review state
and catalog metadata, but it is not an implicit batch-export job.

Studio currently selects the output format but does not expose JPEG quality or
the typed PNG/TIFF controls. Use `ravo catalog export` for those CLI options.

### Output precision and metadata

The current rendered source is RGB8. PNG 16-bit and TIFF high-precision sample
requests are validated but fail as unsupported rather than fabricating
precision. General EXIF/IPTC/XMP packet writing, GPS/timezone policy, generated
sidecars, and full history attachment are not current general export contracts.

### Format-specific input layouts

A recognized extension is not a guarantee of decodability. Unsupported TIFF
pages/layouts, PNG encodings or color metadata, invalid ICC profiles, and RAW
sensor/container combinations return explicit results. The [format coverage
matrix](../qa/format-coverage.md) lists the baseline expectations.

### Media-specific Develop controls

RAW repair, camera metadata modes, and some profile operations require the
corresponding source state. Ravo rejects an invalid or unsupported combination;
it does not replace it with a hidden generic algorithm.

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
