# ADR-0120: Adjacent XMP interchange conflict matrix (first slice)

- Status: Proposed
- Date: 2026-09-03
- Relates: PRO-INTERCHANGE in [TODO.md](../TODO.md), R2 in
  [ProductRoadmap.md](../ProductRoadmap.md)
- Extends: [ADR-0064](0064-atomic-metadata-refresh-and-export-privacy.md),
  [ADR-0119](0119-hierarchical-keywords.md)

## Context

PRO-INTERCHANGE needs user-initiated XMP/catalog conversion and external-editor
round trips without adjacent sidecars becoming a second live authority.
ADR-0119 already keeps hierarchical keywords catalog-owned and out of automatic
XMP writeback. This ADR proposes only the **conflict matrix** for an explicit,
user-initiated adjacent-XMP read/apply path—the smallest decision that unblocks
a later Ready tranche without inventing import/export owners here.

## Decision questions (to resolve before Accepted)

1. **Initiation:** every adjacent-XMP read or catalog→XMP write is an explicit
   CLI/Studio command. No watcher, no import auto-attach, no silent writeback.
2. **Conflict matrix:** classify catalog-newer / sidecar-newer / both-changed /
   identical using catalog revision (or recipe/content fingerprint) vs sidecar
   mtime+hash. Each class has one fail-closed or user-chosen outcome; no silent
   last-write-wins.
3. **Authority:** after a successful apply, SQLite remains the sole live edit
   authority. The sidecar is either a read-only conversion artifact or a newly
   written export packet—not a dual-write mirror.
4. **Scope of first Ready slice (once Accepted):** recipe-field XMP apply/export
   conflict preflight only. Keyword/IPTC/location packets, external-editor
   derived assets, and foreign-catalog migration stay out of that slice.

## Non-goals for the first Ready tranche (once Accepted)

- In-place foreign Lightroom/Capture One catalog migration.
- Hidden external renderer or original RAW mutation.
- Automatic adjacent sidecar watch/merge.
- Keyword hierarchy writeback (ADR-0119 remains catalog-owned until a later
  accepted merge matrix covers keywords explicitly).

## Next step

Expand this Proposed stub into an Accepted ADR with the exact conflict tuples,
fingerprints, CLI/Studio command names, and cancellation/preflight errors, then
move only that bounded PRO-INTERCHANGE slice to P1 / Ready in `TODO.md`.
