# Settings

Open **File → Settings**, press **Cmd/Ctrl+,**, or search for **Settings** in the
command palette. Choose a category on the left; a narrow window uses the
category selector at the top. The content scrolls when it does not fit.
Use **Back** or `Esc` to return to the workspace.

## General: language

Choose the interface language. Studio supports English, German, Spanish,
French, Brazilian Portuguese, Simplified Chinese, Traditional Chinese,
Japanese and Korean. A successful change applies immediately and is remembered
for the next launch.

Language affects interface labels, menus and shortcuts, not image rendering.
Unsupported languages, a missing translation package, or a settings-write
failure produce an error and retain the previous active setting.

## Workspace: panel sizes

Set the left panel width, right panel width and filmstrip height. Changes save
automatically for this user across catalogs. You can also drag the side
dividers and the filmstrip's top edge in the ordinary workspace.

**Reset panel sizes** restores the defaults: left 240 px, right 320 px and
filmstrip 108 px. The controls enforce the supported size bounds. A write
failure remains visible; a small window can constrain the displayed layout
without changing the saved preference.

## Catalog & Backup: automatic backups

These settings belong to the **currently open catalog**. Open a library before
configuring its backup policy.

1. Choose **Enable automatic backups**.
2. Choose an existing **Backup folder**, or enter its local path.
3. Set **Interval (minutes)** and **Backups to keep**.
4. Choose **Save backup settings**.

Intervals range from 15 to 525,600 minutes, and retention from 1 to 100 backups.
A daily schedule is 1,440 minutes. Configuration can also be saved while
disabled. Turning the checkbox off takes effect only after saving.

The page shows the saved enabled state, last verified backup and its size,
next scheduled run, and the last failure. **Run backup now** uses the saved,
enabled policy; save or reload a dirty draft before running. Active work
reports its stage and offers cancellation where supported.

**Reload saved settings** discards the draft. A failed save preserves your
inputs. If another policy update conflicts with the draft, reload the stored
policy before saving again. Switching catalogs resets the form for the new
catalog.

Automatic backups run while Studio is open; this is not an operating-system
background scheduler. Backups contain catalog state and recovery records,
excluding original photos/videos and rebuildable preview caches. Back up
original media separately.

Manual create/verify/restore remains under **File → Recovery**.
Read [File paths, backups, and recovery](../troubleshooting/file-paths-and-recovery.md)
before restoring or moving a library.

## Assistant: connection

The optional floating Assistant panel uses the HTTP endpoint you configure.
Set:

| Setting | Meaning |
| --- | --- |
| URL | OpenAI-compatible API base; default `https://api.x.ai/v1` |
| Model | Provider model identifier; default `grok-4.5` |
| API key | Stored per-user key; an empty value uses `XAI_API_KEY` from the process environment at send time |
| Show | Reveal the key while editing the field |

The connection settings are per user, separate from catalog backups and photo
recipes. Invalid URL/model values are rejected. Keys are not included in
Studio machine snapshots or ordinary logs; local settings storage is not an
OS-keychain guarantee.

Open the panel with **View → Assistant** or the command palette. It has no
default keyboard shortcut. Requests go to the chosen external provider; the
local library/edit/export workflow does not require the Assistant.

## Settings and task options

Import source/destination choices, organisation and preview policy remain in
the import workspace. Export format, size and privacy remain in the export
form. Photo colour/profile choices remain in Edit. Settings does not turn these
task inputs into global photo-rendering defaults.
