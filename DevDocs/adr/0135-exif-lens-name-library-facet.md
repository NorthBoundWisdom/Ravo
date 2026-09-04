# ADR-0135: Exif LensMake/LensModel capture columns and lens-name facet

- Status: Accepted
- Date: 2026-09-04
- Relates: PRO-METADATA in [TODO.md](../TODO.md), R1 in
  [ProductRoadmap.md](../ProductRoadmap.md)
- Extends: [ADR-0040](0040-capture-time-gps-metadata.md),
  [ADR-0059](0059-library-query-filter-contract.md),
  [ADR-0064](0064-atomic-metadata-refresh-and-export-privacy.md),
  [ADR-0128](0128-capture-metadata-library-facets.md)
- Supersedes: the ADR-0128 residual that left Exif `LensMake` / `LensModel`
  out of catalog persistence and library facets (the focal-length lens proxy
  facet remains)

## Context

ADR-0128 shipped camera make+model, focal-length, and capture-day facets over
existing capture columns. Photographers still need a **true lens-name** browse
key from Exif `LensMake` / `LensModel`. Inventing names from camera make, query-
time sidecar reads, or develop-recipe lensfun lookup would create a second
authority beside capture refresh (ADR-0063/0040/0128). Empty facet tables remain
rejected.

## Decision

### Owned capture columns (schema v14)

- Persist optional UTF-8 `lens_make` and `lens_model` on `CaptureMetadata` /
  `asset_metadata`. Catalog schema bumps to **v14** with additive
  `ALTER TABLE` when the columns are missing (reuse existing columns if already
  present; no dual tables).
- Capture refresh and import overwrite both fields from embedded Exif
  `Exif.Photo.LensMake` / `Exif.Photo.LensModel` (trimmed ASCII). When both
  source tags are absent, both catalog fields clear. Lens name is **not**
  user-writable and is never part of `WritableMetadata` / IPTC patches.
- Do not fake lens name from camera make/model or develop-recipe lens lookup.

### Facet label and enumeration

- Facet label = trimmed `make + " " + model`, or model alone when make is empty
  after trim; when both are empty the pair is absent and does not enumerate.
- `LibraryCaptureFacets` gains a bounded `lens_names` vector of
  `{key, label, count, lens_make, lens_model}` entries (deterministic label/key
  ascending). The existing ADR-0128 **focal-length** `lenses` facet is unchanged.
- Enumeration reuses `list_capture_facets` / scoped predicates; no facet index
  tables; `kLibraryFacetMaximumValues` and `truncated` unchanged.

### LibraryQuery selectors

- Exact pair selectors mirror the camera facet contract:
  - `lens_make_equals` / `lens_model_equals` (optional UTF-8; when either is
    set, both must be set; empty string means absent/NULL/empty in storage)
- Product “lens name” chips set this pair (composed label is presentation only).
- Smart-set documents bump to `kLibraryQueryDocumentSchemaVersion = 4` and still
  accept v1–v3 (new selectors default unset).
- Domain validates before repository listing; invalid pairs never become
  empty-result fallbacks.

### Studio / CLI / privacy

- CLI `catalog facets` emits a `lens_names` group; `catalog list` / scoped
  facets accept `--lens-make` / `--lens-model` equality flags.
- Studio Filter bar adds an opt-in lens-name chip path beside the existing
  focal-length lens chip; QML does not author SQL; facet history stays
  session-only (ADR-0059/0077).
- Facet payloads never include GPS or writables. Export privacy is unchanged:
  lens make/model follow other capture identity under `full` / `no-location`
  and strip under `none` (ADR-0064).

## Non-goals (explicit)

- Facet index tables or denormalized composed `lens_name` columns.
- User-editable lens name, lens serial, or develop-recipe lensfun as a library
  facet.
- Replacing the focal-length proxy facet.
- Adjacent XMP lens merge/writeback (PRO-INTERCHANGE).

## Consequences

PRO-METADATA gains a true Exif lens-name facet on schema v14 without dual
authority. Focal-length browsing remains available. Later interchange can merge
lens tags against the same capture columns.

## Rejected alternatives

- A single composed `lens_name_equals` string as the only selector. Pair
  selectors match the camera facet contract and avoid join/whitespace ambiguity.
- Deriving lens name from camera make/model or recipe lens lookup.
- Query-time Exiv2 reads of source files for facets (ADR-0063/0128).
- Replacing the focal-length `lenses` facet with lens names.
