# ADR-0008: Catalog schema v2 for P0 review state

- Status: Accepted
- Date: 2026-08-24
- Relates to: ADR-0007, Ravo accepted catalog/review baseline

## Context

The first Studio slice stored only import identity and preview cache metadata.
P0 requires persisted rating, color label, and reject flags that survive
reopen, without touching originals or putting SQL in QML.

## Decision

- Catalog schema version is 2.
- Review state lives on `asset` as `rating` (0–5), `color_label` enum text, and
  `rejected`.
- Opening a v1 catalog migrates those columns in one transaction. Unknown newer
  versions stay fail-fast.
- Domain owns `ReviewState` / `LibraryQuery`; services expose
  `set_rating` / `set_color_label` / `set_rejected` and filtered listing.
- Preview contract version is 2 so orientation-aware raster probe/decode does
  not reuse incompatible cache keys.

## Consequences

- Existing v1 libraries reopen after a one-way migration.
- Filtering/sorting can be applied in the service layer over repository lists.
- QML only binds presenter snapshots and sends review commands.

## Rejected alternatives

- Mutating v1 in place without a version bump.
- Storing UI colors instead of the color-label enum.
- Putting review writes in QML or the SQLite adapter's public contract.
