# ADR-0107: Apply selected Develop fields to an explicit multi-selection

- Status: Accepted
- Date: 2026-09-01
- Extends: [ADR-0078](0078-copy-paste-develop-edits.md),
  [ADR-0098](0098-selective-develop-presets.md),
  [ADR-0080](0080-studio-observes-catalog-revision.md)

## Context

Selective Copy/Paste Parameters stores one immutable `DevelopParams` snapshot
plus an explicit field set and pastes onto the active photo through ordinary
history/undo (ADR-0078/0098). After culling, photographers need the same overlay
on every ID in an explicit multi-selection without inventing a second recipe
model or resetting unselected destination edits.

Catalog revision is a single SQLite counter on `CatalogSnapshot`. Each recipe
commit increments it, so a per-destination catalog-revision check cannot succeed
inside one batch.

## Decision

- CatalogService owns `apply_develop_selection`. The request carries one
  `DevelopParams` source snapshot, the existing selectable-field IDs, ordered
  unique destination asset IDs, an optional catalog `expected_revision`, and a
  `CancellationToken`. The bound is `kExportBatchMaxAssets`. CLI and Studio
  share this method. QML displays state and forwards the command only.
- Preflight is fail-closed and writes nothing: empty or oversized batch,
  empty/duplicate destination IDs, missing assets, empty/unknown/duplicate
  fields, or a stale catalog revision. Field validation reuses
  `apply_develop_selected_fields`.
- Each destination loads its current recipe, overlays only the requested
  fields, and `save_develop_with_history`. Item load/merge/save failures are
  recorded and do not abort destinations already committed. Cancellation skips
  remaining IDs; completed photos stay committed. The result reports
  applied/failed/skipped counts, per-item status, and the catalog revision after
  the last successful write.
- CLI is `catalog develop-apply --from-asset <id> --asset-id <id>… --fields a,b
  [--revision N] --json`. Studio keeps one-photo **Paste Parameters** on the
  session undo path. **Paste Parameters to Selection** is enabled when the
  session clipboard exists and the selection has at least two photos; it posts
  the same CatalogService command with the observed catalog revision. Session
  undo does not revert other destinations; those photos keep per-photo history.
  If the primary photo is a destination, Studio reloads it from the catalog.

## Consequences

Ten selected photos can receive the same Exposure/WB subset while keeping
unrelated local edits. The session clipboard remains the Studio source; CLI
loads the source snapshot from `--from-asset`. OS clipboard exchange and
applying a named library set as an implicit destination list remain later work.

## Rejected alternatives

- One failed `Result` after a successful prefix, as batch export does. Callers
  need a partial-failure report and must not roll back completed photos.
- Per-asset catalog revision checks inside the loop. `commit_recipe` bumps the
  global catalog revision between items.
- A second recipe or field model for “sync settings.” The existing chooser and
  merge owner are the contract.
- Applying pending in-memory sliders from the live primary. The source is the
  immutable clipboard snapshot or the loaded `--from-asset` recipe.
