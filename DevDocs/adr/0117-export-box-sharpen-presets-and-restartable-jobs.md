# ADR-0117: Export box resize, output sharpen order, presets, and restartable jobs

- Status: Accepted
- Date: 2026-09-03
- Extends: [ADR-0032](0032-encoded-byte-publication-contract.md),
  [ADR-0039](0039-explicit-export-option-controls.md),
  [ADR-0050](0050-ashift-rotation-and-export-scale.md),
  [ADR-0068](0068-typed-batch-export-storage.md),
  [ADR-0113](0113-studio-export-long-edge.md)

## Context

Studio long-edge already projects Catalog `max_edge` and never enlarges
(ADR-0113). PRO-EXPORT still needs box fit, output sharpening after resize,
reusable delivery presets, and a restartable background batch without inventing
a second encoder or publishing from QML. Watermark, colour-conversion frame, and
Develop recipe mutation remain separate undecided work.

## Decision

- `ExportOptions` gains optional box limits `max_width` and `max_height`
  (each `0` means unconstrained for that axis). A positive pair fits the
  rendered image inside the box while preserving aspect ratio and never
  enlarging, using the same CatalogService owner as `max_edge`. When both
  long-edge and box limits are set, CatalogService applies the tighter of the
  two resulting fits. Original copy still rejects every resize field.
- Output sharpen is an optional typed export step owned by CatalogService after
  resize and before encode. Defaults are off. Amount/radius/threshold are
  export-local values, not Develop Sharpen recipe fields, and do not mutate the
  stored recipe. QML forwards numbers only; it does not sharpen pixels.
- Pipeline order for rendered JPEG/PNG/TIFF is fixed: Develop render →
  long-edge/box fit → output sharpen → encode → metadata embed → atomic
  no-replace publication. Skipping an optional step does not reorder the rest.
- Reusable export presets are versioned JSON artifacts that serialize the typed
  `ExportOptions` snapshot (format, codec options, `max_edge`, box limits,
  output-sharpen, metadata mode). CatalogService or a thin services helper owns
  load/save/apply; Studio and CLI apply the same snapshot. Presets do not store
  paths, asset IDs, filename templates, or job state. Corrupt or unknown-schema
  presets fail closed.
- Restartable batch delivery reuses ADR-0068 preflight and `export_asset`. A
  durable job record (services-owned, not QML) names the ordered asset IDs,
  options snapshot, output directory, filename template, and per-item outcome.
  Restart resumes only unfinished items; already-delivered files are retained
  and never overwritten. Conflict, cancel, and partial-failure semantics stay
  those of ADR-0068. There is no overwrite, skip-if-unchanged, or unique-name
  guessing mode.
- Watermark, colour-conversion/frame overlays, Develop-recipe mutation on
  export, and PRO-PRESENT remain out of scope.

## Consequences

Photographers can request box-constrained delivery, optional post-resize output
sharpen, reusable option presets, and resume a stopped batch without a second
publication owner. Implementation may land in follow-up tranches; this ADR is
the contract gate for PRO-EXPORT Ready work.

## Rejected alternatives

- QML or Studio-local resizing/sharpening disconnected from CatalogService.
- Applying Develop Sharpen (or any recipe IOP) as a silent export rewrite.
- Storing destinations or asset lists inside export presets.
- Overwrite / skip / unique-name batch policies that break ADR-0032 no-replace.
- Waiting for watermark or colour-frame before box, sharpen, presets, or jobs.
