# ADR-0062: Asset removal is revision-atomic and disk deletion is quarantined

- Status: Accepted
- Date: 2026-08-29
- Extends: [ADR-0047](0047-first-frame-raw-cache-lifecycle.md)

## Context

Catalog import already classified duplicate, unsupported, missing, corrupt, and
cancelled inputs without publishing partial assets. Catalog-only removal kept
the original, and disk removal deleted it, but asset-row deletion and revision
bump were separate SQL statements. Disk removal also unlinked the original
before the database mutation, so a later database failure was unrecoverable.

## Decision

- Repository `remove_asset` deletes the asset cascade and increments catalog
  revision in one SQLite transaction. Any delete/revision/read/commit failure
  rolls back both asset visibility and revision.
- CatalogService removes rebuildable preview cache before that transaction. A
  later database failure may require cache regeneration but cannot lose asset
  metadata or the original.
- “Remove from catalog” never mutates the source. Unknown IDs fail
  `not_found`; success removes every cache variant and advances one revision.
- “Delete from disk” accepts only an existing regular file. It atomically
  renames the source to a unique adjacent quarantine, runs catalog removal, and
  renames it back if the database transaction fails. After catalog success it
  unlinks the quarantine. A final unlink failure reports
  `catalog_removed=true` plus the recoverable quarantine path rather than
  claiming full success.
- Duplicate import returns the existing asset without advancing revision.
  Missing/corrupt/unsupported/cancelled imports publish no asset. Missing
  originals remain explicit asset state when a trusted preview exists.
- Studio confirmation and multi-selection orchestration remain presentation;
  each service mutation preserves these single-asset invariants and selection
  converges on the next visible asset only after success.

## Consequences

J5's Ravo service contract is accepted. The old `control/jobs/image_jobs.*`
wrapper remains because `common/mipmap_cache.c` still calls its speculative
load job; deletion waits for S11/J4 zero-consumer work. Old batch move/copy,
grouping, local-copy, and duplicate-creation behavior is not silently included
in this removal contract.

## Rejected alternatives

- Unlinking first and reporting the later database error. It loses user data.
- Deleting the catalog row first. A filesystem denial would lose tracking while
  leaving an unmanaged original.
- Hiding cache-removal or quarantine-finalization failures. Derived cache may
  be rebuilt, but the operation must still report its actual terminal state.
