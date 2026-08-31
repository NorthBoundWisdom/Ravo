# Import Failures

## Goal

Distinguish an unsupported file, an unreadable source, a duplicate, and a
directory or runtime problem without losing the catalog state you already have.

**Last reviewed:** 2026-08-31 against the current import candidate and decoder
paths.

## Applies to

- Studio file and folder import.
- `ravo catalog import`.

## First checks

1. Confirm that the failure happened during file selection, directory scanning,
   decoding, database commit, or preview generation.
2. Confirm the source suffix and whether the input is a file or a folder.
3. Check the exact item status and error message. Studio reports imported,
   duplicate, unsupported, and failed counts separately.
4. Retry one known-good file from the same folder to separate a source problem
   from a catalog or runtime problem.

## Common failure types

### Unsupported input

The extension may be an import candidate while the actual container, pixel
layout, compression, color profile, or RAW sensor is outside the current
decoder contract. Ravo reports `unsupported`; it does not substitute a generic
decoder or silently import a blank record.

Check the [format coverage](../qa/format-coverage.md) page and try a supported
baseline JPEG, PNG, TIFF, or LibRaw RAW file.

### Corrupt or unreadable file

An existing file can still fail if its header, pixel data, ICC state, or metadata
is malformed. Check that the file opens in an independent viewer and that the
Ravo process has read permission.

### Duplicate

`duplicate` is a normal import result. Ravo recognized a source already present
in the catalog and did not create a second asset record.

### Folder scan returns fewer files than expected

Directory import is recursive but ignores hidden filenames and considers only
known raster/RAW candidate suffixes. It also sorts and de-duplicates paths. Use
**Import Photos → All files** for an individual source with an uncommon suffix,
then check whether its actual decoder supports the content.

### Import is busy

Studio does not start a second import while the current import or preview warmup
is active. Wait for the Import and Previews meters to finish. A cancelled or
failed item remains represented by its explicit result; it is not treated as a
successful import.

### Preview failed after the asset was imported

The catalog record and source reference can exist even when preview generation
fails. Select the asset again after correcting the source or runtime problem.
If the original path is missing, follow
[File paths, backups, and recovery](file-paths-and-recovery.md).

### Required Qt plugin is missing

The current build requires JPEG, GIF, WebP, TIFF, and QSQLITE Qt runtime
plugins. A missing plugin is a build/configuration failure, not a reason to
reinterpret the file as another format. Install the complete Qt kit and rebuild
or redeploy the application.

## CLI triage

Use JSON to preserve the exact machine-readable error:

```text
ravo inspect "/photos/problem.cr2" --json
ravo catalog import --catalog "/work/Ravo Library.sqlite" \
  --input "/photos/problem.tif" --json
```

Use `ravo inspect` for a RAW input; raster files such as TIFF should be checked
through `catalog import` and `catalog preview`. The outer result has `ok: false`
when a command fails; inspect `error.code`, `error.message`, and
`error.context`. The exit code distinguishes invalid arguments, unsupported
inputs, not-found paths, I/O failures, and conflicts.

## Result

You can classify the item before retrying and keep successful catalog records
intact. No import troubleshooting step modifies the source file.

## Common questions

### Why did importing a folder not import every file in it?

Ravo intentionally filters hidden names and known candidate extensions. A
candidate can still be rejected after decoding if its contents are unsupported.

### Should I rename an unsupported file to `.jpg`?

No. Renaming changes only the suffix and cannot make the content a JPEG. Use a
real conversion tool if you intentionally need a different format, then import
the resulting file as a new source.

### Does a failed preview mean the original was changed?

No. Import and preview are read-only with respect to the original. The preview
cache is separate from the source and catalog database.
