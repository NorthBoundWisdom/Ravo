# ADR-0113: Studio export long-edge uses Catalog `max_edge`

- Status: Accepted
- Date: 2026-09-01
- Extends: [ADR-0039](0039-explicit-export-option-controls.md),
  [ADR-0050](0050-ashift-rotation-and-export-scale.md),
  [ADR-0068](0068-typed-batch-export-storage.md)
- Extended by: [ADR-0117](0117-export-box-sharpen-presets-and-restartable-jobs.md)

## Context

Catalog export already resamples through `ExportRequest.max_edge`. CLI
`--max-edge` projects that field. Studio's export dialog still omits it, so a
photographer cannot request a web/client long-edge from the same owner used by
CLI. PRO-EXPORT asked for long-edge/box resize, do-not-enlarge, output
sharpening, reusable presets, and restartable jobs. Box, sharpen, presets, and
jobs remain separate undecided work.

## Decision

- Studio collects one integer `maxEdge` for rendered JPEG/PNG/TIFF through the
  existing typed export-option conversion. CatalogService remains the resize
  owner. QML does not compute output dimensions.
- `0` keeps the current full rendered size. A positive value fits the longer
  edge inside that limit. `fit_within_max_edge` already refuses to enlarge:
  when both source edges are at or below the limit, output size is unchanged.
- Original copy rejects `maxEdge`. Exact source bytes are not a resize path.
- Studio bounds the control to `0…65535`, matching the RAW dimension cap.
  Invalid, missing, or extra keys fail closed before the file dialog. Single
  and batch export share the same options snapshot.

## Consequences

Studio and CLI can request the same long-edge fit. Box resize, output
sharpening, reusable export presets, and restartable background jobs are
accepted separately in ADR-0117.

## Rejected alternatives

- A second Studio-only scaler or QML width/height math.
- Treating original copy as a resized derivative.
- Waiting for box/sharpen/presets before exposing the existing long-edge field.
