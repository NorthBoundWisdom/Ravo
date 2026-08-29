# ADR-0059: Library filtering is one validated value query without recent-history tracking

- Status: Accepted
- Date: 2026-08-29
- Extends: [ADR-0008](0008-p0-review-catalog-v2.md)

## Context

The old collection/filter UI persisted a stack of SQL-like recent rules and
offered fields coupled to darktable's database, image grouping, local-copy,
module-order, and export/print bookkeeping. Ravo already owned folder, tag,
rating, color-label, reject, and deterministic sorting, but had no stable
contract for the rest of the supported product filters and no explicit
disposition for legacy-only fields.

## Decision

- `LibraryQuery` is the sole immutable domain value passed to
  `CatalogService::list_assets`. Domain validates it before repository listing
  or filtering; invalid values never become an empty-result fallback.
- Supported predicates are rating, color labels, reject state, folder, exact
  tag, ASCII-case-insensitive text over filename/URI/media/tags/writable
  metadata/camera, exact media types, edited/unedited state, camera make/model,
  ISO, aperture, focal length, shutter, aspect ratio, import time, and capture
  time. Numeric and time endpoints are inclusive.
- Supported stable primary sorts are import time, capture time, filename,
  rating, and file size, with asset ID as deterministic tie-breaker. Missing
  capture times sort after present values in both directions.
- Studio exposes photo text, RAW/JPEG/PNG/TIFF, edited state, existing review
  predicates, capture/file-size sort, and clear through the C++ command
  registry. Folder/tag remain in the Library panel. More specialized query
  fields remain available to service consumers without duplicating SQL in QML.
- Ravo does not persist a recent-filter history. Only the current in-memory
  `LibraryQuery` exists, and closing a catalog discards it. This avoids
  creating an unrequested user-behavior history and replaces, rather than
  emulates, `recentcollect`.
- Change/export/print timestamps, lens, exposure bias, local-copy state,
  duplicate/group IDs, module/module-order, white-balance, flash, exposure
  program, metering mode, and arbitrary metadata-column rules are unsupported
  product states. They get no compatibility key or empty shell.

## Consequences

The exclusive GTK `recentcollect`, `filtering`, and `libs/filters/*` owners are
removed. Shared `common/collection*`, selection/review core, view proxies,
configuration keys, and old documentation remain until their other consumers
reach S8/D0 zero-consumer gates. No catalog schema or source file is changed by
filtering.

## Rejected alternatives

- Passing old rule strings or SQL through services. That would leak database
  schema and legacy boolean-rule semantics across the domain boundary.
- Treating invalid filters as no matches. It hides caller errors and makes an
  empty library indistinguishable from an invalid query.
- Persisting recent queries for compatibility. The product has no current
  requirement for behavioral history, and old config keys are not a Ravo data
  format.
