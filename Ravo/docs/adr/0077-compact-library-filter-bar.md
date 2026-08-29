# ADR-0077: Studio library filters are opt-in chips around a default rating control

- Status: Accepted
- Date: 2026-08-29
- Extends: [ADR-0059](0059-library-query-filter-contract.md)

## Context

ADR-0059 already owned the validated `LibraryQuery` and exposed search, media,
edit state, rating, color, reject, and sort through the top Studio bar. Showing
every control at once occupied the toolbar even when only a rating filter was
in use.

## Decision

- The **Filter** checkbox opens the filter row. Unchecking it clears the
  in-memory query extras and hides added chips. Sort remains available without
  opening Filter.
- While Filter is on, the default control is a rating star strip: unrated
  (exact 0) and exact 1–5. Clicking the active value returns to Any.
- Search, type, edits, color, and reject appear only after **Add filter** or
  when the current query already has a non-default value for that field. Each
  added control can be removed; removal clears that predicate.
- Chip visibility is QML session state. It is not catalog-persisted and is not
  a recent-filter history. Commands remain the only query mutation path.

## Consequences

The same Studio commands and `LibraryQuery` contract continue to apply. Minimum
rating remains available to command consumers; the default star strip authors
exact ratings.

## Rejected alternatives

- Keeping every filter control enabled behind the checkbox. That still spends
  a full toolbar on unused predicates.
- Persisting which chips were added. ADR-0059 already rejects filter history.
