# ADR-0099: Atomic catalog restore and operator recovery

- Status: Accepted
- Date: 2026-08-31

## Context

ADR-0097 defined catalog-owned recovery generations and verified backups, but
did not define restore, operator-visible recovery, or a cancellable database
snapshot. The former `VACUUM INTO` snapshot could not provide bounded
cancellation during Studio shutdown. A restore also cannot treat a backup as a
writable catalog or overwrite an existing destination.

## Decision

`CatalogRestoreRequest` and `CatalogRestoreResult` are the shared service and
CLI contract. Restore strictly verifies the manifest, database, every recovery
artifact, hashes, catalog identity, and exclusions before staging anything at
the caller-selected absent destination. One operation-owned temporary root
holds the database and support tree. The support tree publishes first and the
catalog file publishes last as the visibility point. Pre-commit failure or
cancellation removes only exact operation-owned staging. After the catalog
file is visible, every error reports `published=true` and no restore path is
deleted implicitly. The restored database must reopen through the ordinary
SQLite repository path; previews remain excluded and explicitly rebuildable.

Backup snapshotting uses a WAL checkpoint, a bounded data-version retry, a
short `BEGIN IMMEDIATE` writer lock, and cancellable chunked file copy. The
copy is switched to the self-contained DELETE journal mode before verification.
There is no `VACUUM INTO` fallback.

The CLI exposes `catalog backup-restore` and `catalog preview-rebuild`.
Studio projects recovery status/sync, create/verify/restore, selected/all
preview rebuild, progress, and cancellation through the C++ command registry.
QML presents dialogs and state only.

## Consequences

- Source backups, originals, adjacent sidecars, and existing destinations are
  immutable throughout restore.
- A destination race fails closed; no replacement or merge policy exists.
- Restore does not copy preview artifacts, so successful restore explicitly
  reports that preview rebuild is required.
- Database-copy cancellation is bounded by the copy chunk; writer blocking is
  bounded by the same operation and releases on every error path.
