# ADR-0067: Preview cache has a hard byte budget and persistent LRU

- Status: Accepted
- Date: 2026-08-29
- Extends: [ADR-0047](0047-first-frame-raw-cache-lifecycle.md)

## Context

Ravo's preview PNG cache was atomic and rebuildable but could grow without a
bound. A catalog row is only a hint to a cache resource, so capacity policy
must live with the filesystem adapter rather than SQLite or QML. The policy
also has to survive catalog close/reopen without introducing a cache database.

## Decision

- Each `FilesystemPreviewCache` has an explicit byte budget; CLI and Studio use
  the 512 MiB product default. Zero-byte budgets and a single PNG larger than
  the budget reject before publication.
- The adapter indexes only safe regular `<cache-key>.png` entries. File size is
  the accounted unit. The file modification time records cross-session access;
  an in-process monotonic sequence orders hits and commits, with the key as the
  deterministic tie breaker during startup.
- A valid hit refreshes its access time. Before a new atomic commit, the
  adapter removes least-recently-used entries until the committed set plus the
  incoming PNG fits. All map, accounting, access, commit, and removal actions
  are serialized per cache instance.
- A bad PNG signature is deleted as a rebuildable miss. Directory, timestamp,
  measurement, eviction, and publication failures are structured I/O errors;
  they are not hidden as misses. Unrecognized files and symlinks are not owned
  or removed.
- Preview database rows may outlive an evicted file. A later request treats the
  absent path as a miss and rebuilds it from the read-only original. Removing
  an asset removes all of its indexed variants and accounting.
- Catalog checks cancellation again after encode and immediately before cache
  commit. Cancellation at that seam publishes neither a file nor a ready
  preview record. Closing a catalog destroys decoded buffers and the in-memory
  index; bounded disk entries remain for reopen.

## Consequences

S11's Ravo cache contract is accepted. The old `common/cache*`,
`image_cache*`, `mipmap_cache*`, and `imagebuf*` files remain because many old
develop, image I/O, job, and GTK consumers still exist. In particular,
`mipmap_cache.c` still blocks J5 `image_jobs` retirement; acceptance of this
replacement is not a zero-consumer claim.

## Rejected alternatives

- Entry-count limits. Compressed previews vary too much in size.
- A second SQLite cache index. Modification time plus deterministic in-memory
  ordering is sufficient for rebuildable files and avoids transaction coupling.
- A user-facing legacy cache preference. Capacity is an owned product default,
  not a migrated global configuration key.
- Silent use of an unreadable or unmeasurable entry. That would violate the
  hard budget and obscure filesystem faults.
