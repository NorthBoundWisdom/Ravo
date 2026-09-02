# ADR-0105: Asset versions, stacks, and survey cull

- Status: Accepted
- Date: 2026-09-01
- Extends: [ADR-0008](0008-p0-review-catalog-v2.md),
  [ADR-0009](0009-p1-develop-recipe.md),
  [ADR-0100](0100-paged-library-and-foreground-work-scheduling.md),
  [ADR-0103](0103-named-library-sets.md)

## Context

One catalog asset currently owns one original URI and one canonical recipe.
Photographers need a second grade of the same RAW without copying the file, a
way to group a burst of distinct catalog assets, and an N-up view for culling.
Same-stem RAW+JPEG import is one catalog asset, not a stack. ADR-0059 left
legacy group IDs unsupported; this decision is a new Ravo contract, not a
darktable grouping port.

## Decision

Schema v11 keeps the asset as the selectable identity. A primary row has
`version_ordinal = 0` and null `source_asset_id`. A virtual copy is another
asset row with the same `normalized_uri`, `version_ordinal >= 1`, and
`source_asset_id` equal to the primary. Uniqueness is
`(normalized_uri, version_ordinal)`. Duplicate import still matches the
primary URI only.

Creating a version copies file identity, folder, capture/writable metadata,
tags, review, and the current recipe/history into a new asset ID. Previews are
not copied; they rebuild. Deleting a version removes that row only. Deleting a
primary from the catalog cascades its versions. Disk deletion is allowed only
on a primary and removes the single original plus every version row.

`library_stack` and `library_stack_member` group existing asset IDs. One pick
represents a collapsed stack. An asset belongs to at most one stack. Listing
defaults to collapsed: non-pick members are omitted from the page total and
rows. Expanding is session state on `LibraryPageRequest`, not a smart-collection
field. Unstacking or deleting the last member dissolves the stack. If the pick
is removed, the next remaining member by position becomes pick.

Studio **Survey** is a browse mode that shows two or four selected assets as
exact preview images. Each slot uses CatalogService `request_preview` on the
existing serial owner; it does not start N Develop pipelines, and a loading
thumbnail is not a rating or export oracle. Clicking a slot makes that asset
primary for review commands. Before/After comparison remains one-photo Develop
state.

CLI and Studio share CatalogService. QML displays badges, collapse, and survey
layout; it does not own version ordinals, stack membership, or preview work.

Import treats a same-directory, case-insensitive same-stem RAW plus one JPEG
(`.jpg` / `.jpeg`) as a single photo. The RAW is the catalog original. The JPEG
is a non-cataloged companion used for Gallery browse and copied or moved with
the RAW. Ambiguous companions (distinct `.jpg` and `.jpeg` for one stem) fail
closed. JPEG-only stems remain ordinary assets. Stacks still group already
cataloged assets; they are not the RAW+JPEG pairing model.

## Consequences

- Gallery row count can exceed the number of original files.
- Relink still keys off folder identity; versions share the primary URI and
  folder, so they move together.
- Named collections may contain versions and stack members as ordinary asset
  IDs.
- Same-stem RAW+JPEG import publishes one RAW asset. Gallery browse prefers the
  companion JPEG, then an embedded JPEG, then processed RAW with the Sigmoid
  baseline. Develop, loupe, scopes, and export stay on the RAW.

## Rejected alternatives

- A second recipe table without a selectable asset ID. Preview, history, and
  export already key off asset identity.
- Keeping `normalized_uri` globally unique. That forbids two grades of one
  file.
- Using browse placeholders as survey pixels. They are not Develop or review
  oracles.
