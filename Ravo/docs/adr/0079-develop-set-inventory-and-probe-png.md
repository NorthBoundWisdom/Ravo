# ADR-0079: Recipe-owned Develop `--set` inventory and throwaway probe PNG

- Status: Accepted; Studio catalog observation added by
  [ADR-0080](0080-studio-observes-catalog-revision.md)
- Date: 2026-08-29
- Extends: [ADR-0003](0003-versioned-machine-contract.md),
  [ADR-0009](0009-p1-develop-recipe.md)

## Context

`catalog develop` and `catalog probe` accept repeated `--set name=value`
overrides through `apply_develop_field_strict`. Agents and scripts could not
discover the current field names, kinds, and ranges from the CLI, and `catalog
probe` returned only JSON statistics. A throwaway preview image is useful for
parameter sweeps, but it must not write a recipe or a catalog preview record.

## Decision

- Recipe owns the inventory. `list_develop_set_fields()` reports every
  closed `--set` name that `apply_develop_field_strict` accepts from an
  identity `DevelopParams`, plus the one text field `watermarkText`. Kind and
  numeric bounds are derived from that same strict assign/clamp path.
- Canonical-mask leaves stay prefix-described (`colorHarmonizerMask`,
  `graduatedMask`) because their suffixes are not a closed name list.
- CLI exposes the inventory as `ravo develop-fields` and `ravo catalog
  fields`. Neither command requires `--catalog`. The JSON object contains
  `fields`, `prefixes`, `set`, and `text`.
- Optional `catalog probe --output <file.png>` encodes the in-memory probe RGB
  through `EngineFacade::encode_png` and publishes it with the existing atomic
  no-replace byte writer. The path must end in `.png`. An existing path is
  `conflict`. Success still guarantees `recipe_unchanged` and
  `preview_records_unchanged`; the PNG is not a catalog preview record.

## Consequences

Headless clients can enumerate legal `--set` names and capture a throwaway
preview without mutating catalog state. Studio observation of those catalog
writes is ADR-0080. An MCP wrapper around `ravo` remains later packaging.

## Rejected alternatives

- A second hard-coded field table in CLI. It would drift from
  `apply_develop_field_strict`.
- Writing the probe PNG through the catalog preview cache. That would create
  or replace a preview record.
- Publishing JPEG/TIFF from probe. Probe pixels are display RGB8; typed export
  remains `catalog export`.
