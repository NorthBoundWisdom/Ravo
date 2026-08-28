# ADR-0044: Studio canonical mask authoring

- Status: Accepted
- Date: 2026-08-28
- Extends: [ADR-0043](0043-canonical-mask-graph-foundation.md)

## Context

ADR-0043 established the immutable schema-v2 graph, bounded evaluator, normal
mix, and two attached-operation consumers, but Studio deliberately only
preserved loaded graph state. S3.2 adds a bounded authoring surface without
moving typed graph ownership, shape mathematics, serialization, service
lifecycle, or scheduling into QML.

## Decision

- `ravo_recipe` owns the pure `DevelopParams` authoring helper and the stable
  `colorHarmonizerMask*` / `graduatedMask*` numeric field names. Kind indexes
  are `0=none`, `1=all`, `2=linear_gradient`, `3=circle`, `4=ellipse`, and
  `5=parametric`; group is deliberately not selectable. All schema limits,
  selector values, finite checks, and parametric threshold ordering reuse the
  canonical mask contract. Values are rejected rather than clamped, rounded,
  reordered, or repaired.
- Selecting a non-none kind creates a schema-v2 leaf and explicitly enables
  the target operation. Studio-generated leaf IDs use the reserved,
  target-specific namespaces `ravo.studio.mask.color_harmonizer.<n>` and
  `ravo.studio.mask.graduatednd.<n>`, where `<n>` is the first collision-free
  positive decimal suffix. The namespace is part of the Studio contract; a
  producer that chooses it is declaring the same ownership class.
- Studio may edit only an attached leaf in its target namespace that is not
  referenced by the other supported operation or by any group edge. External
  IDs, shared leaves, and group roots are read-only. Their ordinary edits and
  resets return structured unsupported errors with stable target/field/reason
  context. Explicit `none` or whole-mask detach remains allowed and preserves
  external/shared/group nodes exactly.
- Detach clears only the target attachment. It deletes the leaf only when that
  leaf is an unshared Studio-owned editable node. It never infers whether an
  explicitly present/enabled default operation existed before Studio created a
  mask, because that provenance is not persisted; operation presence/enabled
  state therefore remains untouched. Existing operation reset semantics and
  `reset_recipe` remain their owners. A single mask-control reset retains the
  attachment; source and channel reset independently, while resetting any
  parametric threshold restores the four-key ramp atomically so monotonicity is
  never repaired by clamping or reordering. Resetting kind returns the leaf to
  canonical `all`, while the whole `<target>Mask` reset is explicit detach.
- The desktop Presenter projects two read-only QVariantMaps, one per target.
  They include attachment/editability/status, kind/source/channel choices and
  indexes, current typed values, and C++-constructed visible numeric control
  descriptors. Spatial radii expose a documented UI soft minimum while recipe
  validation retains its exact strictly-positive canonical bound. Parametric
  slider ranges are bounded by their current neighbours so normal UI edits
  remain monotonic; strict recipe validation remains the authority.
- QML renders a reusable inline editor and forwards only the existing numeric
  preview/commit/reset intents. Presenter routes every mask edit through the
  strict helper before the existing copy → preview/save → undo/cache lifecycle.
  Failed edits leave Develop, saved, undo, cache, selection, revision, and
  cancellation owners unchanged; the presenter shows a translatable generic
  rejection with the stable reason identifier.

## Consequences

Color Harmonizer and Graduated ND can each author one Studio-owned all, spatial,
or parametric canonical attachment through the same Recipe, Catalog live preview,
save/reopen, ordinary Develop edit, and undo/redo state as S3.1. The existing
render preview displays the resulting operation effect only; Studio does not
provide a mask alpha or tinted overlay.

This is not S3, M1, or C14 acceptance. Group-child editing, path/brush masks,
sampling/pickers, histogram or harmony guidance, mask overlay presentation,
additional blend modes, GPU/OpenCL, and atomic legacy-owner retirement remain
separate work. C15 and `cacorrectrgb` remain forbidden until a later explicitly
authorized tranche; strict legacy XMP mask/custom-blend/multi rejection is
unchanged.

## Rejected alternatives

- Store free-form JSON/QVariant commands in QML or services, or duplicate mask
  geometry/threshold mathematics in presentation code.
- Treat a group root, a shared leaf, or an external ID as editable because it
  happens to have an otherwise supported payload.
- Detach recursively, remove external graph nodes, or guess operation
  provenance to disable an explicit default operation.
- Clamp UI values, reorder parametric thresholds, use an all/zero fallback, or
  make failed edits silently disappear.
- Claim an alpha overlay, full group editor, M1, C14 retirement, C15, or a
  legacy-XMP compatibility expansion from this bounded authoring slice.
