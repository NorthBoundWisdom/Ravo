# Library and Review

## Goal

Use the local library to organize referenced photos, review them quickly, and
keep catalog state separate from original files.

**Last verified:** 2026-08-27 against the current catalog schema and Studio
review workflow.

## Applies to

- Ravo Studio on macOS, Windows, and Linux source builds.
- One or more photos already imported into an open library.

## Prerequisites

- A Ravo library is open.
- At least one source file has been imported when you want to review a photo.

## The library model

In Studio, “library” means a Ravo SQLite catalog. It contains photo records,
review state, tags, writable catalog metadata, recipes, and recipe history. The
original file remains at the path recorded during import.

Preview images are cache files outside the database. For a catalog at:

```text
/work/Ravo Library.sqlite
```

the default preview root is:

```text
/work/Ravo Library.sqlite.preview/
```

The cache can be rebuilt from readable originals. The catalog does not become a
copy of the source folder.

## Open or create a library

1. Choose **File → Open Library** to open an existing `.sqlite` file, or choose
   **File → Create Library** to create one.
2. Wait for the status bar to report that the library is open.
3. If the library is empty, use **Import…** in the left panel or **File →
   Import Photos / Import Folder**.

Older Ravo catalog schema versions can be upgraded to the current catalog
schema during open. A catalog created by the former application is not a Ravo
catalog and is not migrated in place.

## Browse folders

The left **Library** panel contains:

- **All Photographs**, which shows every asset in the catalog.
- A collapsible tree of source folders represented by imported assets.
- A count beside each folder.
- A tag filter field.
- **Import…** and **Export…** entry points.

Click a folder to limit the center view. Click its disclosure marker to collapse
or expand child folders. Folder selection works together with rating, color,
reject, and tag filters.

## Filter and sort the library

Enable **Filter** in the top bar to expose the review controls.

| Control | Available values |
| --- | --- |
| Rating | Any, minimum 1–5, or exact 0–5. |
| Color | Red, yellow, green, blue, and purple; more than one color can be selected. |
| Rejected | Include, Exclude, or Only. |
| Sort | Import time, filename, or rating. |
| Direction | Ascending or descending. |
| Tag | A tag name entered in the left-panel field. |

Use **Clear filters** to return to the full current folder/library view. If no
photo matches, Studio says so instead of showing stale thumbnails.

## Select photos

- Click a thumbnail to make it the active photo and clear the other selection.
- Hold **Cmd** on macOS or **Ctrl** on Windows/Linux while clicking to add or
  remove a photo from the selection.
- Hold **Shift** while clicking to select the range between the selection anchor
  and the clicked photo.
- Use **Previous** and **Next**, or the Left/Right arrow commands, to move the
  active photo through the current visible order.

The active photo is the one shown in Loupe, Edit, the Inspector, and export.
Review actions such as rating, color label, reject state, tags, and catalog
metadata can apply to the current selection. Develop and export operate on the
active photo.

## Rate, label, and reject photos

Use the bottom review bar, the **Photo** menu, or the photo context menu:

- Set a rating from `0` to `5` stars.
- Apply **No Color**, **Red**, **Yellow**, **Green**, **Blue**, or **Purple**.
- Set **Reject** or **Keep**.

These states are catalog values. They do not modify the original image. Rating,
label, and reject state are also visible in the thumbnail and filmstrip when
there is enough room.

## Add tags and catalog metadata

In the right **Photo** panel:

1. Enter tags in the `tags, comma separated` field. Commas and semicolons are
   accepted as separators; duplicate tags are removed.
2. Enter a **Title**, **Creator**, or **Copyright** value.
3. Finish editing the field so Studio commits it to the catalog.

Capture information such as camera, ISO, aperture, focal length, and shutter
information is displayed when available and is read-only in the current Studio
panel. The current CLI can also store a catalog **Description**; see [the CLI
guide](cli.md).

Writable metadata is catalog metadata. It is not written back into the source
file during review or editing. TIFF export can map the current title-related
catalog values into its bounded baseline directory metadata; complete metadata
packet and sidecar policy is not a current export contract.

## Remove a photo

Select one or more photos and open the **Photo** menu or context menu.

### Remove from Catalog

**Remove from Catalog…** deletes the selected catalog records and their preview
cache records. It does not delete the original files on disk.

### Delete from Disk

**Delete from Disk…** first shows a confirmation dialog, then permanently
deletes the selected original files and removes their catalog records. This is
irreversible. The command is unavailable if a selected original is already
missing.

If you only want the library to stop tracking a file, use **Remove from
Catalog…**.

## Result

- Review state and catalog metadata survive closing and reopening the library.
- Source files stay in place during normal import, review, and editing.
- Removing a catalog record is separate from deleting an original file.

## Common questions

### Why are some review controls disabled?

The library must be open, and a photo must be selected. Export additionally
requires a selected active photo and a destination path.

### Why does the folder tree show only source folders that contain assets?

The tree is derived from imported asset paths. Empty folders are not catalog
records and are not shown.

### Can I use one catalog on another computer?

Yes, but the original paths must resolve on the other computer. Ravo stores
references rather than portable source copies, so moving the catalog alone does
not move or relink the photos.

### What happens to previews when I remove a catalog record?

The record and associated cached preview are removed. The original is untouched
and can be imported again later.
