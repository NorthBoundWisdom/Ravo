# ADR-0103: Named library sets

- Status: Accepted
- Date: 2026-09-01
- Extends: [ADR-0059](0059-library-query-filter-contract.md),
  [ADR-0100](0100-paged-library-and-foreground-work-scheduling.md)

## Context

Studio can filter the current session with `LibraryQuery` and can list source
folders, but those views disappear when the catalog closes. Photographers
organize jobs as named sets: an explicit membership collection, or a saved
query that is re-evaluated. ADR-0059 forbids a recent-filter history; it does
not forbid a named, user-created set.

## Decision

Schema v10 stores `library_set(id, kind, name, query_json, created_unix_ms,
updated_unix_ms)` and `library_set_member(set_id, asset_id, added_unix_ms)`.
`kind` is `manual` or `smart`. Names are unique, trimmed, 1–128 bytes, and
reject control characters. At most 1,000 sets exist in one catalog.

A manual set owns explicit membership. Adding requires every asset ID to exist;
duplicates in one request are collapsed; a member that is already present is
left unchanged. Removing requires every named ID to already be a member.
Deleting a set cascades membership and never deletes assets. Deleting an asset
cascades that row out of every set.

A smart set stores a versioned `LibraryQuery` document (`schema_version` 1).
The document cannot contain `collection_id`. Listing a smart set re-evaluates
that query through the ordinary paged SQL path and ANDs any additional session
predicates. Empty membership or an empty smart result is valid.

Mutations run in one SQLite transaction with the catalog revision. Callers may
supply the observed revision; a mismatch fails as `stale_catalog_revision`
without writing. Invalid queries, unknown IDs, kind mismatches, and duplicate
names fail closed.

`LibraryQuery.collection_id` selects a set. Manual sets add a membership
predicate; smart sets substitute the stored query and keep session sort.
Paged listing still materializes at most one page. Studio and CLI share
CatalogService. QML displays set rows and forwards named commands; it does not
own membership or SQL.

Last Imported Photos remains a session-only group and is not a catalog set.

## Consequences

- Named sets travel with catalog backup/restore because they live in SQLite.
- Selecting a set is session state and is discarded when the catalog closes.
- Smart-set counts are live query totals, not cached membership.
- Additional blend, version, or stack identity is out of scope.

## Rejected alternatives

- Persisting recent filters as fake smart collections. That reopens ADR-0059.
- Treating folders as collections. Folder identity remains path/relink owned.
- QML-owned set arrays. They would fork membership from SQLite.
- Loading every member to count a smart set. That breaks ADR-0100 bounds.
