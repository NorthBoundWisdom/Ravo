# ADR-0156: XMP fail-closed for unrepresentable multi-instance locals

- Status: Accepted
- Date: 2026-09-04
- Relates: LOCAL-01 in [TODO.md](../TODO.md)
- Extends: [ADR-0120](0120-xmp-interchange-conflict-matrix.md),
  [ADR-0143](0143-crs-process-version-matrix.md),
  [ADR-0145](0145-multi-instance-local-adjustments.md)
- Does not supersede ADR-0120 conflict resolve or ADR-0138 adjacent metadata merge.

## Context

ADR-0145 allows ordered Exposure and Color Balance RGB instance vectors with
owned masks. CRS/XMP only carries a singleton Exposure2012-style look subset.
Export previously listed unrelated omissions and still wrote a sidecar that
silently dropped every extra instance. Import of a CRS look onto a multi-instance
catalog recipe would likewise collapse the vector. Photographers need an
explicit fail-closed gate with a stable reason rather than silent loss.

## Decision

### Unrepresentable multi-instance predicate

`crs_xmp_unrepresentable_multi_instance_reason(look)` returns
`unrepresentable_multi_instance_local_adjustments` when either
`exposure_instances.size() > 1` or `color_balance_rgb_instances.size() > 1`.
Empty vectors and single-instance recipes remain CRS-exportable under the
existing singleton mapping (masks and other unmapped fields may still appear in
`omitted_catalog_fields` without blocking).

### Export

`export_crs_xmp` / `export_xmp_adjacent_interchange` / `xmp_interchange_export`
fail closed with `ErrorCode::kUnsupported` and reason
`unrepresentable_multi_instance_local_adjustments`. No sidecar bytes are written.

### Status

`xmp_interchange_status` loads the catalog recipe and, when the predicate
matches, sets `catalog_crs_unrepresentable_reason` to the same stable string so
Studio/CLI can show the block before export. Conflict classification is
unchanged.

### Import

When a CRS look would be applied (`crs_parse_ok` path in
`xmp_interchange_import`), the same catalog predicate fail-closes before
`apply_crs_look` / `save_develop`. Metadata-only / keyword-only imports that do
not apply CRS remain allowed.

### First Ready

- Predicate + export/status/import fail-closed with shared reason.
- Unit/catalog tests covering multi-instance Exposure and Color Balance RGB.
- CLI `xmp-status` JSON surfaces `catalog_crs_unrepresentable_reason`.

## Consequences

LOCAL-01 XMP interchange no longer silently drops multi-instance locals.
Photographers keep Ravo recipe authority until a future representable dialect
exists.

## Rejected alternatives

- Emitting only `.front()` and listing instances under `omitted_catalog_fields`
  (silent data loss).
- Approximating locals as CRS local-adjustment stacks (out of scope; fail closed).
- Blocking metadata-only sidecar import when the catalog has multi-instance
  recipes (CRS apply is the destructive path).
