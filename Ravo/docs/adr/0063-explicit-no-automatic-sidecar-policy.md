# ADR-0063: Catalog editing never automatically reads or writes sidecars

- Status: Partially superseded by ADR-0097
- Date: 2026-08-29
- Extends: [ADR-0009](0009-p1-develop-recipe.md)
- Partially superseded by:
  [ADR-0097](0097-catalog-recovery-sidecars-and-verifiable-backups.md)

## Context

The old sidecar job continuously synchronized mutable application state to
adjacent XMP files and was called from image, history, crawler, GUI, and global
startup owners. Ravo stores canonical recipes/history and writable metadata in
its SQLite catalog, while rendered exports already embed a bounded fresh
Exif/XMP/IPTC snapshot. Automatic sidecar synchronization would introduce a
second state owner and mutate paths the user imported by reference.

## Decision

- Import treats `.xmp` and other sidecars as non-assets and never implicitly
  attaches them to a photo. Existing adjacent sidecars are read-only and
  unchanged.
- Catalog recipe, recipe history, tags, review state, and writable metadata are
  the only live edit owners. Save, undo, snapshot, metadata changes, removal,
  and application shutdown generate no sidecar.
- Historical XMP conversion is explicit only:
  `ravo recipe import-xmp` reads a caller-selected file, accepts the strict
  evidenced subset, and writes a new canonical recipe destination. It does not
  attach to a catalog or modify the input/image.
- Rendered JPEG/PNG/TIFF XMP is newly encoded inside the destination from the
  bounded Catalog export snapshot. It is not a sidecar and does not copy an
  opaque source packet. Original-copy writes exact media bytes and creates no
  metadata or sidecar.
- Ravo provides no automatic sidecar read, write, watch, conflict merge,
  background queue, configuration compatibility, or “write on import” mode.
  A future explicit interchange format would require a separate versioned
  contract and atomic conflict policy.

## Consequences

J6's product policy and original-safety gate are accepted. The old
`control/jobs/sidecar_jobs.*` wrapper cannot yet be deleted because old
`common/darktable.c` and `common/image.c` still call it; those files and direct
sidecar writers remain S7/S9/J2/D0 cleanup dependencies. No Ravo fallback or
disabled sidecar switch is added.

## Rejected alternatives

- Mirroring every edit beside the original. It violates reference-only import
  and creates two authorities with unowned conflict recovery.
- Automatically consuming adjacent legacy XMP. The importer intentionally
  supports only strict, explicit, evidence-bound conversion.
- Calling embedded export XMP a sidecar. Its ownership, destination, and
  bounded metadata contract are different.
