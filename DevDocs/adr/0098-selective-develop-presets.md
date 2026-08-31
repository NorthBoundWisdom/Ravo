# ADR-0098: One selective Develop-field owner serves presets and copy/paste

- Status: Accepted
- Date: 2026-08-31
- Extends: [ADR-0065](0065-versioned-recipe-style-artifact.md)
- Extends: [ADR-0078](0078-copy-paste-develop-edits.md)
- Relates to: [ADR-0086](0086-lightroom-crs-interchange.md)

## Context

Recipe-style schema v1 is deliberately complete: saving captures the whole
canonical Recipe and applying replaces the target Recipe apart from asset
identity. That remains the correct portable-style contract, but it cannot
represent a preset that applies Exposure while retaining the target photo's
existing Crop, Color, or Detail changes. Filtering operations out of a v1
template would still reset every omitted target value when the complete style
was applied.

Studio's managed Presets panel also offered import/apply only. Saving from the
File menu wrote a user-owned complete style and selected every current edit
implicitly, so users could not create a reusable subset in the library's
`Ravo Presets` folder.

Studio also exposed a complete session clipboard through four controls: Copy,
Paste, Paste Light, and Paste Color. That implicit copy plus fixed paste groups
could overwrite unrelated destination edits and duplicated the new preset
selection problem.

## Decision

- Recipe-style schema v1 remains a complete replacement artifact with its
  existing parser, serializer, CLI, and Studio behavior.
- Schema v2 adds a required, non-empty `selected_fields` array. Values are
  sorted, unique stable Develop field identifiers. Unknown, duplicate,
  unsorted, empty, oversized, or wrong-type selections reject structurally.
- The template still carries one complete canonical Recipe so selected values,
  enabled state, compound curves, profiles, Retouch, and mask dependencies
  retain their exact versioned representation. Recipe owns the selection and
  merge operation: it decodes source and target through `DevelopParams`, copies
  only selected logical fields, rebuilds canonical operation order, and then
  requires ordinary Engine validation.
- Scalar photographic controls remain independently selectable. Compound
  operations such as curves, profiles, Retouch, output layout, and canonical
  mask-backed operations are atomic logical fields; QML cannot split their
  internal schema. When a selected field carries masks, source graph nodes are
  merged by stable ID while target-only nodes remain. Any invalid combined
  graph or operation state fails before publication.
- The same stable field inventory, baseline-relative candidate projection, and
  merge function own the session clipboard. **Copy Parameters** opens the same
  selection component, with no field selected by default, and stores the exact
  `DevelopParams` snapshot plus the accepted field IDs. **Paste Parameters**
  overlays only that selection through the ordinary history/undo/preview path.
  The complete clipboard and fixed Light/Color paste groups are removed.
- Studio shows **Save…** immediately to the right of **Import…** in Presets.
  The dialog lists only changes relative to the selected photo's product
  baseline and starts with no parameter selected. Name plus at least one
  explicit selection is required. The presenter rechecks that every submitted
  field is still modified, serializes schema v2, and atomically publishes a
  complete `.rstyle.json` in the library's `Ravo Presets` folder after rejecting
  a pre-existing path.
- Applying schema v2 overlays the selected fields onto the target's current
  Recipe and enters the existing save/history/undo/preview lifecycle. Applying
  schema v1 still replaces the complete Recipe. CLI can apply either schema to
  an explicit `--target-recipe`; the older asset/input form remains valid only
  for complete schema-v1 styles.
- Saving is synchronous desktop-owned file work on the UI thread and starts no
  task or detached lifetime. Empty/stale selection, invalid name, unsupported
  field, target omission, directory failure, and output conflict are explicit
  errors; no fallback silently widens the selection or overwrites a file.

## Consequences

Managed presets and the session clipboard can preserve unrelated edits on every
target while complete portable styles remain reproducible and backward
compatible. The stable field inventory becomes part of schema-v2 compatibility
and Studio copy/paste behavior; a new field may be added, but an existing
identifier cannot silently change meaning.

## Rejected alternatives

- Removing unselected operations from a schema-v1 style. Complete application
  would still reset target edits and required operation parameters could become
  invalid.
- Defaulting every checkbox on. That preserves the old implicit behavior and
  does not establish affirmative user choice.
- Merging Recipe JSON in QML. It would duplicate schema, ordering, masks, and
  validation outside the Recipe owner.
