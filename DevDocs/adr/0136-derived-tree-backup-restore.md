# ADR-0136: Derived-tree and external-editor backup/restore packaging

- Status: Accepted
- Date: 2026-09-04
- Relates: PRO-INTERCHANGE in [TODO.md](../TODO.md), R2 in
  [ProductRoadmap.md](../ProductRoadmap.md)
- Extends: [ADR-0097](0097-catalog-recovery-sidecars-and-verifiable-backups.md),
  [ADR-0099](0099-atomic-catalog-restore-and-operator-recovery.md),
  [ADR-0122](0122-external-editor-derived-assets.md)
- Supersedes: the ADR-0122 residual that deferred backup/restore packaging of
  `{catalog}.ravo/derived/` (and external-editor provenance) beyond recovery
  sidecars

## Context

ADR-0097/0099 verify catalog SQLite + recovery sidecars and explicitly exclude
originals and rebuildable previews. ADR-0122 stores external-editor pixels under
`{catalog}.ravo/derived/` and provenance under `{catalog}.ravo/external-editor/`.
Those trees are catalog-owned and not rebuildable from recipes; omitting them
from backup silently loses derived assets after restore.

## Decision

### Backup must include derived and external-editor trees

- A verified catalog backup **must** package:
  - existing recovery sidecars under `sidecars/` (unchanged);
  - the full byte tree `{catalog}.ravo/derived/` when present;
  - the full byte tree `{catalog}.ravo/external-editor/` when present.
- Trees absent on the live catalog are recorded as empty lists (not invented).
- **Originals are never copied** as a side effect of derived packaging. Excludes
  remain `originals` and `previews`.

### Manifest format version 2

- Bump `kCatalogBackupFormatVersion` to **2**.
- Manifest root keys add required arrays `derived` and `external_editor`
  (may be empty). Each entry is `{file, bytes, sha256}` where `file` is a
  relative POSIX path under that tree (no `..`, no absolute paths).
- Version 1 backups remain **readable for verify/restore of catalog+sidecars
  only**; opening a v1 backup does not invent derived trees. New backups always
  write v2.

### Content-address verification

- Each packaged derived/external-editor file is copied with the same atomic
  no-replace + SHA-256 verify posture already used for recovery sidecars.
- `verify_backup` fails closed if any listed file is missing, extra, size-
  mismatched, or checksum-mismatched. Never invent or synthesize pixels.

### Restore

- Restore restores derived and external-editor trees **atomically with** the
  catalog support root (support-first / catalog-last, ADR-0099).
- Destination `{dest}.ravo/` may contain `sidecars/`, `derived/`, and
  `external-editor/` after restore. Layout verification allows exactly those
  known directories when present.
- If a listed derived/provenance file is missing from the backup package,
  restore/verify reports the path and fails closed — it must not invent pixels
  or skip silently while claiming success.

### First Ready code

Wire the existing `create_backup` / `verify_backup` / `restore_catalog_backup`
path and add tests using a registered derived asset fixture (ADR-0122).

## Non-goals (explicit)

- Copying originals or rebuildable preview caches into the backup.
- Re-deriving editor pixels from provenance after restore.
- Launching external editors or Gallery auto-stack UX (ADR-0122 residuals).
- Changing recovery sidecar identity or pending-generation gates.

## Consequences

PRO-INTERCHANGE derived assets survive verified backup/restore. Format v2 is a
one-way write bump; v1 remains readable for catalog+sidecar recovery. Operators
must use a Ravo that understands v2 to restore derived trees. Restored
`{dest}.ravo/derived/` and `external-editor/` trees are byte-identical to the
backup package; rewriting catalog absolute URIs that still name the *source*
`{catalog}.ravo/` prefix onto the destination path is a follow-up (assets remain
openable when the source tree still exists, and trees remain on disk under the
destination support root regardless).

## Rejected alternatives

- Leaving derived trees out of backup until a separate archive product exists.
- Embedding derived bytes inside SQLite BLOBs (duplicates ADR-0122 file store).
- Silently skipping missing derived files on restore while reporting success.
- Copying originals “just in case” when packaging derived trees.
