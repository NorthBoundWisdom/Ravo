# ADR-0102: Planned Add, Copy, and Move import workspace

- Status: Accepted
- Date: 2026-09-01
- Extends: [ADR-0007](0007-first-usable-catalog-viewer.md),
  [ADR-0028](0028-original-copy-publication-contract.md), and
  [ADR-0100](0100-paged-library-and-foreground-work-scheduling.md)

## Context

The first catalog slice intentionally accepted only reference import. Direct
file and folder dialogs could not review a source, select individual photos,
plan managed destinations, or choose initial preview work. Publishing every
sorted insertion also made Gallery unstable during a batch.

## Decision

- Studio has one full-page import workspace. One local source root is scanned
  deterministically with optional recursion. Supported non-duplicates start
  selected and request bounded 320-pixel thumbnails through C++ services.
- A typed request chooses Add, Copy, or Move; single-folder, preserved-root, or
  `YYYY/MM/DD` organization; and Minimal 320, Standard 1600, or full-size 1:1
  previews. Session defaults are Add, recursive, Standard, and single-folder.
- Copy and Move require an existing destination root and preserve names. The
  complete media and same-stem XMP output set is preflighted before mutation.
  Existing outputs, symlinks, duplicate paths, catalog destination conflicts,
  and ambiguous `.xmp`/`.XMP` pairs reject with zero publication. There is no
  overwrite, skip, or unique-name fallback.
- Copy uses bounded atomic no-replace publication. Move copies and catalogs the
  destination before rechecking and removing the source. Cleanup failure keeps
  safe bytes at the source and reports it explicitly rather than risking data
  loss through rollback.
- Catalog publication returns before selected preview rendering. Studio closes
  the workspace, selects Last Imported Photos, and drains one cancellable
  background preview at a time. Preview failure is rebuildable and never
  removes an asset.
- CLI uses the same planner and transfer contract, but completes previews
  synchronously because it cannot own work after process exit.

## Consequences

Managed import changes ADR-0007's original reference-only product scope without
changing the catalog schema: assets still store only final normalized URIs.
PTP/MTP, DNG conversion, metadata presets, and Smart Previews remain out of
scope. [ADR-0104](0104-bounded-rename-and-verified-second-copy-ingest.md)
subsequently accepts bounded renaming and a byte-verified second-copy tree for
Copy/Move. Add and Copy never alter originals; Move is the only explicit
source-deleting import mode.
