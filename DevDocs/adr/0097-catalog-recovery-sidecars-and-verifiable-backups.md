# ADR-0097: Catalog recovery sidecars and verifiable backups

- Status: Accepted; snapshot and restore lifecycle extended by
  [ADR-0099](0099-atomic-catalog-restore-and-operator-recovery.md)
- Date: 2026-08-31
- Extends: [ADR-0011](0011-atomic-develop-publication.md) and
  [ADR-0067](0067-bounded-preview-cache-lru.md)
- Partially supersedes:
  [ADR-0063](0063-explicit-no-automatic-sidecar-policy.md)

## Context

The SQLite catalog is the only live edit authority, but a catalog file by itself
is an unnecessarily large recovery failure domain for a professional photo
library. A per-photo recovery representation makes committed recipes, history,
review state, tags, and writable metadata independently inspectable and
backupable. It must not recreate the old adjacent-XMP behavior that modified
imported directories, introduced two live owners, and placed filesystem I/O in
every interactive Develop commit.

Previews have the opposite durability profile: they are bounded derived caches
that can be regenerated from originals and catalog state. Copying them into a
catalog backup increases time and space without improving recovery. Originals
are user-owned referenced media and also do not belong in a catalog backup.

Static review of the current darktable sidecar queue and the ART/RawTherapee,
Filmulator, and vkdt cache/database boundaries under `Downloads/` reinforced
the separation between durable edit state and rebuildable image caches. Adobe
Lightroom Classic and Capture One documentation likewise separates catalogs,
adjustments, previews/cache, and original media, and shows why sidecar work must
not block each interactive adjustment.

## Decision

- SQLite remains the sole live commit authority. Schema v6 adds
  `asset_recovery_state`, whose asset-local monotonically increasing generation
  advances in the same transaction as every durable asset, recipe, history,
  tag, or metadata mutation. Preview/cache writes do not advance it.
- The derived recovery store is catalog-owned at
  `<catalog>.ravo/sidecars/`; it never writes beside an original. A file is
  named `<asset-id>.<generation>.ravo.json` and contains a bounded canonical
  versioned snapshot of catalog/source identity, review and capture state,
  tags, writable metadata, the canonical recipe, and ordered history. A
  SHA-256 envelope detects corruption. The global catalog revision is an
  observation at serialization time; asset ID plus generation and the payload
  excluding that observation define immutable generation content.
- Filesystem publication happens after the database commit. It uses a unique
  generation filename and atomic file replacement, verifies the published
  artifact, acknowledges only the exact generation, and removes older files
  only after that acknowledgement. A newer concurrent generation therefore
  remains pending. A database commit followed by filesystem failure is not
  rolled back; the error reports `catalog_committed=true` and
  `recovery_pending=true`.
- Catalog open retries the durable pending set. Explicit sync and clean close
  drain it again. Other catalog mutations publish synchronously. Studio
  Develop coalescing queues the preview result for the UI before the same
  serial worker publishes recovery, so adjustment-to-preview latency contains
  no sidecar serialization or filesystem write without leaving the generation
  deferred indefinitely. Sync, close, reopen, and backup retry any failure.
- Import still ignores existing adjacent `.xmp` files and never attaches or
  rewrites them. Explicit `recipe import-xmp`, newly embedded rendered XMP,
  and exact original-copy behavior from ADR-0063 remain unchanged. The Ravo
  recovery JSON is not an interchange XMP and is never an implicit input.
- A version-1 catalog backup is an immutable absent destination directory with
  exactly `catalog.sqlite`, `manifest.json`, and `sidecars/`. Creation first
  drains pending generations and checks the live catalog, uses SQLite
  `VACUUM INTO` for a consistent database snapshot, removes preview rows from
  that copy, copies and verifies the exact committed sidecar generation set,
  rejects a source revision/generation change, writes the strict manifest
  last, verifies the staged backup, and atomically publishes the directory
  without replacement.
- The manifest records format/schema/catalog identities, revision, creation
  time, database bytes and SHA-256, ordered sidecar identities/generations/
  bytes/hashes, and explicit `originals` and `previews` exclusions. Verification
  rejects unknown/missing entries, symlinks, checksum or identity mismatch,
  pending recovery state, preview rows, malformed/newer manifests, and SQLite
  integrity failure without opening the backup as a live catalog.
- CLI `catalog sidecar-status`, `sidecar-sync`, `backup`, and `backup-verify`
  expose the shared service contract through versioned JSON. `backup-verify`
  depends only on its backup directory and uses read-only database/artifact
  verifiers rather than a live catalog session. Restore and Studio backup UI
  remain separate unfinished product surfaces; verification is not described
  as restore.

## Consequences

A catalog edit has one authoritative commit and a retryable derived recovery
artifact instead of two writable authorities. A clean backup is small relative
to the source library and can be verified independently, while originals and
adjacent sidecars remain byte-, size-, and mtime-stable. A corrupt or unwritable
recovery store is visible and leaves an exact pending generation rather than
silently claiming durability.

Sidecar serialization remains bounded by 16 MiB and 10,000 history entries per
asset; exceeding either bound is an explicit recovery error after the catalog
commit. SQLite `VACUUM INTO` itself is not interruptible in the middle of the
statement, although cancellation is checked before it and during subsequent
pruning, hashing, copying, and verification. Restore, scheduled retention,
cloud/network destinations, and adjacent interoperable XMP are not introduced
by this decision.

## Rejected alternatives

- Writing Ravo state beside every original. Referenced and read-only media must
  remain importable, and existing XMP belongs to the user or another product.
- Treating sidecars as a peer authority or merging them automatically. That
  needs a separate external-edit conflict policy and would make deterministic
  catalog recovery ambiguous.
- Serializing a sidecar inside every Studio adjustment commit. It adds
  variable JSON/filesystem latency to the first visible edited frame.
- Backing up preview files or rows. They are contract-versioned caches and must
  rebuild through the ordinary preview owner after a future restore.
- Copying originals into a catalog backup. Managed-media backup is a separate
  storage lifecycle with capacity, collision, and source-mutation policy.
