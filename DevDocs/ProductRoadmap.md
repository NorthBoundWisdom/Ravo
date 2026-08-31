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

The remaining product P0/P1 release evidence lives only in
[`TODO_PHOTO_MANAGEMENT.md`](../TODO_PHOTO_MANAGEMENT.md). The complete legacy
module inventory, `MR*` migration ranks, and retirement gates live only in
[`TODO_LEGACY_MIGRATION.md`](../TODO_LEGACY_MIGRATION.md). This document records
cross-layer decisions that are not ready for either queue; it must not carry a
second execution order or completion status. Completed conclusions belong in
their owning architecture/ADR/code/test truth source.

## Mask and local adjustment graph

ADR-0043 freezes S3.1 ownership, attached-frame pixel-centre coordinates,
immutable recipe publication, ROI/cancellation, and normal mix for a bounded
typed all/linear-gradient/circle/ellipse/parametric/group graph. ADR-0044
accepts S3.2's bounded Studio-owned spatial/parametric leaf authoring for
Color Harmonizer and Graduated ND; Graduated ND's own density gradient remains
separate operation math. ADR-0045 accepts preview-only overlay, owned
group-child editing, and path/brush sampling/lifecycle; Color Harmonizer's
frozen IOP is retired.

- Decide picker, histogram/harmony interactions, and any additional undo intent
  without moving graph mathematics into QML.
- Decide any additional blend modes only with a named operation consumer and
  source-order/failure/ROI contract; historic blend-mode completeness is not
  implied by S3.
- Leftover GTK `mask_manager` / `libs/masks.c` wait for zero develop/history
  consumers. Strict legacy XMP mask/custom-blend rejection remains current
  policy. C15 and `cacorrectrgb` remain outside execution until a later exact
  tranche.

## RAW and optical contracts

- ADR-0096 fixes the serial RAW/optical decision order and fail-closed ownership
  boundary. Exact per-operation stage placement, real fixtures, and measured
  memory/time limits remain the active execution gates in the root TODO.

## Color, profile and creative-look contracts

- Define explicit workspace/profile/gamut ownership and overlap with accepted
  defaults before each queued color operation freezes its schema.

## Geometry, ROI and resource contracts

- ADR-0070/0093 accept Canvas growth, an attached photo-content frame, and
  ordered Perspective/straighten/crop composition of pixels plus preview
  alpha. Rotate/flip/lens after Canvas, attached sub-ROI evaluation, and a new
  mask consumer after composed geometry remain explicit failures until their
  resampling and coordinate owners are defined.
- External image/LUT/SVG/font resources require versioned lookup, immutable
  task ownership and deterministic missing/corrupt behavior.

## Export workflow expansion

- Background batch-job persistence and reusable export-option presets remain
  undecided; bounded foreground batch export and recipe styles are accepted.
- Bounded Catalog-owned embedded JPEG/PNG/TIFF metadata is accepted under
  [ADR-0038](../Ravo/docs/adr/0038-embedded-export-metadata.md) and
  [ADR-0040](../Ravo/docs/adr/0040-capture-time-gps-metadata.md).
  ADR-0063/0064 accept no adjacent interchange sidecars, atomic metadata
  refresh, and full/no-location/none privacy. ADR-0097 adds catalog-owned
  recovery mirrors and verified catalog backups without changing original/XMP
  ownership. PNG pHYs, TIFF multipage masks, and shared old format/job
  retirement remain under their specific owners.

## Catalog and source-file lifecycle

- Backup failure hardening, absent-destination restore, Studio recovery,
  bounded paging/import scheduling, verified scheduled retention, stable
  folder identity, and relink are implemented under ADR-0099–0101. The product
  P0/P1 TODO retains only private-corpus and non-macOS release evidence; these
  capabilities are no longer deferred design items here.
- Managed copy/move still requires a dated decision for collision policy,
  cross-volume publication, source mutation detection, rollback, cancellation,
  recovery-sidecar updates, and original ownership before schema or UI work.
- Legacy catalog import still requires a bounded source-version matrix,
  read-only conversion artifact, conflict/rejection policy, rollback, and
  fixtures. It must not open or migrate a frozen catalog in place.
- Adjacent standard-XMP writeback remains a separate interoperability decision.
  It must not silently consume or overwrite an existing sidecar or become a
  second live authority beside SQLite.

## Extended library workflows

- Collections and smart search, stacking/versions, duplicate-content
  detection, face/GPS-oriented workflows, and removable or network catalogs.
- Each requires a measured user outcome plus explicit privacy, indexing,
  persistence, offline/conflict, cancellation, and user-facing failure behavior
  before it can enter the product TODO. Do not add placeholder tables or empty
  Studio surfaces.

## Explicit non-candidates

Only the non-algorithm UI/ABI/OpenCL/data leftovers listed in
[`Ravo/MIGRATION.md`](../Ravo/MIGRATION.md) are non-candidates. ADR-0015 puts
every remaining image algorithm in migration scope; execution status remains
exclusively in the root TODO.
