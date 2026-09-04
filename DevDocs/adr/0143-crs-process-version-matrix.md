# ADR-0143: CRS / XMP ProcessVersion source-version matrix

- Status: Accepted
- Date: 2026-09-04
- Relates: PRO-INTERCHANGE residual in [TODO.md](../TODO.md), R2 in
  [ProductRoadmap.md](../ProductRoadmap.md)
- Extends: [ADR-0086](0086-lightroom-crs-interchange.md),
  [ADR-0120](0120-xmp-interchange-conflict-matrix.md)

## Context

ADR-0086/0120 map Camera Raw Settings (CRS) onto Ravo Develop using the
**PV2012 recipe-field dialect** (Exposure2012 / ToneCurvePV2012 / …). Lightroom
and ACR emit many `crs:ProcessVersion` strings. Without an explicit matrix,
operators cannot tell which Process Versions are accepted versus fail-closed,
and `xmp-status` only reported a boolean parse result.

## Decision

### Supported Process Versions (PV2012 field dialect)

Ravo accepts the following `crs:ProcessVersion` values as **supported** for the
owned PV2012 recipe-field mapping (not a PV2012 colour engine, Adobe DCP, or
Kelvin/tint schema — ADR-0086):

`6.7`, `8.0`, `9.0`, `10.0`, `11.0`, `15.0`, `15.1`, `15.2`, `15.3`, `15.4`,
`16.0`, `17.0`.

Absence of `ProcessVersion` on an otherwise CRS document is class
`absent` and does **not** by itself fail closed; remaining ADR-0086 key/WB/
profile rules still apply.

### Fail-closed unsupported versions

Any other present `ProcessVersion` (including older PV2010-only values, future
unlisted ACR versions, and non-numeric labels) is class `unsupported` and fails
closed with structured reason `unsupported_crs_process_version`. Import must not
silently drop or approximate the look.

### Status / import reporting

`catalog xmp-status` and import error payloads report:

| Field | Meaning |
| --- | --- |
| `crs_process_version` | Raw `ProcessVersion` string when present |
| `crs_version_class` | `absent` \| `supported-pv2012` \| `unsupported` |

`crs_parse_ok` remains false when the version class is `unsupported` or any
other ADR-0086 gate fails. Version class is still reported when parse fails for
version reasons so operators can see why.

### First Ready

Adapter helper `classify_crs_process_version` + xmp-status/import wiring +
tests. No new recipe fields; no Adobe colour engine.

## Non-goals (explicit)

- Implementing a real PV2012 / ACR colour pipeline.
- Mapping Process Versions outside the listed set.
- Auto-upgrading older Process Versions.

## Consequences

PRO-INTERCHANGE gains a dated source-version matrix. Unsupported Process
Versions stay visible and non-applied. Extending the supported list requires a
new dated ADR (or an explicit supersession) after mapping evidence exists.

## Rejected alternatives

- Best-effort import of unknown Process Versions by ignoring the attribute.
- Treating every CRS document as PV2012 without reporting the version class.
- Shipping an Adobe Process Version colour engine.
