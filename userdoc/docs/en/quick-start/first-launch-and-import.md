# First Launch and Import

## Goal

Create or open a local library, import photos by reference, and confirm that
the first previews appear in Gallery.

**Last verified:** 2026-08-27 against the current Studio and catalog service
contracts.

## Applies to

- Ravo Studio on macOS, Windows, and Linux source builds.
- The `ravo catalog` CLI path described in [the CLI guide](../guides/cli.md).

## Prerequisites

- Ravo Studio is running.
- You have a writable location for a `.sqlite` catalog.
- You have at least one valid image or a folder containing supported media.
- The original files are readable and will remain at their current paths.

## Create a library in Studio

1. Choose **File → Create Library**.
2. Select a writable filename ending in `.sqlite`.
3. Confirm the dialog and wait for the status bar to report that the library
   was created.
4. Choose **Import…** in the Library panel, or use **File → Import Photos**.

The default first-launch location is the system Pictures directory, with the
suggested filename `Ravo Library.sqlite`. A new catalog starts empty.

## Import individual files

1. Choose **File → Import Photos**.
2. Select one or more local files.
3. Confirm the selection and wait for the Import and Previews meters to finish.
4. Select a thumbnail to open its preview, or double-click it to enter Loupe.

The file dialog lists common raster and RAW extensions. **All files** can be
used when a valid supported file has an uncommon suffix.

## Import a folder

1. Choose **File → Import Folder**, or choose **Import…** in the left Library
   panel and select a directory.
2. Wait while Ravo recursively scans the directory.
3. Review the completion summary: imported, duplicate, unsupported, and failed
   items are reported separately.

Directory import is recursive, ignores hidden filenames, sorts paths
deterministically, and removes duplicate paths from one batch. It does not
copy the files into the catalog.

## Current input boundary

Ravo considers these raster extensions as import candidates:

| Family | Extensions | Notes |
| --- | --- | --- |
| JPEG | `.jpg`, `.jpeg` | RGB JPEG input; embedded ICC is retained when supported. |
| PNG | `.png` | RGB PNG input with supported color metadata. |
| TIFF | `.tif`, `.tiff` | Supported baseline TIFF layouts only; multipage, tiled, floating-point, and other unsupported layouts fail explicitly. |
| Other raster | `.bmp`, `.gif`, `.webp` | Decode depends on the matching Qt runtime plugin and valid file content. |
| RAW | Common LibRaw files such as `.arw`, `.cr2`, `.cr3`, `.nef`, `.dng`, `.raf`, `.orf`, and `.rw2` | The actual sensor/container must be supported by the pinned LibRaw path. |

The scanner also recognizes additional RAW suffixes handled by the current
source. A suffix alone does not guarantee a successful import: malformed data,
unsupported compression, unsupported TIFF/PNG layout, a missing runtime plugin,
or an unsupported RAW sensor returns a structured unsupported or failed result.

## What import changes

- The catalog stores the normalized original path, media type, dimensions when
  available, capture metadata when readable, and a content fingerprint.
- Review state starts at rating `0`, no color label, and Keep/not rejected.
- A baseline recipe is synthesized for the asset. RAW assets use the default
  Sigmoid Standard SDR display path; a default raster asset has no visible edit.
- A rebuildable preview is written outside the SQLite file. RAW Gallery
  thumbnails may use a readable embedded JPEG; Loupe, Develop, scopes, and
  export use the processed CPU path.
- The original file is not moved, copied, renamed, or rewritten.

## Progress and results

During a batch, the left panel can show separate Import and Previews progress.
The final Studio status has the form:

```text
Imported N, duplicate N, unsupported N, failed N
```

An unsupported or failed item does not silently become a placeholder asset.
Keep the exact error text when troubleshooting.

## Result

- The imported photos appear in Gallery and in their source folders in the
  Library tree.
- Selecting a photo loads its thumbnail and, when needed, a larger preview.
- Restarting Studio and reopening the same `.sqlite` file restores the catalog
  records; missing previews can be rebuilt from still-readable originals.

## Common questions

### Why did a second import report `duplicate`?

Ravo identifies an already imported source and does not create a second catalog
record for it. This is expected protection against duplicate entries.

### What happens if I move the original after importing?

The catalog keeps the original path; it does not track a moved file. The asset
remains in the library with its review and recipe state, but previews and export
cannot regenerate until the original is available at the recorded path. See
[File paths and recovery](../troubleshooting/file-paths-and-recovery.md).

### Can I import a whole folder without copying it?

Yes. Folder import is reference-only. Make sure the folder remains readable at
its current location before using the library on another machine.

### Does Ravo migrate a darktable catalog?

No. The Ravo SQLite schema is independent. Import supported source files into a
new Ravo library; strict legacy XMP conversion is a separate CLI operation and
accepts only evidenced, representable history.
