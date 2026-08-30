# Regression Checklist

Use this checklist when a change affects catalog, import, preview, Develop,
desktop, or export behavior. It is intentionally focused on user-observable
contracts.

## Library and import

- [ ] Create a new library at a new path.
- [ ] Open the same library after closing Studio.
- [ ] Import one supported raster file.
- [ ] Import one supported RAW file when the target test includes LibRaw.
- [ ] Import a directory recursively.
- [ ] Confirm hidden files are not unexpectedly imported.
- [ ] Re-import the same source and confirm duplicate handling.
- [ ] Exercise malformed, unsupported, missing, and unreadable inputs.
- [ ] Confirm Import and Previews progress finish with recoverable results.

## Browse and review

- [ ] Select All Photographs and a nested source folder.
- [ ] Filter by tag, rating minimum/exact value, color, and reject state.
- [ ] Sort by import time, filename, and rating in both directions.
- [ ] Use additive Cmd/Ctrl selection and Shift range selection.
- [ ] Apply rating 0–5, every color label, and Keep/Reject.
- [ ] Edit tags and Studio-visible writable metadata.
- [ ] Confirm capture information remains read-only.
- [ ] Remove from Catalog and confirm the original remains.
- [ ] Test Delete from Disk only with disposable source data.

## Viewer and Develop

- [ ] Open Gallery, Loupe, and Edit.
- [ ] Exercise Fit, Fill, 1:1, click-to-1:1 restore (smooth zoom, not a camera jump), wheel zoom, pan, and navigator seeking.
- [ ] Switch Histogram, Waveform, Parade, Vectorscope, and Split scopes.
- [ ] Confirm a missing original shows Missing without losing catalog state.
- [ ] Commit a geometry, Light, and Color change.
- [ ] Exercise Canvas placement/colour and confirm masks exclude the added area.
- [ ] Exercise Output Dither and Output Frame; confirm preview and export share
      the framed dimensions.
- [ ] Add a Text Watermark with a token; confirm preview, reopen, style reuse,
      and rendered export keep the same placement and pixels.
- [ ] Exercise Color Zones with each selection axis and interpolation; confirm
      custom imported nodes stay preserved when Studio cannot edit them.
- [ ] Exercise Monochrome filter a*/b*, size, highlights, mix, and a mask;
      confirm preview scale and full export keep the intended filter response.
- [ ] Exercise Split Toning shadow/highlight hue+saturation, balance,
      compression, mix, and an attached mask.
- [ ] Exercise Velvia strength and mid-tones bias at both endpoints; confirm
      preview, save/reopen, export, reset, and an attached mask agree.
- [ ] Confirm interactive preview cannot replace a newer committed request.
- [ ] Exercise Before/After, per-control reset, section reset, Reset all,
      Undo, Redo, and Copy/Paste Edits onto a second photo.
- [ ] Create, list, and restore a labeled snapshot.
- [ ] Close and reopen; confirm recipe, history, and Edited state persist.

## Export and CLI

- [ ] Export PNG, JPEG, TIFF, and Original copy to new destinations.
- [ ] Confirm an existing destination returns conflict and remains unchanged.
- [ ] Confirm rendered export uses the saved recipe.
- [ ] Batch-export at least two selected assets with a filename template; check
      ordering, conflict preflight, and partial cancellation reporting.
- [ ] Confirm original copy does not render or rewrite the source.
- [ ] Test JPEG quality bounds and TIFF option validation through CLI.
- [ ] Test PNG 16-bit and TIFF uint16/float16/float32 product export succeed, and mismatched 8-bit sources still fail closed.
- [ ] Run `--json` commands and parse the single `ravo.cli.result` object.
- [ ] Check exit status for invalid argument, not found, unsupported, conflict,
      cancellation, and I/O failures where the test harness can induce them.

## Platform and delivery record

Record:

- platform and OS version;
- build preset and Ravo version;
- Qt runtime/plugin set;
- source formats and file sizes;
- exact failing step and message;
- whether the check was run in Studio, CLI, or both;
- whether the check was not applicable or not run.

Only claim the platform and checks actually executed. Never configure, build, or
run the frozen `legacy/` application as part of this checklist.
