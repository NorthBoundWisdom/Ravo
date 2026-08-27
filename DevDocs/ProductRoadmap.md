# Deferred product capabilities

This document tracks only unfinished capabilities that still require a dated
product or architecture decision before they can enter execution. It is not a
release checklist and does not repeat completed Ravo or legacy migration work.

## Selection rule

A capability belongs here only when:

- the user-visible outcome and retained legacy scope are not yet fixed;
- ownership crosses engine/services/desktop, a source-root dependency, or a
  durable catalog/recipe contract;
- failure behavior and deterministic completion evidence still need a design
  decision.

The complete legacy module inventory, execution order and retirement gates now
live only in [`TODO_LEGACY_MIGRATION.md`](../TODO_LEGACY_MIGRATION.md). This
document records cross-layer decisions that those rows depend on; it must not
carry a second module queue or completion status. Completed conclusions belong
in their owning architecture/ADR/code/test truth source.

## Mask and local adjustment graph

- Define general drawn/parametric mask and blend ownership.
- Define coordinate spaces, composition order, immutable publication,
  cancellation, ROI/tile behavior and recipe versioning before adding UI.
- `ravo.effect.graduatednd` is the first local adjustment and uses the gradient
  itself as the mask. A generic mask/blend graph still needs this decision
  before drawn or parametric masks.

## RAW and optical contracts

- Each queued RAW row needs explicit stage placement, unsupported sensor policy,
  real fixture and memory/time budget.

## Color, profile and creative-look contracts

- Define explicit workspace/profile/gamut ownership and overlap with accepted
  defaults before each queued color operation freezes its schema.
- LUT support requires an explicit format/profile adapter and deterministic
  missing/invalid-file behavior.

## Geometry, ROI and resource contracts

- Define coordinate spaces, resampling, canvas growth, tiling and mask
  transforms before the queued geometry/deformation rows execute.
- External image/LUT/SVG/font resources require versioned lookup, immutable
  task ownership and deterministic missing/corrupt behavior.

## Export workflow expansion

- Batch job persistence, presets/styles, and still-undecided export workflow.
- Bounded Catalog-owned embedded JPEG/PNG/TIFF metadata is accepted under
  [ADR-0038](../Ravo/docs/adr/0038-embedded-export-metadata.md). Capture
  timezone/GPS and sidecar/history policy remain later S9/J6 work.

## Catalog and source-file lifecycle

- Legacy catalog migration, managed copy, move/relink, backup/restore and
  sidecar writeback.
- Require a backup/rollback contract and explicit original-file mutation policy
  before schema or UI work begins.

## Extended library workflows

- Collections, smart search, faces and GPS-oriented workflows.
- These remain outside the local review/develop baseline until privacy,
  indexing, persistence and user-facing failure behavior are defined.

## Explicit non-candidates

Only the non-algorithm UI/ABI/OpenCL/data leftovers listed in
[`Ravo/MIGRATION.md`](../Ravo/MIGRATION.md) are non-candidates. ADR-0015 puts
every remaining image algorithm in migration scope; execution status remains
exclusively in the root TODO.
