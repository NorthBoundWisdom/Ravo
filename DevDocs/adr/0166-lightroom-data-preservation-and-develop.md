# ADR-0166: Lightroom preservation and compatible Develop conversion

- Status: Accepted
- Date: 2026-10-10
- Extends: [ADR-0164](0164-lightroom-catalog-reader.md)
- Receipts, audit and conservative resume are extended by
  [ADR-0167](0167-foreign-conversion-checkpoints.md).

## Decision

The requested RAWmakase integration admits database Develop conversion,
independent virtual copies, descriptive metadata, collections, history/snapshots
and explicit source-volume mappings. Implementation remains C++20 in Ravo's
existing owners, without a Rust target, Adobe runtime or Lua interpreter.
The referenced parsing design retains RAWmakase's MIT notice.

Adapters return bounded owned values from a closed private SQLite snapshot.
Serialized `s = { ... }` settings are data with duplicate-key, nesting, size and
malformed-string checks. Independent parameter groups use the existing CRS
adapter; coupled controls convert together. Unsupported groups are explicit
omissions and do not block independent supported groups. This policy is
specific to catalog conversion; explicit CRS preset import stays fail-closed.
Compatible crop coordinates, straightening and Texture use existing Ravo owners. There is no
Lightroom appearance-equivalence claim or silent renderer/profile fallback.

ConversionService owns writes. Masters import before virtual copies; copies
use the existing asset-version service and independent recipes/review/keywords.
The import intent clears the version command's cloned edit, tags and writable
metadata before applying the copy's own record; empty foreign state must not
inherit a master's adjustments, keywords or labels.
Catalog XMP uses the existing metadata parser; keywords and review still come
from Lightroom's tables. Compatible history steps and snapshots become named
Ravo snapshots and use ordinary restore. Each state starts from the product
baseline rather than accumulating previously imported steps.
Prepared states use DevelopService's same snapshot owner, including recipe/source
validation, catalog revision and recovery publication even when no current edit
exists. Import does not append history directly through the repository port.

Collections use existing manual sets. Parent names form a display path; Adobe
smart rules are not evaluated. Stored smart membership is a static snapshot
with an explicit reason. Reports include collection IDs, destination set IDs,
member counts and missing-member/failure reasons. Nested read-only ownership
remains a separate gate.

Import path mappings select the longest matching path-segment prefix and never
fall back to a different original path. Missing media are explicit reported
items, not invented file identities. Per-computer offline locations require
qualification before replacing current folder-relink identity policy.
An explicit bounded foreign-ID selection can validate a representative subset
without hashing or reading unselected originals. Unknown/duplicate IDs and a
copy without its selected master reject before writes. The full original
catalog remains archived; collection membership outside the selection is
reported as partial rather than silently claimed complete.
Explicit selection carries the observed source SHA-256; missing hashes and
source changes reject before any destination write.

Schema 19 stores immutable source metadata and ordered 1 MiB SQLite BLOB chunks.
Source bytes are streamed, hashed and committed in one cancellation-aware
transaction before photo conversion. Hash mismatch/failure rolls back source,
chunks and revision. Ordinary SQLite backup/restore includes the archive.
Export verifies chunk order, size and SHA-256 before no-replace publication.
Preserved bytes do not mean unsupported fields are operationally mapped.
External `.lrcat-data`, previews, originals and profiles remain separate inputs.
Read-only `inspect-foreign` reports bounded source statistics, Develop-field
frequencies, parse diagnostics and companion presence without a destination.
Catalog text supports declared-length zlib records with bounded expansion;
empty current settings are absence, not an editing recipe.
Legacy 2003/2010 exposure maps in EV; other active legacy tonal controls remain
explicit omissions. Parallel obsolete controls in PV2012 records are not
applied a second time.

Studio retains its catalog executor and destruction cancellation; there are no
new threads or UI/SQL handles. Completed destination work survives cancellation
or later item failure. ADR-0167 adds explicit journal-based resume; an archive
alone is not a checkpoint. The archive is never a live
vendor database owner.

## Validation

Synthetic service tests cover database-only settings without a sidecar,
independent copy edits, metadata, named history/snapshots, collection membership,
source hashes after reopen, archive rollback, verified export and output
conflict/cancellation. Schema upgrade, backup/restore, native CLI and desktop
command acceptance remain mandatory. [Execution gates](../TODO_LIGHTROOM_IMPORT.md)
track remaining RAWmakase mappings, real catalogs and platform qualification.
