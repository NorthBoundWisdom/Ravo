# Welcome to Ravo

Ravo Studio brings your photo library, RAW editing and video browsing into one
local workspace. Import a shoot, review the frames, refine a look and export
the finished selection. Your catalog and original media stay on storage you
control.

[Download a release](https://github.com/NorthBoundWisdom/Ravo/releases/latest)
or [install and launch](quick-start/install-and-launch.md). This handbook
describes the current development branch; release notes identify the features
in each published build.

<!-- RAVO_DOCS_BUILD_METADATA -->

## Start with a shoot

1. **[Import](quick-start/first-launch-and-import.md)** — Add files in place,
   Copy into an organised folder or explicitly Move after verification.
2. **[Review](guides/library-and-review.md)** — use ratings, colours,
   Pick/Reject, keywords, folders and collections to find your selection.
3. **[Inspect](guides/viewer-and-scopes.md)** — use Loupe, Fit/30%/1:1,
   comparison, the navigator and photographic scopes.
4. **[Develop](guides/develop.md)** — shape exposure, white balance, tone,
   colour and detail; use local masks to refine an area.
5. **[Deliver](guides/export-and-share.md)** — export JPEG, PNG, TIFF or an
   original copy with explicit format, size and privacy choices.

For a guided first session, follow the
[five-minute tour](quick-start/five-minute-tour.md).

## Keep photos and videos together

Supported MOV/MP4/M4V videos appear beside photographs, with poster thumbnails
and duration badges. Open a video in Loupe to play, pause, seek, change volume
or mute. Video export currently copies the original file; photo Develop tools
and video transcoding do not apply.

The [format coverage matrix](qa/format-coverage.md) explains RAW, raster,
HEIC/HEIF and video boundaries. A recognised extension is a candidate, not a
guarantee that every camera mode or codec variant is supported.

## Know what is saved

- **Originals:** Add and Copy preserve source media. Move removes sources only
  after requested copies verify and the primary is cataloged.
- **Edits and organisation:** a local SQLite catalog stores recipes, review
  state, tags and history. Non-destructive editing leaves original content intact.
- **Previews:** rebuildable image caches are separate from the catalog.
- **Recovery and backups:** catalog-owned recovery records and verified backups
  preserve catalog state. They exclude original media and preview caches.

Configure **[Settings → Catalog & Backup](guides/settings.md#catalog-backup-automatic-backups)**
for an automatic schedule, retention and backup status. Schedules run while
Studio is open. Keep a separate backup of your original photos and videos.

Read [File paths, backups, and recovery](troubleshooting/file-paths-and-recovery.md)
before relocating or restoring a library.

## Make the workspace comfortable

[Settings](guides/settings.md) groups language, saved panel dimensions,
current-catalog backups and the optional Assistant connection. Nine interface
languages are available. [Shortcuts and the command palette](guides/shortcuts.md)
help you reach commands quickly and explain unavailable actions.

## Automate a workflow

The [CLI guide](guides/cli.md) covers JSON results, catalog inspection, edits,
video frame artifacts, export, backups and revision-checked local Studio
control. Scripts use the same catalog and image services as the desktop app.

## If something needs attention

- [Import failures](troubleshooting/import-failures.md)
- [Known limitations](troubleshooting/known-limitations.md)
- [Glossary](troubleshooting/glossary.md)
- [User-run smoke test](qa/smoke-test.md)
- [Regression checklist](qa/regression-checklist.md)

Ravo is in active development before 1.0. Hardware, package and real-corpus
qualification remain specific to the source SHA and host. An unsupported
operation reports a reason; a successful test on one platform does not qualify
every other platform.
