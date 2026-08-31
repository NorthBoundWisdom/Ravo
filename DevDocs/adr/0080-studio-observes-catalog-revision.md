# ADR-0080: Studio observes live catalog revision instead of wrapping CLI in MCP

- Status: Accepted
- Date: 2026-08-29
- Extends: [ADR-0003](0003-versioned-machine-contract.md),
  [ADR-0007](0007-first-usable-catalog-viewer.md),
  [ADR-0011](0011-atomic-develop-publication.md)
- Relates to: [ADR-0079](0079-develop-set-inventory-and-probe-png.md)

## Context

CLI and Studio are both supported clients of one CatalogService. An agent
already speaks `ravo-cli/v1`. A second machine protocol (MCP around `ravo`)
would duplicate that contract. The remaining gap was that Studio kept an
in-memory recipe/listing after another process committed a catalog revision.

`CatalogSnapshot.revision` already exists and mutators bump it in the same
transaction as recipe/review/import changes. The SQLite adapter had been
returning a cached revision, so a second connection's bump was invisible.

## Decision

- `SqliteCatalogRepository::snapshot()` re-reads `schema_info.revision` on
  every call. CatalogService snapshot is the ObserveCatalog read model.
- Studio's presenter owns a 1 s UI-thread timer. Catalog I/O stays on the
  serial executor. QML does not poll, read SQLite, or own revision policy.
- When live revision differs from the last applied revision, Studio reloads
  the visible listing and, for a still-selected asset, the catalog recipe and
  history. An unchanged recipe keeps the session undo stack. A different
  recipe replaces Develop memory, clears undo/redo, and requests a preview.
- Polls skip while create/open/import is busy, a Develop save/preview is
  in flight, or another poll is running. Snapshot/list/recipe failures are
  explicit errors; the previous applied revision is left unchanged so the
  next tick retries.
- MCP around `ravo` remains later optional packaging. It is not the Studio
  control plane.

## Consequences

An open Studio window converges on CLI catalog writes within one poll
interval without a second protocol. Agents keep using `ravo --json`. Two
writers can still race; the later committed revision wins. Uncommitted
Studio overlay is not a catalog fact and is discarded when an external
recipe revision is applied.

## Rejected alternatives

- An MCP server that drives the live Qt process. That adds a second
  versioned contract, a long-lived extra process, and UI-thread puppetry
  while leaving CLI/Studio catalog divergence unsolved.
- Filesystem watches on the SQLite/WAL path. WAL files appear and vanish,
  and Studio's own writes would still need revision comparison.
- Push notifications from SQLite. Update hooks do not fire for other
  connections.
