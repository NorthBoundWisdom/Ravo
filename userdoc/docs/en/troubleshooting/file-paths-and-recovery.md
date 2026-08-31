# File Paths, Backups, and Recovery

## Goal

Recover from missing originals, unavailable destinations, preview-cache issues,
pending recovery mirrors, or catalog-open problems, and create a verified
catalog backup without confusing it with an original-media backup.

**Last reviewed:** 2026-08-31 against the current catalog schema-v6 recovery,
backup, cache, and atomic-publication contracts.

## Applies to

- Ravo Studio's local catalog and preview cache.
- Local `ravo` CLI workflows.
- Catalog-owned recovery mirrors and CLI catalog backup/verification.

## Original files are references

Import records a normalized local path and a source fingerprint. It does not
copy the source into the library. Normal editing, preview generation, metadata
changes, and rendered export do not rewrite it.

This makes the source folder part of the library's operational setup. Keep the
source readable at the recorded path, or keep a filesystem layout that resolves
the same path when moving a catalog between machines.

## Missing original

Typical signs are:

- A thumbnail or image surface says **Missing**.
- The right Photo panel still shows catalog review state but no new preview can
  be generated.
- Export returns an original-not-found error.

The catalog record, rating, color label, reject state, tags, metadata, recipe,
and history remain stored. Restore the file to its recorded path, then select
the asset again so Studio can request a new preview.

There is no automatic relink-by-filename workflow in the current baseline. Do
not delete the catalog record merely because the source is temporarily
unmounted.

## Preview cache

The default cache is adjacent to the catalog:

```text
<catalog>.preview/
```

It contains rebuildable PNG previews and is not the source of truth for recipes
or review state. If a cache entry is missing, Ravo can regenerate it from a
readable original. If the source is missing, cache regeneration cannot succeed.

Do not treat a cached preview as an archival copy of the original RAW or raster
file. Verified catalog backup deliberately excludes this cache.

## Catalog-owned recovery mirrors

Durable catalog state has a separate support root:

```text
<catalog>.ravo/sidecars/
```

Each asset's current generation is a bounded, checksummed `.ravo.json` snapshot
of source identity, review/capture state, tags, writable metadata, recipe, and
history. These files are derived from SQLite after a successful catalog commit.
They are not placed beside originals, are never imported automatically, and do
not become a second live edit authority.

Studio and the CLI retry pending generations when a catalog opens or closes.
Inspect or explicitly synchronize them with:

```text
ravo catalog sidecar-status --catalog "/work/Ravo Library.sqlite" --json
ravo catalog sidecar-status --catalog "/work/Ravo Library.sqlite" \
  --asset-id <asset-id> --json
ravo catalog sidecar-sync --catalog "/work/Ravo Library.sqlite" --json
```

Without `--asset-id`, status lists pending generations only. A mutation can
commit to SQLite while filesystem publication fails; the error then reports
`catalog_committed=true` and `recovery_pending=true`. Do not repeat the
catalog edit blindly. Repair the support-directory problem and run
`sidecar-sync` or reopen the catalog.

Do not hand-edit, rename, or copy one recovery JSON back into the live catalog.
Restore from these artifacts is not an accepted command yet.

## Create and verify a catalog backup

For a healthy catalog, prefer the supported backup command over copying a live
SQLite filename:

```text
ravo catalog backup --catalog "/work/Ravo Library.sqlite" \
  --backup "/backups/Ravo-2026-08-31" --json
ravo catalog backup-verify --backup "/backups/Ravo-2026-08-31" --json
```

The backup destination must not exist. Creation drains pending recovery,
integrity-checks and snapshots the live database, removes rebuildable preview
rows, copies the exact recovery generations, verifies the staged result, and
publishes the directory without replacement. The result has exactly:

```text
Ravo-2026-08-31/
├── catalog.sqlite
├── manifest.json
└── sidecars/
```

`backup-verify` is self-contained and takes no `--catalog`; it verifies the
strict layout, hashes, identities, sidecar generations, SQLite integrity, and
the absence of preview rows without opening the snapshot as a live library.

!!! warning

    A verified catalog backup excludes originals and previews. Back up every
    referenced original separately. Ravo currently has no restore command,
    scheduled retention, cloud destination, or Studio backup UI; verification
    proves artifact integrity, not restorability through a supported workflow.

## Catalog open errors

- **Catalog database does not exist**: use Open Library on the correct path, or
  create a new library.
- **Catalog database already exists**: Create Library will not overwrite it;
  choose another path or open the existing file.
- **Catalog is missing schema information / invalid**: the file is not a valid
  Ravo catalog or is damaged. Preserve a copy before attempting recovery.
- **Catalog schema is newer than this Ravo**: use a Ravo build that understands
  that schema; the current build does not downgrade it.

Ravo's catalog schema is independent from the old application. Do not point
Ravo at a legacy database and expect an in-place migration.

## Output path problems

Rendered export and original-copy export use atomic no-replace publication:

- An existing destination returns `conflict`.
- A missing or unwritable parent directory returns an I/O failure.
- A cancelled or failed write does not become a successful partial export.

Choose a new output path and verify that its parent directory is writable. If a
previous output is important, preserve it before retrying.

## Safe recovery sequence

1. Stop the current import or export if it is still running.
2. If the catalog still opens, inspect pending recovery and create then verify a
   new backup at an absent destination.
3. Verify that every required original path exists and is readable; remember
   that the catalog backup does not contain those files.
4. Reopen the catalog and select the affected asset.
5. Let Ravo rebuild the preview, then verify the image, review state, recipe,
   and history.
6. Export to a new destination and check the result independently.

If the catalog cannot open, close every Ravo process before making a forensic
filesystem copy. Preserve the catalog, its `.ravo` support directory, and any
same-name SQLite `-wal` / `-shm` files that still exist. That raw copy is not
a verified Ravo backup, but it avoids destroying evidence before diagnosis.

## Result

Recovery distinguishes catalog records, recovery mirrors, verified backup
artifacts, source files, cache files, and outputs. Work on the affected layer
without deleting another one or mistaking verification for restore.

## Common questions

### Can I delete the `.preview` directory to fix a stale preview?

Previews are rebuildable, but remove or replace cache data only when Studio is
closed. First create and verify a catalog backup when the library is healthy,
and preserve the originals separately.

### Will reopening the catalog relink moved photos?

No. Reopening reloads the stored paths. Restore the source path or use an
external filesystem arrangement that makes the recorded path valid again.

### Does Remove from Catalog delete the original?

No. **Remove from Catalog** leaves the original on disk. **Delete from Disk** is
the separate, confirmed, irreversible action.

### Can I open `catalog.sqlite` inside a backup directly?

No supported workflow does that. `backup-verify` treats the directory as a
read-only artifact. Catalog restore and publication to a new live destination
remain unfinished.
