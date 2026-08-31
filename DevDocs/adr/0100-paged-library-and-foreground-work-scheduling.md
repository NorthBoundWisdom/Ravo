# ADR-0100: Paged library and foreground work scheduling

- Status: Accepted
- Date: 2026-08-31

## Context

Materializing the full catalog in the desktop model and queuing a whole import
batch made memory, query work, and foreground latency proportional to library
size. Large-library work needs one measurable domain contract rather than a
QML cache or a second scheduler.

## Decision

Schema v7 stores adapter-private `display_name` and `folder_uri` projections
plus indexes for accepted filter and sort fields. `LibraryPageRequest` and
`LibraryPage` own validated query, offset, bounded limit, stable keyset cursor,
known total, elapsed database time, and materialized-row count. Sequential
pages use the sort key and asset ID cursor; an explicit viewport jump may use
an offset. Tags, metadata, and preview records are fetched only for the current
page. `EXPLAIN QUERY PLAN` tests pin the page, tag, and folder indexes.

Studio's `AssetListModel` exposes the full logical row count while retaining at
most three 200-row pages. Unloaded rows are placeholders and Gallery delegates
request their page and bounded thumbnail look-ahead. Selection protects its
resident page and remains ID-based across eviction.

Import enumeration is deterministic and bounded. Studio dispatches one normal-
priority item at a time and does not enqueue the remainder. The next import
item is dispatched only after the current result is observed, so cancellation
stops undispatched files. Develop, selection, history, white-balance, and
perspective work use the existing foreground lane; database commits remain
serialized and neither QML nor an adapter owns a scheduler.

The synthetic acceptance fixture contains 10,000 real SQLite rows. Every page
materializes at most 200 assets, traversal order matches the domain oracle, and
the host-local run records page P90/max. Existing exact 960 px interactive and
1600 px settled preview contracts, revision rejection, and the 30 ms Release
P90 slider intent-to-image gate remain the Gallery-to-Edit pixel and latency
authority.

## Consequences

- Listing memory and auxiliary queries scale with page and cache bounds rather
  than total assets.
- Random deep jumps are supported but are not the ordinary sequential path.
- Import remains serial because current measurements do not justify parallel
  probe/decode ownership or its additional memory lanes.
- Private real-photo corpus measurements remain host-local release evidence;
  they are never committed as user data or generalized across platforms.
