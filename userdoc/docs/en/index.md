# Ravo User Handbook

Ravo is an open-source RAW photo editor, local photo library manager, and
non-destructive color grading application for Windows, macOS, and Linux. It is a
complete C++20 and Qt 6 redesign that takes the photo workflow and image
processing of darktable 0.9 as its reference, and it is under active
development.

Ravo Studio brings cataloging, browsing, culling and review, non-destructive
Develop, and export into one desktop workspace. The `ravo` command-line client
exposes the same catalog and image engine for scripts, CI, and headless
workflows.

## Three things to know first

- Import is reference-only. Ravo records the original path and reads the
  source; it does not copy, move, rename, or rewrite the original during
  import or editing.
- The library is a local SQLite file. Rebuildable preview PNGs live outside
  the database, beside it in a `<catalog>.preview/` directory. Catalog-owned
  recovery JSON lives separately under `<catalog>.ravo/sidecars/`.
- Editing is non-destructive. Develop settings are stored as versioned recipes;
  previews and exports use the same CPU image engine.

!!! note

    This handbook describes the current repository-backed development baseline.
    The repository workflow defines macOS, Windows, and Linux preset variants,
    but the generated preset file and final installation acceptance are
    platform-specific. Use the validation status in the build you received
    rather than assuming that one platform's result applies to all others.

## What you can do today

- Create or open a local library and import individual files or directories.
- Browse a Gallery grid, select one or more photos, and inspect a primary photo
  in Loupe.
- Use Fit, Fill, 1:1, click-to-1:1, pan, a navigator, an RGB histogram, and an RGB parade.
- Rate photos from 0 to 5, apply color labels, reject or keep photos, search and
  filter by media/edit/review/tag/folder/capture fields, and sort deterministically.
- Edit a selected photo with geometry, profile, exposure, color, detail,
  effects, RAW repair, lens, and tone controls.
- Compare Before and After as a toggle or synchronized left/right view, reset
  controls or sections, undo and redo changes, create snapshots, and restore
  recipe history.
- Save selected modified parameters as a managed preset, or copy an explicit
  parameter subset to another photo without resetting unrelated edits.
- Store catalog tags and writable metadata without changing the source file.
- Export a rendered photo as PNG, JPEG, or TIFF, or make an exact original
  copy. Existing destination files are never overwritten implicitly.
- Run the same catalog, preview, recipe, and export paths through `ravo`.
- Inspect or synchronize recovery generations and create or verify an immutable
  catalog backup through `ravo`. Backups exclude originals and previews.
- Switch Studio among English, German, Spanish, French, Brazilian Portuguese,
  Simplified or Traditional Chinese, Japanese, and Korean.
- Open a floating Assistant panel and configure its URL, model, and API key in
  Settings.

## Quick entry points

- [Install and launch](quick-start/install-and-launch.md) — build the current
  source tree and start Ravo Studio.
- [First launch and import](quick-start/first-launch-and-import.md) — create a
  library and bring in photos or a folder.
- [Five-minute tour](quick-start/five-minute-tour.md) — exercise the shortest
  useful Studio path.
- [Library and review](guides/library-and-review.md) — organize, filter, rate,
  tag, and remove photos safely.
- [Viewer and scopes](guides/viewer-and-scopes.md) — use Gallery, Loupe, zoom,
  pan, and the right-side scopes.
- [Develop](guides/develop.md) — make and recover non-destructive edits.
- [Export and sharing](guides/export-and-share.md) — choose an output format
  and understand conflict behavior.
- [CLI](guides/cli.md) — automate inspection, catalog operations, recipes,
  preview diagnostics, recovery/backup, and export.
- [File paths, backups, and recovery](troubleshooting/file-paths-and-recovery.md)
  — distinguish originals, catalog state, recovery mirrors, previews, and
  verified backups.

## Recommended reading order

1. [Install and launch](quick-start/install-and-launch.md)
2. [First launch and import](quick-start/first-launch-and-import.md)
3. [Five-minute tour](quick-start/five-minute-tour.md)
4. [Library and review](guides/library-and-review.md)
5. [Viewer and scopes](guides/viewer-and-scopes.md)
6. [Develop](guides/develop.md)
7. [Export and sharing](guides/export-and-share.md)
8. [Settings](guides/settings.md)
9. [File paths, backups, and recovery](troubleshooting/file-paths-and-recovery.md)
10. [Troubleshooting import failures](troubleshooting/import-failures.md)

## How this handbook is organized

- **Quick Start** covers source-build launch, library creation, import, and the
  first complete loop.
- **Guides** are organized around user tasks rather than C++ targets.
- **Troubleshooting** explains supported boundaries, missing originals, output
  conflicts, and structured CLI failures.
- **QA Appendix** contains a compact smoke path and format matrix for validating
  a build.

## Scope and safety

Ravo Studio is an independent Qt Quick application with a C++ service and CPU
engine. The old GTK application under `legacy/` is read-only migration evidence
and is not a supported Ravo dependency or runtime. Ravo does not open an old
catalog in place. For historical sidecars, use the strict CLI XMP import path
described in [the CLI guide](guides/cli.md); unsupported history is reported as
unsupported rather than silently approximated.

When an operation, profile, file container, or legacy history cannot be
represented by the current contract, Ravo returns a visible failure. That is a
product boundary, not a request to overwrite the source with a fallback.
