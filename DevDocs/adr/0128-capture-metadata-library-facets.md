# ADR-0128: Camera / lens / capture-date facets over existing capture metadata

- Status: Accepted
- Date: 2026-09-03
- Relates: PRO-METADATA in [TODO.md](../TODO.md), R1 in
  [ProductRoadmap.md](../ProductRoadmap.md)
- Extends: [ADR-0040](0040-capture-time-gps-metadata.md),
  [ADR-0059](0059-library-query-filter-contract.md),
  [ADR-0064](0064-atomic-metadata-refresh-and-export-privacy.md),
  [ADR-0077](0077-compact-library-filter-bar.md),
  [ADR-0100](0100-paged-library-and-foreground-work-scheduling.md)
- Supersedes: the ADR-0059 deferral of **lens** as an unsupported product filter
  for the narrow **focal-length lens facet** only (Exif LensMake/LensModel remain
  residual); the ADR-0119/0124/0126 deferrals of camera/lens/date facets

## Context

Photographers need to browse a library by **camera body**, **lens-related
optics**, and **capture day** the same way they filter by rating or media type.
`asset_metadata` already stores capture make/model, focal length, and local Exif
datetime (ADR-0040). ADR-0059 already owns substring `camera`, numeric
`focal_length_mm`, and `captured_*_unix_s` ranges on `LibraryQuery`, but there
is no validated **facet enumeration** contract, no exact make+model / day keys
for chip selection, and ADR-0059 explicitly left “lens” unsupported. Empty
placeholder facet tables would invent a second authority beside capture refresh.

## Decision

### No facet tables; query existing capture columns

- Camera / lens / capture-date facets are **derived views** over existing
  `asset_metadata` capture columns. This tranche adds **no schema version bump**,
  no facet index table, and no dual live authority with source Exif.
- Facet enumeration and facet equality filters never invent GPS, location
  writables, faces, or IPTC delivery fields.

### Facet kinds (first tranche)

1. **Camera** — distinct `(camera_make, camera_model)` pairs where at least one
   side is non-empty after trim. Label joins non-empty parts with a single
   space. Selecting a facet sets exact equality filters on make and model
   (empty string means absent/NULL/empty in storage).
2. **Lens** — distinct `focal_length_mm` values already persisted on capture
   metadata. This is the **lens-related** facet for this tranche. True Exif
   `LensMake` / `LensModel` / lens serial are **out of scope** until a later
   capture-metadata ADR adds owned columns; do not fake them from camera make.
3. **Capture date** — distinct calendar days from
   `substr(captured_local_exif, 1, 10)` as `YYYY:MM:DD` when
   `captured_local_exif` is present and at least 10 characters. Do **not**
   invent a local day from unzoned `captured_unix_s` (ADR-0040). Selecting a
   day sets `captured_local_date` equality on that prefix.

### LibraryQuery filter / sort contract

- Existing ADR-0059 predicates remain: substring `camera`, numeric ranges
  (including `focal_length_mm`), and inclusive `captured_after_unix_s` /
  `captured_before_unix_s`.
- This ADR adds optional exact facet selectors on `LibraryQuery`:
  - `camera_make_equals` / `camera_model_equals` (optional UTF-8 strings;
    when either is set, both are applied with empty-as-absent semantics)
  - `focal_length_mm_equals` (optional exact double; may combine with the
    existing range only when both agree — validation rejects contradictory
    equality vs range)
  - `captured_local_date` (optional `YYYY:MM:DD` Exif day spelling)
- Sort fields stay ADR-0059. Smart-set documents bump to
  `kLibraryQueryDocumentSchemaVersion = 2` and still **accept** schema v1
  (new selectors default unset).
- Domain validates before repository listing; invalid facet selectors never
  become empty-result fallbacks.

### Enumeration API and bounds

- `CatalogService::list_capture_facets()` / repository
  `list_capture_facets()` return one immutable `LibraryCaptureFacets` value:
  cameras, lenses (focal lengths), and capture dates, each a bounded vector of
  `{key, label, count, …typed fields}` sorted deterministically (label/key
  ascending for camera and lens; date descending).
- Each kind is capped at `kLibraryFacetMaximumValues` (2048). Excess rows are
  truncated after deterministic order; `truncated` is true when any kind hit the
  cap. Callers must not assume completeness beyond the bound.
- First tranche enumerates **catalog-wide** (not scoped by the live
  `LibraryQuery`). Scoped facet counts remain residual.
- Enumeration must not materialize every asset row into the service layer when
  SQL can `GROUP BY`.

### Privacy

- Facet payloads include only camera make/model, focal length, local capture
  day, and counts — never GPS, altitude, location writables, or people.
- Export privacy is unchanged (ADR-0064): capture camera/time remain under
  `full` / `no-location` and are stripped under `none`. Facet UI is catalog-
  local and creates no network or telemetry surface.
- Studio must not persist facet history (ADR-0059/0077): only the current
  in-memory `LibraryQuery` holds selected facet equality filters.

### Studio / CLI

- CLI exposes `catalog facets` and accepts facet equality / existing range
  flags on `catalog list` (and smart-set query JSON via schema v2).
- Studio Filter bar may add opt-in camera / lens / capture-date chips that
  call C++ commands mutating `LibraryQuery` (ADR-0077). QML must not author
  SQL.

## Non-goals (explicit)

- Facet index tables, FTS, or denormalized facet columns.
- Exif LensMake/LensModel persistence or develop-recipe lensfun lookup as a
  library facet.
- LibraryQuery filters on IPTC location writables (still PRO-METADATA residual).
- Scoped facet counts, hierarchical date trees (year→month→day UI), or
  persisted recent facet history.
- Multi-select editing of capture identity (capture remains refresh-owned).

## Consequences

PRO-METADATA gains an accepted facet contract on top of ADR-0040 columns and
ADR-0059 queries without schema churn. Exact facet selectors make chip
selection unambiguous where substring `camera` cannot match “Make Model” as a
single needle. A later tranche may add owned lens-name columns or scoped
counts without reopening placeholder tables.

## Rejected alternatives

- Empty `camera_facet` / `lens_facet` / `capture_date_facet` tables “for later.”
  They would be a second authority and invite drift from capture refresh.
- Treating lens as Exif LensModel before owned persistence. That would invent
  columns or read sidecars at query time against ADR-0063/0040 boundaries.
- Deriving capture-day facets from `captured_unix_s` alone. Unzoned unix is not
  a local calendar day (ADR-0040).
