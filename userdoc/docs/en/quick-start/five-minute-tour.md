# Five-Minute Tour

## Goal

Complete one short path through library management, review, viewing, Develop,
and export.

**Last reviewed:** 2026-08-31 against the current Studio command registry and
QML workspace.

## Applies to

- Ravo Studio on any host with a working source build.
- Users and testers who want a quick confidence check before a longer session.

## Prerequisites

- A library is open.
- At least one photo has been imported and its original is readable.
- A second photo is available when you want to exercise parameter copy/paste.

## Five-minute route

1. In **Library**, select a folder or **All Photographs**.
2. Click one thumbnail. Confirm that the left panel shows the folder tree, the
   center shows the Gallery, and the right panel shows photo details and scopes.
3. Set a rating or color label in the bottom review bar. Toggle **Keep / Reject**
   once, then return the photo to the state you want to keep.
4. Double-click the thumbnail, or choose **View → Loupe**. Try **Fit**, **Fill**,
   and **1:1**. Click the photo to animate to 1:1, then click again to restore
   the previous zoom. At a non-grid zoom, pan the image and use the navigator in the
   left panel to move to another area.
5. Switch the right-side scope through **Histogram**, **Waveform**, **Parade**,
   **Vectorscope**, and **Split**.
6. Switch the top view control to **Edit**. Change one light or color control,
   wait for the preview, then press **Before / After**. Press **Y|Y** to compare
   the immutable baseline and live edit side by side with shared zoom and pan.
7. Choose **Copy Parameters**, explicitly select that changed field, move to a
   second photo, and use **Paste Parameters**. Unselected target edits remain
   unchanged.
8. Press **Undo**, then **Redo**. Use the individual reset button or **Reset
   all** if you do not want to keep the change.
9. Return to **Gallery**, select the edited photo, and choose **File → Export
   Selected**. Pick PNG, JPEG, TIFF, or Original copy and choose a new path.
10. Close and reopen Studio with the same library to confirm that review state
   and the edit survive a restart.

## What each area is for

| Area | Use it for |
| --- | --- |
| Menu bar | Library, view, photo, edit, settings, and command-palette actions. |
| Filter bar | Optional Filter checkbox, default rating stars, add/remove extra filters, and stable sort. |
| Library panel | Folders, tag filtering, import, export, and a small navigator. |
| Center stage | Gallery grid, Loupe preview, or Edit image/crop surface. |
| Review bar | Thumbnail size, rating, color label, Keep/Reject, Previous, and Next. |
| Inspector | Photo information, catalog metadata, presets, history, Develop controls, and scopes. |
| Filmstrip | Whole-photo thumbnails with sequence number, rating, flags, and edit state. |
| Status bar | Import, preview, loading, success, and error feedback. |

## Result

You have confirmed the main local workflow: a catalog opens, a source can be
reviewed, the image can be inspected, an edit is non-destructive, and an output
can be created without overwriting an existing destination.

## Common questions

### Why is the right panel empty?

A photo must be selected. Select a thumbnail or use Previous/Next after opening
the library.

### Why does Gallery look different from Edit for a RAW file?

Gallery may display an embedded camera JPEG for a fast thumbnail. Loupe,
Develop, scopes, and export use Ravo's processed RAW pipeline so that editing
and output share one CPU path.

### Why did export stop instead of replacing my file?

Ravo uses no-replace publication. Choose a new destination or explicitly move
the old file yourself before exporting again.
