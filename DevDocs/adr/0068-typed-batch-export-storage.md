# ADR-0068: Batch export uses a typed filename template and no-replace storage

- Status: Accepted
- Date: 2026-08-29
- Extends: [ADR-0032](0032-encoded-byte-publication-contract.md)
- Extended by: [ADR-0113](0113-studio-export-long-edge.md),
  [ADR-0117](0117-export-box-sharpen-presets-and-restartable-jobs.md)

## Context

The old disk storage plugin combined GTK, global configuration, a broad string
variable language, parallel sequence mutation, directory creation, and four
overwrite policies. Ravo already had typed per-format export and atomic
no-replace publication, but only for one explicitly named destination. I14
requires a bounded batch/path owner without reviving that dynamic ABI.

## Decision

- `ExportOptions` is the single typed format/codec/size/privacy value shared by
  one-item and batch requests. A batch contains 1–10,000 unique ordered asset
  IDs, one existing output directory, a cancellation token, and a filename
  template of at most 512 UTF-8 bytes.
- The only template tokens are `{stem}`, `{asset_id}`, `{sequence}`, and
  `{ext}`. Sequence is one-based and at least four digits. Rendered extensions
  are canonical `.jpg`, `.png`, or `.tif`; original copy keeps the source
  extension. If `{ext}` is absent it is appended once.
- Expansion produces one flat portable filename of at most 240 UTF-8 bytes.
  Separators, controls, Windows-forbidden characters/device names, stray or
  unknown braces, empty components, and trailing dot/space reject. There is no
  shell expansion, date/camera/config lookup, or compatibility variable parser.
- CatalogService preflights the complete ordered set before writing: output
  directory, unique assets, readable regular originals, expanded-name
  uniqueness, and every existing file/symlink/directory conflict. A known
  conflict therefore publishes zero batch items.
- Each item then calls the ordinary `export_asset` owner and its atomic
  no-replace destination primitive. Races still lose safely. Overwrite,
  overwrite-if-changed, skip, and unique-name guessing are not product modes.
- Cancellation is checked during preflight, between items, and by rendering,
  encoding, copying, and publication. A runtime failure stops immediately.
  Successfully delivered earlier files are user output and are never rolled
  back; the error names completed/total counts, failed index, asset, output,
  and whether the batch is partial.
- CLI exposes `catalog export-batch`. Studio uses the same options dialog: one
  selection opens a save-file dialog, while multiple selections expose the
  template and open a folder dialog. QML forwards values only.

## Consequences

The old `imageio/storage/disk.c` module, its CMake registration, and its stale
workspace configuration key are removed. The shared
`imageio_storage_api.h`/loader and old export-job consumers remain until U10
and J2 reach zero; I14's Ravo contract and exclusive disk owner are accepted,
but the whole dynamic image-I/O ABI is not yet retired.

## Rejected alternatives

- Importing the old variable/config language. It exposes unrelated mutable
  application state and has no stable machine contract.
- Overwrite or overwrite-if-changed. Neither can preserve ADR-0032's
  no-clobber guarantee under races.
- Rolling back earlier successful files after a later failure. Those paths may
  already be consumed externally; deletion would be a new destructive action.
- Expanding paths in QML or the CLI. Domain validation and service preflight
  must be identical for every client.
