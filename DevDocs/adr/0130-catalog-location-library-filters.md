# ADR-0130: LibraryQuery exact filters and facets for catalog location labels

- Status: Accepted
- Date: 2026-09-03
- Relates: PRO-METADATA in [TODO.md](../TODO.md), R1 in
  [ProductRoadmap.md](../ProductRoadmap.md)
- Extends: [ADR-0059](0059-library-query-filter-contract.md),
  [ADR-0077](0077-compact-library-filter-bar.md),
  [ADR-0126](0126-catalog-owned-location-fields.md),
  [ADR-0128](0128-capture-metadata-library-facets.md)
- Supersedes: the ADR-0126 / ADR-0128 deferrals of LibraryQuery filters and facet
  enumeration for the catalog-owned location text quartet

## Context

ADR-0126 owns `country` / `province_state` / `city` / `sublocation` on
`asset_metadata`. Photographers need the same chip-style exact browse as
ADR-0128 capture facets. Substring `LibraryQuery::text` already searches those
labels but cannot select one country without matching titles or other fields.
Empty placeholder location-facet tables would invent a second authority beside
catalog writables.

## Decision

### Exact LibraryQuery selectors

- Add optional exact equality selectors on `LibraryQuery`:
  - `country_equals`
  - `province_state_equals`
  - `city_equals`
  - `sublocation_equals`
- Each selector is independent. When set, empty string means absent/NULL/empty
  storage (same empty-as-absent semantics as ADR-0128 camera make/model).
- Domain validates UTF-8/length before repository listing; invalid selectors
  never become empty-result fallbacks.
- Smart-set documents bump to `kLibraryQueryDocumentSchemaVersion = 3` and still
  **accept** schema v1/v2 (new selectors default unset).

### Location facets (derived views)

- `CatalogService::list_location_facets()` / repository
  `list_location_facets()` return one immutable `LibraryLocationFacets` value:
  countries, province_states, cities, and sublocations — each a bounded vector of
  `{key, label, count}` where key and label are the distinct non-empty trimmed
  field value, sorted label/key ascending.
- Each kind is capped at `kLibraryFacetMaximumValues` (2048) with the same
  truncation/`truncated` contract as ADR-0128. First tranche is catalog-wide.
- Enumeration uses SQL `GROUP BY` on the existing writable columns. No schema
  bump, no facet index table, no GPS/capture fields in payloads.

### Studio / CLI

- CLI `catalog list` accepts `--country` / `--province-state` / `--city` /
  `--sublocation` as exact equality filters (same flag names as
  `catalog metadata` writes; scoped by subcommand).
- CLI `catalog facets` includes the location facet groups beside capture facets.
- Studio Filter bar may add an opt-in Location chip row that mutates
  `LibraryQuery` through C++ commands (ADR-0077). QML must not author SQL. Do
  not persist location filter history.

## Non-goals (explicit)

- Catalog-editable GPS / map UI (PRO-PRESENT).
- Hierarchical place vocabularies or place IDs.
- Scoped facet counts; Exif LensMake/LensModel (still residual).
- Adjacent XMP location merge (PRO-INTERCHANGE).

## Consequences

PRO-METADATA gains browse/filter parity for ADR-0126 labels without schema
churn. Capture GPS remains ADR-0040-only and stays out of facet payloads.

## Rejected alternatives

- Reusing substring `text` alone for country chips (ambiguous matches).
- Empty `location_facet` tables “for later.”
- Coupling location filters to capture GPS equality.
