# ADR-0119: Hierarchical keywords (decision stub)

- Status: Proposed
- Date: 2026-09-03
- Relates: PRO-METADATA in [TODO.md](../TODO.md), R1 in
  [ProductRoadmap.md](../ProductRoadmap.md)

## Context

Catalog assets already carry flat pick/rating/colour and searchable text, but
photographers need nested keyword vocabularies for cull and delivery. IPTC
subset, location fields, and camera/lens/date facets remain adjacent PRO-METADATA
work. This stub records the keyword-hierarchy decision only so a later accepted
ADR can authorize a narrow Ready tranche without inventing empty schema tables.

## Decision questions (to resolve before Accepted)

1. **Authority:** catalog-owned keyword graph is the live authority; embedded
   source keywords are import/refresh inputs with an explicit merge matrix
   (catalog-only vs source-only vs both-changed). No adjacent XMP writeback as a
   second live authority.
2. **Identity:** stable keyword IDs (not path strings) so rename/move of a node
   preserves membership. Display paths are derived.
3. **Hierarchy shape:** single-parent tree vs multi-parent DAG; synonyms/aliases
   yes/no; case and Unicode folding rules for uniqueness.
4. **Membership:** asset↔keyword links are transactional, revision-checked for
   multi-selection edits, and bounded for LibraryQuery. Bulk apply is
   cancellable; failure rolls back or reports exact partial state per the
   accepted contract.
5. **Export:** embed/omit policy for hierarchical keywords is export-options
   owned (not Develop recipe). Privacy stripping must match that policy exactly.
6. **Out of scope here:** faces/people, GPS/location tables, full IPTC Core/Ext,
   and AI keyword proposals (blocked on AI-00).

## Non-goals for the first Ready tranche (once Accepted)

- No placeholder empty Studio panes or unused SQLite tables before the merge
  matrix and LibraryQuery/index plan are accepted.
- No QML-built SQL; CatalogService owns mutations and queries.
- No silent refresh that clobbers catalog-only keywords.

## Next step

Expand this stub into an Accepted ADR with the merge matrix, ID/rename rules,
LibraryQuery filters, export embed/omit fields, and the smallest migration, then
move only that bounded tranche to P1 / Ready in `TODO.md`.
