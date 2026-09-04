# ADR-0154: External-editor working-copy reopen, abandon, and conflict states

- Status: Accepted
- Date: 2026-09-04
- Relates: EDITIN-01 in [TODO.md](../TODO.md)
- Extends: [ADR-0122](0122-external-editor-derived-assets.md),
  [ADR-0139](0139-external-editor-os-open-and-derived-stack.md)
- Does not supersede ADR-0139 OS open-with or derived auto-stack.

## Context

EDITIN-01 already creates a catalog-owned TIFF working-copy session and
registers returned pixels via explicit check-returned (no watch-folder). Studio
kept only an in-memory session map: Cancel/clear did not remove durable session
trees, process restart could not reopen an existing session, and conflict cases
(source mutated, missing working file, stale catalog revision, unchanged return)
lacked a single machine-visible status for Studio/CLI.

## Decision

### Machine states (`ravo.external-editor.working-copy/v1` status)

`CatalogService::external_editor_working_copy_status(working_copy_id)` loads the
durable session and reports one primary `machine_state`:

| State | Meaning |
| --- | --- |
| `pending` | Working TIFF present and still matches the create fingerprint |
| `modified` | Working TIFF present and differs — eligible for check-returned |
| `missing_working_copy` | Session exists; working TIFF is absent |
| `source_conflict` | Source original fingerprint no longer matches the session |
| `stale_catalog` | Live catalog revision differs from `observed_catalog_revision` |

Severity order when multiple apply: `source_conflict` >
`missing_working_copy` > `stale_catalog` > `modified` > `pending`. Status also
exposes boolean flags (`working_copy_present`, `working_copy_modified`,
`source_original_unchanged`, `catalog_revision_current`) and a `reason` string
matching the primary state.

`check_external_editor_returned` additionally fail-closes with
`reason=source_mutated_during_return` when the source original fingerprint
diverges from the session before register. Existing
`editor_output_unchanged` / `editor_output_missing` /
`stale_catalog_revision` reasons remain.

### Abandon (cancel without register)

`abandon_external_editor_working_copy` requires `user_initiated`, removes the
durable `{catalog}.ravo/external-editor/working-copies/<id>/` tree (session +
working TIFF), and never registers derived pixels. Originals stay
byte-identical. Optional `expected_catalog_revision` fail-closes stale callers.
Studio Abandon calls this API then clears the in-memory session map; Clear
Session alone remains UI-only memory clear.

### Reopen existing session

`reopen_external_editor_working_copy` loads status for an existing id (after
restart or when resuming). It never creates a new TIFF. Optional
`invoke`/`openAfterReopen` is a presentation concern (Studio/CLI OS open of
`working_path`) only when status is `pending` or `modified`.
`list_external_editor_working_copies` enumerates durable sessions (optional
`source_asset_id` filter) so Studio can resume the active asset’s session.

### CLI

```text
catalog editor-working-copy-status --catalog <path> --working-copy-id <id>
catalog editor-working-copy-list --catalog <path> [--asset-id <source>]
catalog editor-abandon-working-copy --catalog <path> --working-copy-id <id>
  --user-initiated [--revision N]
catalog editor-reopen-working-copy --catalog <path> --working-copy-id <id>
  --user-initiated [--invoke-os-open] [--application-path <path>]
```

### Non-goals

- Watch-folder auto-register; proprietary editor scripting.
- Naming templates beyond default working-copy paths.
- Package-host matrix / profile bit-depth equality beyond sRGB uint8/uint16.
- Auto-delete of registered derived assets.

## Consequences

EDITIN-01 gains durable abandon, reopen after restart, and clear conflict
machine states shared by service, CLI, and Studio without a second authority.

## Rejected alternatives

- Treating Cancel as memory-only forever (leaves orphan working-copy trees).
- Recreating a new TIFF on “reopen” (would discard in-editor unsaved work).
- Silent success when source originals mutated during return.
