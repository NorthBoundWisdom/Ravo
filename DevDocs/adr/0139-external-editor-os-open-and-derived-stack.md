# ADR-0139: External-editor OS open-with + derived-pair Gallery stack

- Status: Accepted
- Date: 2026-09-04
- Relates: PRO-INTERCHANGE in [TODO.md](../TODO.md), R2 in
  [ProductRoadmap.md](../ProductRoadmap.md)
- Extends: [ADR-0122](0122-external-editor-derived-assets.md),
  [ADR-0105](0105-asset-versions-stacks-and-survey.md),
  [ADR-0136](0136-derived-tree-backup-restore.md)
- Supersedes: the ADR-0122 deferrals of “launching external editors” (OS
  open-with only) and “auto-stacking / Gallery UX for derived pairs” (service
  auto-stack on register; richer Gallery UX remains residual)

## Context

ADR-0122 lets photographers register editor output as a catalog-owned derived
asset with provenance, without mutating originals or inventing a hidden
renderer. Operators still hand-open files outside Ravo and stack derived pairs
manually. Photographers expect a **user-initiated** “Edit in …” that opens a
safe path in the OS default/open-with handler and, after register, optionally
stacks the derived asset with its source in the existing Gallery stack model.

## Decision

### Initiation — record intent, return OS open payload

- `CatalogService::prepare_external_editor_open` is the sole authority that
  decides which absolute path/URI to open. It requires explicit user initiation
  (`user_initiated == true`). It **never** launches an editor, never scripts
  proprietary editor APIs, and never starts a watch-folder importer.
- On success it **records** a durable open-intent document under
  `{catalog}.ravo/external-editor/open-intents/<intent-id>.json` with contract
  `ravo.external-editor.open-intent/v1`, then returns an OS open payload:
  `open_path`, `open_uri` (`file://`), `open_kind`, asset ids, and intent id.
- Desktop/CLI may invoke the platform open-with equivalent
  (`QDesktopServices` / `open` / `xdg-open`) **only** after an explicit user
  action (Studio click, or CLI `--invoke-os-open`). Invoking OS open is a
  presentation concern; the service contract stops at the payload + intent.
- Watch-folder auto-import without an explicit `editor-register` remains
  forbidden (ADR-0122).

### Which path opens (ADR-0122 originals rule)

| Asset | `open_kind` | Path |
| --- | --- | --- |
| External-editor derived (provenance present), or URI under `{catalog}.ravo/derived/` | `derived_working_copy` | The durable derived file (prefer provenance `derived_path`, else normalized URI path) |
| Ordinary original / non-derived | `original` | The catalogued original path |

- Prefer the derived working copy when editing a derived asset. Never open a
  path for the purpose of mutating an original in place as Ravo policy; when
  `open_kind` is `original`, editors must Save As to a distinct file and the
  user registers that output via ADR-0122. Source originals remain
  byte-identity checked on register.
- Optional `--editor` on open is an opaque hint recorded on the intent only; it
  does not select or script a product.

### Register + optional auto-stack

- Extend `editor-register` with `auto_stack` (CLI `--auto-stack`; Studio
  default-on when a Studio surface ships). When true, after a successful
  derived publication, call existing `stack_assets({source, derived},
  pick=derived)` so the edited version is the Gallery pick.
- **Fail closed** on stack conflict (`asset_already_stacked`, invalid members,
  stale revision): do not merge into an existing foreign stack, do not invent
  multi-parent membership. The derived asset and provenance from the successful
  register **remain** (pixels are already published); the combined command
  returns `kConflict` with `reason=editor_auto_stack_conflict` and
  `derived_asset_id` in context so the caller can unstack/retry manually.
- Users may `catalog unstack` at any time (existing ADR-0105 API).

### First Ready tranche

In scope:

- Service `prepare_external_editor_open` + open-intent persistence.
- CLI `catalog editor-open --asset-id … --user-initiated [--editor …]
  [--invoke-os-open] [--revision N]`.
- CLI/service `editor-register --auto-stack` wiring + conflict fail-closed.
- Unit/service tests (open payload kinds, intent on disk, auto-stack success +
  conflict, originals unchanged). Studio surface deferred if TU-heavy.

Out of scope:

- Proprietary editor scripting / COM / AppleScript / Photoshop actions.
- Watch-folder auto-register.
- Rich Gallery “Edit in …” chrome beyond calling the service payload.
- Preparing a new raster working copy of an original on open (Save As + register
  remains the original path).
- ADR-0136 destination URI rewrite residual.

## Consequences

Photographers get an explicit, auditable open → edit → register → optional
stack loop without a second live authority or hidden renderer. Open intents sit
under the existing `{catalog}.ravo/external-editor/` tree (covered by ADR-0136
backup packaging). Studio can later default `--auto-stack` on without a new
ADR.

## Rejected alternatives

- Auto-launching or scripting Photoshop/Affinity/GIMP from CatalogService.
- Watcher-based import of `*-edit.tif` without `editor-register`.
- Silently skipping stack conflicts while reporting overall success when
  `--auto-stack` was requested.
- Opening derived assets via the source original path (would encourage
  overwriting the wrong file).
