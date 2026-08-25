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

Once those facts are explicit, create or extend a root `TODO_<TOPIC>.md` with
the execution sequence, dependencies, validation commands and completion gate.
Remove the promoted item from this document in the same change. Completed work
belongs in its owning architecture/contract/code/test truth source, not here.

Do not promote another RAW/color IOP or a lensfun source-root until
[`TODO_LEGACY_MIGRATION.md`](../TODO_LEGACY_MIGRATION.md) finishes C1 closeout
and L1 `channelmixerrgb`. Promoted algorithm work must replicate the frozen C
default CPU path and then retire the old owner; it must not land a simplified
substitute and treat that as migration complete. The next algorithm batch, if
promoted, is either `hotpixels` plus one CA method, or `colorbalancergb`—not
both, and not a generic denoise stack.

## Mask and local adjustment graph

- General mask/blend ownership: `mask_manager`, drawn masks and parametric masks.
- Define coordinate spaces, composition order, immutable publication,
  cancellation, ROI/tile behavior and recipe versioning before adding UI.
- `ravo.effect.graduatednd` is the first local adjustment and uses the gradient
  itself as the mask. A generic mask/blend graph still needs this decision
  before drawn or parametric masks.

## Additional RAW and optical repair

- `cacorrect`, `cacorrectrgb`, `hotpixels`, `rawdenoise`.
- Each needs a selected default, RAW-stage placement, unsupported sensor policy,
  real fixture and memory/time budget.

## Color and creative look

- `lut3d`, `colorbalancergb`, `borders`.
- Admit only capabilities that do not duplicate the accepted Sigmoid,
  tone-curve, 8-band `colorequal`, or the queued channel-mixer.
- LUT support requires an explicit format/profile adapter and deterministic
  missing/invalid-file behavior.

## Export workflow expansion

- Metadata/ICC embedding, batch job persistence and presets/styles.
- Define allowed fields, profile ownership, overwrite/conflict policy,
  cancellation, crash recovery and reproducible output evidence.

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

Capabilities listed as leftover in [`Ravo/MIGRATION.md`](../Ravo/MIGRATION.md)
do not belong here. Reopening one requires a dated product decision that first
changes the migration boundary.
