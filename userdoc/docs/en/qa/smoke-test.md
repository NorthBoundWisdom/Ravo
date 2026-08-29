# Smoke Test

## Goal

Validate the shortest complete Ravo path from launch to catalog, preview,
review, edit, and export.

**Last verified:** 2026-08-27 against the current Studio and CLI acceptance
paths.

## Prerequisites

- A built `ravo_studio` executable.
- A writable temporary or test directory.
- One known-good JPEG or PNG and, when RAW coverage is needed, one
  LibRaw-supported Bayer RAW fixture.

## Minimum Studio path

1. Launch Studio and create a new `.sqlite` library.
2. Import one known-good file and wait for Import and Previews to finish.
3. Confirm the asset appears in Gallery and its folder appears in the Library
   tree.
4. Select the asset and enter Loupe.
5. Exercise Fit, Fill, 100%, pan, and the left navigator.
6. Switch the right scope through Histogram, Waveform, Parade, Vectorscope, and Split.
7. Set a rating, a color label, and Reject/Keep; confirm the tile updates.
8. Enter Edit, change one Light or Color control, then enable Canvas, Output
   Frame, or Text Watermark; confirm preview, Before/After, Undo, and Redo.
9. Create a labeled Snapshot, change a control again, and restore the snapshot.
10. Export a PNG or JPEG to a new path and verify its dimensions and bytes.
11. Attempt the same export path again and confirm that a conflict is reported
    and the first file is unchanged.
12. Close and reopen the same library; confirm review state, recipe, history,
    and preview are restored.

## Recovery checks

- Import the same source twice and confirm the second result is `duplicate`.
- Import a known unsupported or malformed candidate and confirm an explicit
  unsupported/failed result.
- Temporarily make an imported original unavailable and confirm Missing state
  without losing catalog review data.
- Use **Remove from Catalog** and confirm that the original remains on disk.
- Test **Delete from Disk** only with a disposable copy and confirm the explicit
  confirmation boundary.

## CLI path

```text
ravo --version --json
ravo inspect <known-good-input> --json
ravo catalog create --path <test-catalog.sqlite> --json
ravo catalog import --catalog <test-catalog.sqlite> --input <known-good-input> --json
ravo catalog list --catalog <test-catalog.sqlite> --json
ravo catalog preview --catalog <test-catalog.sqlite> --asset-id <id> --json
ravo catalog probe --catalog <test-catalog.sqlite> --asset-id <id> --baseline --json
ravo catalog export --catalog <test-catalog.sqlite> --asset-id <id> \
  --output <test-output.png> --format png --json
ravo catalog export-batch --catalog <test-catalog.sqlite> \
  --asset-id <id> --output-dir <existing-output-directory> \
  --filename-template '{stem}-{sequence}{ext}' --format png --json
```

The CLI path must use the same catalog and source as Studio when checking
cross-client persistence. Do not use the old CLI, old CTest project, or
`legacy/tests/run` as a live oracle.

## Pass criteria

- The library opens and reopens without losing records.
- Import, duplicate, unsupported, missing, and conflict results are explicit.
- Review state and recipe edits persist across restart.
- Preview and export never overwrite the original or an existing destination.
- CLI JSON has one `ravo.cli.result` object and a correct `ok` value.
