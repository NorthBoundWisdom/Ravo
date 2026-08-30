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

- Each queued RAW row needs explicit stage placement, unsupported sensor policy,
  real fixture and memory/time budget.

## Color, profile and creative-look contracts

- Define explicit workspace/profile/gamut ownership and overlap with accepted
  defaults before each queued color operation freezes its schema.
- LUT support requires an explicit format/profile adapter and deterministic
  missing/invalid-file behavior.

## Geometry, ROI and resource contracts

- ADR-0070 accepts Canvas growth and an attached photo-content frame for
  full-frame mask evaluation. Remaining geometry/deformation rows must define
  resampling, tiling, sub-ROI, and composed content-frame transforms before
  they execute after Canvas; unsupported combinations continue to fail.
- External image/LUT/SVG/font resources require versioned lookup, immutable
  task ownership and deterministic missing/corrupt behavior.

## Export workflow expansion

- Background batch-job persistence and reusable export-option presets remain
  undecided; bounded foreground batch export and recipe styles are accepted.
- Bounded Catalog-owned embedded JPEG/PNG/TIFF metadata is accepted under
  [ADR-0038](../Ravo/docs/adr/0038-embedded-export-metadata.md) and
  [ADR-0040](../Ravo/docs/adr/0040-capture-time-gps-metadata.md).
  ADR-0063/0064 accept no automatic sidecars, atomic metadata refresh, and
  full/no-location/none privacy. PNG pHYs, TIFF multipage masks, and shared old
  format/job retirement remain under their specific owners.

## Catalog and source-file lifecycle

- Legacy catalog migration, managed copy, move/relink, backup/restore and
  sidecar writeback.
- Require a backup/rollback contract and explicit original-file mutation policy
  before schema or UI work begins.

## Extended library workflows

- Collections, smart search, faces and GPS-oriented workflows.
- These remain outside the local review/develop baseline until privacy,
  indexing, persistence and user-facing failure behavior are defined.

## Local agent automation and live Studio control

The accepted headless baseline is the versioned `ravo --json` interface.
Explicit catalog and asset IDs select stored state; `catalog recipe` and
`catalog history` inspect it, `catalog probe` produces read-only statistics and
an optional no-replace PNG, and `catalog develop` commits through the shared
service path. Studio observes another client's catalog revision within one
second. None of these commands claims to expose the running window's current
selection or unsaved presentation state.

Before adding selection-relative automation, freeze one local control contract:

- Desktop C++ owns the ephemeral session snapshot and existing command routing;
  services remain the owner of catalog mutation, preview work, and image
  processing. QML, SQL adapters, logs, and preview-cache activity are not
  control-plane state.
- A snapshot identifies protocol/session version, catalog path/revision,
  primary and selected asset IDs, browse mode, recipe state/revision, and a
  bounded preview-resource identity. A mutation carries the observed session
  and selection revisions and rejects stale, closed, changed, busy, cancelled,
  or unavailable state without applying it to another photo.
- The transport is local same-user only, has bounded messages and images,
  supports multiple Studio sessions without a global writable singleton, and
  removes or invalidates discovery state on owner destruction. It does not
  expose assistant credentials or start a network listener by default.
- CLI is the required cross-platform client and contract-test surface. An MCP
  server may be a thin adapter over the same snapshots, command intents, and
  immutable image resources; it cannot introduce MCP-only mutations, another
  renderer/cache, or direct QML/SQLite/engine access.
- New live-control image results use atomic no-replace artifacts described by
  MIME type, dimensions, color-profile identity, content hash, and lifecycle
  rather than large inline JSON. MCP may return the identical immutable bytes
  as an image resource.

Acceptance requires structured CLI coverage for discovery, read-only state,
selection/revision races, concurrent catalog writes, cancellation, unavailable
commands, output conflict, multiple sessions, process-exit cleanup, bounded
resource use, and proof that no secret settings enter the protocol. Until then,
agents require explicit catalog and asset identity and must not substitute UI
automation or process/log inference.

## Explicit non-candidates

Only the non-algorithm UI/ABI/OpenCL/data leftovers listed in
[`Ravo/MIGRATION.md`](../Ravo/MIGRATION.md) are non-candidates. ADR-0015 puts
every remaining image algorithm in migration scope; execution status remains
exclusively in the root TODO.
