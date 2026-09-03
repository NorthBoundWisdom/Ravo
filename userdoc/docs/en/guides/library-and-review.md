# Library and Review

## Goal

Use the local library to organize referenced photos, review them quickly, and
keep catalog state separate from original files.

**Last reviewed:** 2026-09-03 against the current catalog schema and Studio
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

The cache can be rebuilt from readable originals. Durable catalog changes have
a separate catalog-owned recovery mirror under:

```text
/work/Ravo Library.sqlite.ravo/sidecars/
```

That support directory contains versioned, checksummed recovery JSON; it is not
an adjacent photo sidecar or a second edit authority. The catalog does not
become a copy of the source folder.

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
- **Collections**, which persist named manual membership sets and smart
  saved-filter sets in the catalog.
- A collapsible tree of source folders represented by imported assets.
- A count beside each folder.
- A tag filter field.
- **Import…** and **Export…** entry points.

Click a folder to limit the center view. Click its disclosure marker to collapse
or expand child folders. Right-click a folder for **Import Photos from This
Folder**, **Show in Finder/Explorer**, **Update Folder Location** when the
folder is missing, **Expand/Collapse**, and **Remove from Catalog**. Removing a
folder removes its photos from the library and does not delete original files.
**All Photographs** cannot be removed. Folder selection works together with
rating, color, reject, and tag filters.

Create a **Collection** from the current selection, or a **Smart** collection
from the current filters. Selecting a collection is session state and is
discarded when the catalog closes; the set itself survives reopen. Deleting a
collection does not delete photos. The CLI exposes the same sets through
`ravo catalog sets` and related commands.

Create a **virtual copy** of the selected photo to keep a second grade of the
same original. The copy is another catalog asset with its own recipe and
history; it does not duplicate the file. A same-stem RAW+JPEG pair imports as
one RAW photo, not two rows. Stack two or more selected photos to group a
burst; collapsed Library view shows the pick.
**Survey** shows two or four selected photos as exact previews for culling.
Deleting a virtual copy removes only that catalog row. Deleting the original
from disk is allowed only on the primary asset and removes every version of
that file from the catalog.

## Filter and sort the library

Enable **Filter** in the top bar. The default control is a star strip:

- The empty star keeps only unrated photos.
- Stars 1–5 keep photos with that exact rating.
- Click the active value again to return to Any.

Use **+** to add Search, Type, Edits, Color, or Rejected. Each added control
has a close button that removes it and clears that predicate. Unchecking
**Filter** hides the extras and clears active filters.

Sort stays on the right of the bar even when Filter is off.

| Control | Available values |
| --- | --- |
| Rating | Unrated (exact 0) or exact 1–5. |
| Search | Case-insensitive filename, URI, media type, tags, catalog metadata, and camera text. |
| Type | Any, RAW, JPEG, PNG, or TIFF. |
| Edit state | Any, Edited, or Unedited. |
| Color | Red, yellow, green, blue, and purple; more than one color can be selected. |
| Rejected | Include, Exclude, or Only. |
| Sort | Import time, capture time, filename, rating, or file size. Photos without capture time sort after dated photos. |
| Direction | Ascending or descending. |
| Tag | A tag name entered in the left-panel field. |

If no photo matches, Studio says so instead of showing stale thumbnails.

Filter state belongs to the current open session and is not written as a
recent-search history. Closing the catalog discards it. Service integrations
may additionally use validated camera, ISO, aperture, focal-length, shutter,
aspect-ratio, import-time, and capture-time ranges; Studio's search field
already matches camera make/model.

## Select photos

- Click a thumbnail to make it the active photo and clear the other selection.
- Press **Cmd+A** on macOS or **Ctrl+A** on Windows/Linux to select every photo
  currently loaded in Gallery and the filmstrip. The active photo stays the
  same. The shortcut is available in Gallery and while the filmstrip is visible
  in Loupe or Edit, and it yields to text fields.
- Hold **Cmd** on macOS or **Ctrl** on Windows/Linux while clicking to add or
  remove a photo from the selection.
- Hold **Shift** while clicking to select the range between the selection anchor
  and the clicked photo.
- Use **Previous** and **Next**, or the Left/Right arrow commands, to move the
  active photo through the current visible order.

The active photo is the one shown in Loupe, Edit, and the Inspector.
Review actions such as rating, color label, reject state, tags, and catalog
metadata can apply to the current selection. Develop operates on the active
photo; export uses one active photo or the current multi-selection.

## Rate, label, and reject photos

Use the bottom review bar, the **Photo** menu, or the photo context menu.
If the window is too narrow, Survey, Virtual Copy, Stack, Size, and
Previous/Next collapse into **More**. Rating and color labels shrink to a
compact star count and a single swatch that open a small picker. Keep/Reject
stays on the bar.

- Set a rating from `0` to `5` stars.
- Apply **No Color**, **Red**, **Yellow**, **Green**, **Blue**, or **Purple**.
- Set **Reject** or **Keep**. Rejected photos keep the red Reject flag and
  show a greyed, dimmed thumbnail in Gallery and the filmstrip.
- Choose **Copy Info** to copy a stable identity block for the selected photo
  to the system clipboard. The block names the open library, asset ID, file
  URI/path, fingerprint, and basic file metadata so a later debug session can
  target that exact photo.
- Choose **Copy Parameters** to copy a versioned English block containing the
  selected photo identity, whether the current recipe is saved or pending, and
  the complete canonical recipe JSON. It includes in-memory adjustments that
  have not yet been saved; QML does not translate or reformat the parameter
  names and values.

The photo context-menu **Copy Parameters** command above writes diagnostic text
to the system clipboard. The Edit History rail's command with the same visible
name opens a field chooser and stores selected Develop values in Studio's
session clipboard; it does not expose recipe JSON.

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

Use **Photo → Refresh Capture Metadata** when the original's embedded capture
tags changed after import. Refresh replaces the Catalog capture snapshot and
file identity atomically; malformed or unreadable metadata leaves the prior
Catalog values in place. It never writes the original or an adjacent
interchange sidecar; the committed catalog change advances its recovery
generation normally.

Writable metadata is catalog metadata. It is not written back into the source
file during review or editing. Rendered JPEG/PNG/TIFF write the bounded
Catalog-owned public metadata snapshot, including validated capture
time/offset/GPS; TIFF also maps current writable values into its baseline
directory metadata. Export can keep full metadata, remove location, or omit
public metadata; explicit capture refresh updates the catalog snapshot.
Arbitrary source-packet copying remains outside the contract. Adjacent-sidecar
behavior is intentional: Studio never automatically reads or writes XMP beside
the original; legacy XMP conversion is explicit and rendered XMP is embedded
only in the new destination. Catalog-owned recovery JSON is a separate
durability artifact and is never imported as interchange metadata.

## Recovery state and catalog backups

Normal durable edits advance an asset-local recovery generation. Ravo retries
an unpublished generation when the catalog opens or closes, and the CLI can
inspect or synchronize it explicitly. The CLI can also create and independently
verify an immutable catalog backup that includes the database snapshot and
recovery mirrors but excludes originals and rebuildable previews.

Studio exposes recovery status/sync, backup create/verify/restore, scheduled
retention, and selected/all preview rebuild under **File → Recovery**. Use the
same CLI commands and safety boundaries in
[File paths, backups, and recovery](../troubleshooting/file-paths-and-recovery.md).

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

Yes, but the originals still need to be available. Ravo stores references
rather than portable source copies, so moving the catalog alone does not move
the photos. If a stable direct source folder is missing and an unchanged copy
is available elsewhere, use its missing row to choose the explicit replacement;
Ravo validates every file identity before relinking.

### What happens to previews when I remove a catalog record?

The record and associated cached preview are removed. The original is untouched
and can be imported again later.
