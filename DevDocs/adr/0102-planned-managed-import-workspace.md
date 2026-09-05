# ADR-0102: Planned Add, Copy, and Move import workspace

- Status: Accepted
- Date: 2026-09-01
- Updated: 2026-09-05 — content filtering, destination preference, responsive workspace
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
  deterministically with optional recursion. Cancellable content and metadata
  checks publish named new-photo placeholders incrementally. Supported non-duplicates start selected and
  request bounded 320-pixel thumbnails through C++ services as the grid
  demands them. The workspace grid fits available width. A highlight set is
  distinct from the import checkbox: Command/Control multi-selects, Shift
  selects a range, Command/Control+A selects all, and checking one highlighted
  cell applies that check state to every highlighted eligible photo.
- A typed request chooses Add, Copy, or Move; single-folder, preserved-root,
  `YYYY/MM/DD`, or `YYYY/MM` organization; and Minimal 320, Standard 1600, or
  full-size 1:1 previews. Workspace defaults are Copy, recursive, Standard, and
  single-folder. Studio shows mounted-folder trees for source and Copy/Move
  destination; QML only displays the C++ browser model.
- The page has source / new-photo grid / scrollable destination settings.
  Rename and second-copy sections start collapsed; windows below 1000px use
  side drawers. Copy is selected on every entry. Mounted-card Move remains
  unavailable under the ingest transport contract.
- Catalog URI or full-file SHA-256 matches are hidden. Stable path order keeps
  the first supported representative of each same-content group in a scan.
  Same name, size, or timestamp alone never establishes a duplicate. Unavailable
  candidates stay visible and unchecked. Uncheck All also affects later arrivals.
- Schema v17 adds a derived content-hash table and size/hash index. New imports
  commit hashes with assets; old records are filled in bounded, size-matched
  pages. Index writes do not advance catalog or recovery revisions. Indexed
  offline originals retain their imported identity; unknown or changed originals
  yield explicit errors. The detailed data and lifecycle contract is in architecture.
- `StudioImportPreferences` owns `desktop/import/lastDestination`, shared across
  catalogs. The first committed managed-import photo remembers the primary
  root. Failed batches and cancelled drafts preserve the prior value. Reopen
  restores the panel path, reveals the folder tree, and initializes the picker.
  Unavailable roots block Copy until reconnected or reselected. Preference write
  failure is reported independently of successful photo publication.
- `catalog import-scan --json` exposes `ravo-import-scan/v1` from the same
  service. `catalog import/ingest --skip-existing` enables Studio's content
  policy; CLI's default Add/URI policy is unchanged. Preflight rechecks selected
  hashes and catalog revision before opening Gallery. A concurrent duplicate
  after file publication retains safe output bytes and reports their paths,
  since another client may already have cataloged that URI.
- Copy and Move require an existing destination root and preserve names. The
  complete media, same-stem XMP, and same-stem JPEG companion output set is
  preflighted before mutation. Existing outputs, symlinks, duplicate paths,
  catalog destination conflicts, and ambiguous `.xmp`/`.XMP` or `.jpg`/`.jpeg`
  pairs reject with zero publication. There is no overwrite, skip, or
  unique-name fallback.
- Copy uses bounded atomic no-replace publication. Move copies and catalogs the
  destination before rechecking and removing the source. Cleanup failure keeps
  safe bytes at the source and reports it explicitly rather than risking data
  loss through rollback.
- Catalog publication returns before selected preview rendering. Studio closes
  the workspace immediately, publishes named Gallery placeholders for the
  selected files, fills each cell as that item is cataloged, and drains one
  cancellable background preview at a time. Add import dispatches one item at
  a time so viewport browse thumbnails can run between items. Gallery cells
  keep the `kThumbnailMaxEdge` browse thumbnail; Standard/1:1 drain warms the
  processed cache and does not replace grid pixels. Preview failure is
  rebuildable and never removes an asset.
- CLI uses the same planner and transfer contract, but completes previews
  synchronously because it cannot own work after process exit.

## Consequences

Managed import changes ADR-0007's original reference-only product scope.
Assets still reference final normalized URIs; schema v17's content index is
rebuildable and contains neither originals nor editing state.
PTP/MTP, DNG conversion, metadata presets, and Smart Previews remain out of
scope. [ADR-0104](0104-bounded-rename-and-verified-second-copy-ingest.md)
subsequently accepts bounded renaming and a byte-verified second-copy tree for
Copy/Move. Add and Copy never alter originals; Move is the only explicit
source-deleting import mode.
