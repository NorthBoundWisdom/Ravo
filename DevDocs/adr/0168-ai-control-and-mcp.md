# ADR-0168: Revision-bound AI grading and local MCP

- Status: Accepted
- Date: 2026-10-11
- Extends: [ADR-0090](0090-versioned-live-studio-control.md)

## Decision

The requested AI grading workflow uses the supported CLI as the mandatory
acceptance client. `ravo mcp` is a C++ stdio adapter over those same JSON commands;
it owns protocol framing, child-process lifetime and immutable artifact resource
projection only. It does not own SQL, recipes, rendering, command policy, AI
inference or another application-state store. No network listener or additional
runtime dependency is introduced. The declared MCP protocol revisions are tested
explicitly; messages from different revisions are never silently mixed.

Studio's desktop command owner validates session, selection, recipe and catalog
revisions on its GUI thread. Database work remains on the existing catalog
executor. Primary metadata and query-relative positions are resolved through
LibraryService independently of display-page residency. Machine snapshots report
selection resolution and bounded selection projections explicitly.

Read-only evaluation captures the exact current recipe, applies Recipe-owned
strict fields in memory, renders through CatalogService/Engine and publishes an
immutable, verified, no-replace PNG. A candidate binds its base recipe, source,
catalog and selection revisions and its exact resulting recipe hash. Evaluation
changes neither the live draft nor catalog recipe, history or preview records.
Commit revalidates that binding rather than reinterpreting it against a new photo.

The desktop operation owner keeps bounded session-local idempotency receipts.
The request ID names an exact immutable command; reuse with different content
fails. Cancellation before the catalog commit prevents publication. Once the
transaction commits, cancellation or preview failure cannot change the receipt
to cancelled or imply that the recipe was not saved. Receipts contain catalog
and history revisions; they remain queryable after selection changes, and expire
with their Studio session. An absent session/expired receipt is an explicit
unknown outcome, never permission to replay a write automatically.

IDs are first reserved by the desktop owner. Its 128-entry store evicts only
inactive entries; unknown/evicted IDs cannot begin a new command. Exact replay
returns the existing receipt before checking today's selection. The store is
ephemeral, not a cross-restart journal. Clients must retain confirmed outcomes
and reconcile recipe/history explicitly if the session or receipt expires.

The stdio adapter supports MCP 2025-11-25 initialization and the 2026-07-28
per-request metadata/discovery contract explicitly. It runs at most eight CLI
children, queues at most 16 input frames of 8 MiB each, and retains at most 64
image resources within 64 MiB (16 MiB per image). Resource expiry is explicit.

Mask commands and history restoration enter the existing command owner. Batch
grading uses explicit confirmed IDs and existing service preconditions, reports
each committed/failed/cancelled item and never claims global atomicity. A later
failure does not roll back independently committed photos or erase their receipts.

MCP transports structured errors and receipts without rewriting business
outcomes. Image resources resolve only issued artifact identities, never arbitrary
filesystem paths. Cancellation is propagated to the operation owner before a
child client is stopped. Closing stdin joins the reader and releases subprocesses
and temporary resources. Original images remain unchanged.

## Validation

Service contracts cover candidate neutrality, source/revision rejection,
transaction failure and per-photo batch outcomes. Desktop/real-CLI contracts
cover missing resident metadata, changed sort ranks, pending selection, strict
preview fences, idempotent requests, cancellation before/after commit and lost
responses. MCP subprocess contracts cover declared protocol versions, schemas,
read/evaluate/commit/image workflows, cancellation, malformed/oversized input,
EOF cleanup and absence of credentials. Full Ravo contracts and source boundaries
remain required; unavailable hosts and private-corpus performance are untested,
not inferred from the macOS synthetic fixtures.

## Protocol references

- [MCP 2025-11-25 lifecycle](https://modelcontextprotocol.io/specification/2025-11-25/basic/lifecycle)
- [MCP 2026-07-28 schema](https://modelcontextprotocol.io/specification/2026-07-28/schema)
- [MCP 2026-07-28 stdio](https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/stdio)
