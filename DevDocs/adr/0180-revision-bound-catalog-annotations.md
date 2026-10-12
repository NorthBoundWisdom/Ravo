# ADR-0180: Revision-bound catalog annotations

- Status: Accepted
- Date: 2026-10-12
- Extends: [ADR-0168](0168-ai-control-and-mcp.md), [ADR-0177](0177-bounded-library-query-and-machine-selection.md)

## Decision

The shared service annotation contract accepts one explicit asset and required
catalog revision, with a version-one closed edit payload: review (rating, colour,
pick/reject/unflag), writable metadata (explicit strings or null clears), or
replacement hierarchical keyword paths. These are separate commands, not a
cross-command transaction. No implicit selection, auto-advance or original/XMP
write is introduced. Existing domain validation and service/repository owners
remain authoritative; CLI Studio commit is the baseline client and MCP is thin.

Review, writable-metadata and keyword transactions acquire the SQLite writer
before checking revision. They check cooperative cancellation before acquiring
the writer, after acquisition and before commit; selection writes also check
between assets. Failed or cancelled transactions roll back their revision.
Metadata/keyword result records are materialized before commit, so a readback
failure cannot masquerade as an uncommitted write. Existing callers use the
same extended ports and receive the same strengthened transaction behavior.

The command owner validates edit availability and the shared payload. Desktop
control binds the existing foreground executor task to the full observed Studio
binding and an owner cancellation fence. A changed binding cancels queued work;
database revision guards remain authoritative during publication. Destruction
cancels and joins borrowed closures. Accepted results update catalog metadata,
never the Develop recipe/history. Catalog/selection changes reject late UI
publication without erasing a known commit receipt.

Reserved operation IDs provide exact replay and conflict detection. A committed
annotation remains committed if later recovery synchronization or cancellation
fails; its receipt carries the actual revision and a structured warning. Clients
reconcile missing/expired receipts explicitly and never replay an unknown write
with a new ID. Live state and capabilities expose versioned annotation fields.
State projection is bounded to 1 MiB and reports unavailable explicitly instead
of truncating keyword membership. A terminal `unknown` receipt preserves known
commit facts when revision proof cannot be reconciled; it cannot restart a write.
Unexpected execution exceptions also produce an unresolved outcome rather than
asserting rollback. Clients must reconcile the catalog before another write.

No new executor, dependency, schema, renderer, inference runtime or fallback.
Bulk annotation commands and keyword-tree management remain separate follow-up
work. Performance qualification is deferred per the user's instruction.

## Validation

Shared parser/service and real SQLite tests cover unknown/wrong-type/empty
patches, null clears, hierarchy, mutually exclusive flags, stale revision,
transaction rollback, a held writer lock plus cancellation, late cancellation,
and post-commit recovery failures. Real CLI/MCP subprocess tests cover
query/state → review/metadata/keywords → readback, unchanged source/recipe/history,
exact replay, changed-ID payload conflict, competing clients, selection/catalog
replacement, EOF and close/join. Run available macOS functional contracts and
code-quality checks; timing probes and Windows/Linux are explicitly untested.
