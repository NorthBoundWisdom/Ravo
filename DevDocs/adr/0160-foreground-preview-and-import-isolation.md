# ADR-0160: Isolate foreground preview from import work

Date: 2026-09-27
Status: Accepted

## Problem

Foreground priority in one serial executor cannot interrupt an already running
catalog scan, whole-batch destination preflight, or copy/move import. A cache hit
still waits behind that work. Global catalog revision guards also reject a photo
edit when an unrelated import has merely added another asset.

## Decision

Studio owns separate foreground and import executors. Each owns its own Engine,
raster decoder, CatalogService and SQLite connection, constructed and destroyed
on that executor. Scans, destination planning, preflight, transfer and deferred
import previews use the import executor. Selected-image recipe loading, Develop
and inspect remain foreground work. Visible browse requests retain bounded
demand and cancellation when selected-image work supersedes them.

Both services share one mutex-protected filesystem preview-cache owner and one
recovery-publication lock. SQLite remains the sole durable state owner; no SQL
or image algorithm moves into QML. Import and foreground writes are short
transactions; filesystem copy/hash/decode never holds a catalog transaction.
Catalog replacement and shutdown cancel import work, drain its owner, and then
release foreground services. UI publication checks operation generations so old
work cannot update a replacement catalog or import operation.

Ordinary photo edits carry their observed photo recipe instead of treating
unrelated catalog insertions as conflicts. The service checks that expectation,
and the repository checks the observed stored state inside the write transaction.
Real same-photo changes still fail with a structured conflict. Explicit CLI
catalog-revision preconditions retain their strict meaning.

This adapts RapidRAW's separation of interactive loading and background work to
Ravo's existing service/adapter architecture. There are no detached workers,
alternate renderers, implicit retries of failed writes, or GPU-to-CPU fallbacks.

## Validation

Gate scan/preflight/import workers deterministically and require an uncached
foreground preview to complete before the gate opens. Check newest-selection
publication, import cancellation, catalog replacement and destruction. Check an
edit after unrelated asset insertion, a conflicting same-photo edit, and
transactional rejection without history mutation. Run catalog/service contracts,
desktop tests and the full Ravo suite for this scheduling boundary.
