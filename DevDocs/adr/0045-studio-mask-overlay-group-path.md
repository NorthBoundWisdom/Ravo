# ADR-0045: Studio mask overlay, owned group editor, and path/brush

- Status: Accepted
- Date: 2026-08-28
- Extends: [ADR-0043](0043-canonical-mask-graph-foundation.md), [ADR-0044](0044-studio-canonical-mask-authoring.md)
- Extended by: [ADR-0114](0114-mask-click-placement.md)

## Context

ADR-0043 owns the immutable schema-v2 graph, ROI evaluator, and normal mix.
ADR-0044 lets Studio author one unshared owned leaf and present
external/shared/group state as read-only. P0 still required a preview overlay,
an owned group-child editor, and path/brush capability before Color Harmonizer
could retire its frozen owner. Historic blend-mode completeness, pickers,
histogram/harmony guides, GPU, C15, and leftover GTK mask-manager consumers
remain outside this decision.

## Decision

- Overlay is preview-only session state owned by the desktop presenter. It is
  not recipe data, not a cache key, and never enters export or persisted PNG
  previews. When visible, Catalog requests a live RGB8 preview plus the named
  canonical root's alpha, evaluated on the post-recipe linear working frame
  with that frame as both parametric input and operation-output. Exact alpha
  zero keeps display RGB bits. Desktop C++ composites a fixed yellow tint
  (`255,204,0` at strength `0.65`) after scopes read the un-overlaid image.
  QML only forwards the visibility/target intent.
- Studio-owned unshared groups are editable. Kind index `6` creates a
  collision-safe Studio group whose first child is `replace`; later children
  use union/intersection/difference/exclusion. Selecting a child routes
  spatial/parametric/path/brush fields to that child. Group opacity/inversion
  remain on the group node. Child-index and point-index are Develop authoring
  cursors: they participate in live/undo values but are not serialized into
  the canonical recipe. External, shared, and non-Studio groups stay
  read-only and detach-only. Detaching an owned group deletes that group and
  any now-unreferenced Studio-owned children; it never guesses operation
  provenance.
- Schema v2 adds `path` and `brush` kinds. Path is a closed cubic Bézier of
  3–32 corners with stored handles and a uniform feather normalized against
  `min(full_width, full_height)`. Brush is an open cubic stroke of 2–32
  corners with per-point radius/hardness/density. Coordinates remain
  attached-frame pixel-centre normalized values. The private evaluator
  tessellates with the frozen recursive pixel-size stop, fills with the
  frozen ROI edge-flag algorithm, and applies the frozen path/brush falloff
  stamps. Studio-owned creation uses default geometry and recomputes smooth
  handles when a corner moves; stored custom handles on read-only external
  nodes are preserved. Tessellation above 65,536 samples, missing handles, or
  empty geometry fail closed.
- Color Harmonizer remains the C14 algorithm owner. This tranche completes
  its product mask surface. The frozen `iop/colorharmonizer.c` owner retires
  with CMake registration once a whole-repository consumer search is empty.
  Exclusive OpenCL and leftover order/module-group/manual names stay D0.3/D0.4
  because `legacy/host/data` is freeze-identical. `mask_manager.c` and
  `libs/masks.c` keep remaining develop/history/proxy consumers and are not
  deleted here. Strict legacy XMP mask/custom-blend/multi rejection is
  unchanged. C15 and `cacorrectrgb` stay forbidden.

## Consequences

Studio can show the same canonical alpha used by Color Harmonizer and
Graduated ND, author owned groups, and author path/brush leaves through one
recipe/engine/service path. Remaining S3 work is additional blend modes and
the leftover GTK mask-manager owners. M1 product presentation is accepted;
its old IOP/lib files wait for zero leftover consumers.

## Rejected alternatives

- Composite overlay in QML or persist it in the preview cache/export.
- Treat every group as editable because its payload is otherwise valid.
- Substitute a signed-distance or simple polygon fill for frozen Bézier
  tessellation and ROI falloff.
- Invent legacy-XMP path/brush import or delete leftover GTK mask owners
  while develop/history still reference them.
