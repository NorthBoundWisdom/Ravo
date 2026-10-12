# ADR-0177: Bounded library query and machine selection

- Status: Accepted
- Date: 2026-10-11
- Extends: [ADR-0168](0168-ai-control-and-mcp.md)

## Decision

The existing catalog CLI gains opt-in bounded list pages, using LibraryService's
existing query, stack and pagination implementation. `--limit` is 1–512;
`--offset` requires a limit. The versioned page includes catalog identity/revision,
total, offset, continuation, canonical query document and materialized row count. Optional expected catalog
revision is checked before and after the read; concurrent changes return conflict.
Named-set scope stays in the separate `set_id` field because saved query
documents cannot reference another set; stack collapse is reported explicitly.
The legacy unpaged command remains available to existing callers. MCP always
requests a bounded page and forwards the versioned strict query document to CLI.

Live explicit selection is a new `select` kind in the existing reserved-operation
command contract. It carries the current full binding plus a distinct target
asset ID. The desktop controller validates that binding and rejects pending
edits/local gestures. Its existing foreground executor resolves the target
through LibraryService without changing selection. GUI completion checks
cancellation, catalog, listing and binding generations again before invoking the existing
photo-select command. Invalid IDs never clear or redirect the current selection.
The command can select a nonresident catalog asset; the presenter retains its
immutable record and current-query rank before dispatch and keeps its existing
recipe/preview owners. A target outside the current query has no invented rank;
query replacement while resolving rejects the request.
Existing page-driven navigation also reports pending selection as busy until
its page resolves or the intent is replaced/failed. Busy notifications cover
each transition, so clients and command availability cannot edit the previous
photo while a new row is being resolved.
Starting a newer row intent cancels older primary-metadata resolution before
capturing the pending row; a late old result cannot discard the newer intent.
Video selection does not require a Develop recipe, allowing a return to a photo
through the same command. Photo-editing availability remains unchanged.

Selection receipts are session-bounded ephemeral outcomes, not saved edits.
They report the selected target and fresh binding; callers wait for a non-busy
state and read a fresh binding before evaluating a recipe. Reusing an operation
ID reconciles the original outcome without reselecting, even after later manual
navigation. Lost responses are never automatically replayed with a new ID.

The controller owns and cancels the operation; destruction joins borrowed worker
closures before releasing state. No GUI SQL, new executor, renderer, schema,
dependency, MCP-only authority, permissive target fallback or image algorithm.

## Validation

Real CLI tests cover bounded pages, stable ordering/continuation, strict limits,
query schema, stale revision and unchanged catalog. Real stdio MCP tests cover
query → explicit selection → evaluate/commit, unknown and nonresident assets,
stale/racing requests, cancelled queued work, duplicate operation IDs, drafts,
EOF and owner teardown. Verify source/recipe/history safety and run the full
Ravo suite and code-quality checks on the available platform.
