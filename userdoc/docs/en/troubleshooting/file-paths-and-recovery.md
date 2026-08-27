# File Paths and Recovery

## Goal

Recover from missing originals, unavailable destinations, preview-cache issues,
or catalog-open problems while preserving the files that still exist.

**Last verified:** 2026-08-27 against the current catalog, cache, and atomic
publication contracts.

## Applies to

- Ravo Studio's local catalog and preview cache.
- Local `ravo` CLI workflows.

## Original files are references

Import records a normalized local path and a source fingerprint. It does not
copy the source into the library. Normal editing, preview generation, metadata
changes, and rendered export do not rewrite it.

This makes the source folder part of the library's operational setup. Keep the
source readable at the recorded path, or keep a filesystem layout that resolves
the same path when moving a catalog between machines.

## Missing original

Typical signs are:

- A thumbnail or image surface says **Missing**.
- The right Photo panel still shows catalog review state but no new preview can
  be generated.
- Export returns an original-not-found error.

The catalog record, rating, color label, reject state, tags, metadata, recipe,
and history remain stored. Restore the file to its recorded path, then select
the asset again so Studio can request a new preview.

There is no automatic relink-by-filename workflow in the current baseline. Do
not delete the catalog record merely because the source is temporarily
unmounted.

## Preview cache

The default cache is adjacent to the catalog:

```text
<catalog>.sqlite.preview/
```

It contains rebuildable PNG previews and is not the source of truth for recipes
or review state. If a cache entry is missing, Ravo can regenerate it from a
readable original. If the source is missing, cache regeneration cannot succeed.

Do not treat a cached preview as an archival copy of the original RAW or raster
file. For backup, preserve both the SQLite catalog and the referenced source
files.

## Catalog open errors

- **Catalog database does not exist**: use Open Library on the correct path, or
  create a new library.
- **Catalog database already exists**: Create Library will not overwrite it;
  choose another path or open the existing file.
- **Catalog is missing schema information / invalid**: the file is not a valid
  Ravo catalog or is damaged. Preserve a copy before attempting recovery.
- **Catalog schema is newer than this Ravo**: use a Ravo build that understands
  that schema; the current build does not downgrade it.

Ravo's catalog schema is independent from the old application. Do not point
Ravo at a legacy database and expect an in-place migration.

## Output path problems

Rendered export and original-copy export use atomic no-replace publication:

- An existing destination returns `conflict`.
- A missing or unwritable parent directory returns an I/O failure.
- A cancelled or failed write does not become a successful partial export.

Choose a new output path and verify that its parent directory is writable. If a
previous output is important, preserve it before retrying.

## Safe recovery sequence

1. Stop the current import or export if it is still running.
2. Copy the catalog file before attempting filesystem repair.
3. Verify that the original path exists and is readable.
4. Reopen the catalog and select the affected asset.
5. Let Ravo rebuild the preview, then verify the image and review state.
6. Export to a new destination and check the result independently.

## Result

Recovery distinguishes catalog records, source files, cache files, and output
files. You can restore one layer without accidentally deleting another.

## Common questions

### Can I delete the `.preview` directory to fix a stale preview?

Previews are rebuildable, but remove or replace cache data only when Studio is
closed and keep a backup if the workspace is important. The catalog and original
files are the durable inputs.

### Will reopening the catalog relink moved photos?

No. Reopening reloads the stored paths. Restore the source path or use an
external filesystem arrangement that makes the recorded path valid again.

### Does Remove from Catalog delete the original?

No. **Remove from Catalog** leaves the original on disk. **Delete from Disk** is
the separate, confirmed, irreversible action.
