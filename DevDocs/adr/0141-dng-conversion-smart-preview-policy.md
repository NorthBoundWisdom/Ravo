# ADR-0141: DNG conversion + Smart Preview policy

- Status: Accepted
- Date: 2026-09-04
- Relates: PRO-INGEST remaining work in [TODO.md](../TODO.md), R1 in
  [ProductRoadmap.md](../ProductRoadmap.md)
- Extends: [ADR-0102](0102-planned-managed-import-workspace.md),
  [ADR-0104](0104-bounded-rename-and-verified-second-copy-ingest.md),
  [ADR-0047](0047-first-frame-raw-cache-lifecycle.md),
  [ADR-0122](0122-external-editor-derived-assets.md),
  [ADR-0125](0125-ptp-mtp-ingest-transport.md),
  [ADR-0136](0136-derived-tree-backup-restore.md)
- Supersedes: the PRO-INGEST deferral of “DNG conversion and Smart Preview
  policy” for the owned contracts below (packaged converter binary and
  production Smart Preview generation remain residual)

## Context

Photographers expect optional DNG copies for interchange and lightweight Smart
Previews for Library browse. Two failure modes must stay forbidden: silently
replacing camera originals with converted DNG, and letting a lossy browse proxy
become a Develop/export fallback. No Adobe/packaged DNG converter ships in the
default Ravo package today, so the first Ready tranche must expose an explicit
fail-closed stub rather than a half-working encoder.

## Decision

### Optional Copy-mode DNG conversion

- DNG conversion is **user-initiated** only (`CatalogService` + CLI
  `--user-initiated`).
- Mode is **Copy only**: write a new `.dng` into a managed destination under
  `{catalog}.ravo/derived/<asset-id>/` (or an explicit `--output` directory).
  The source original URI/bytes stay untouched; conversion must never Move,
  rename-over, or delete the camera original.
- Successful conversion publishes a **new derived asset** and records provenance
  under `{catalog}.ravo/dng-conversion/<derived-asset-id>.json`.
- Without a **packaged converter** recorded by Dependency Workflow / Packaging
  (named binary or library, SPDX/notices, optional vs bundled), conversion
  **fails closed** with structured reason `dng_converter_unavailable`. No host
  ImageIO/Core Image “export as DNG”, no LibRaw write path pretending to be a
  converter, and no `#ifdef`-only stub that looks shipped.

### Smart Preview = browse-derived only

- A Smart Preview is an optional **browse-lane** derived raster under
  `{catalog}.ravo/smart-previews/<asset-id>/…`.
- It may be used only for Library/browse acceleration.
- It must **never** be a Develop, loupe, scopes, export, or interactive-edit
  fallback. Missing/corrupt Smart Previews must not change Develop behaviour.
- First Ready tranche: service/CLI contract that reports availability and
  refuses generation when no owned Smart Preview generator is packaged
  (`smart_preview_converter_unavailable`), while documenting the Develop
  non-fallback rule in tests (`usable_for_develop` / `develop_fallback` always
  false).

### Stub / CLI contract (this Ready)

- `catalog dng-status` → converter packaging status.
- `catalog dng-convert --asset-id … --user-initiated [--output …]` → service
  `convert_asset_to_dng`; always Copy semantics; fail-closed without packaged
  converter; originals byte-identical.
- `catalog smart-preview --asset-id … [--ensure --user-initiated]` → service
  `smart_preview_status` / `ensure_smart_preview`; fail-closed without packaged
  generator; status JSON states `develop_fallback=false` /
  `usable_for_develop=false` permanently.
- Restore URI known-support prefixes include `dng-conversion/` and
  `smart-previews/`.
- Verified backup format **v3** packages those trees with SHA-256 manifests
  (ADR-0136 extension).
- Studio chrome is optional and out of this tranche.

## Non-goals (explicit)

- Shipping Adobe DNG SDK / third-party converter binaries in this commit.
- Background conversion that replaces originals.
- Using Smart Previews for Develop, export, or full-resolution inspect.
- HEIC owned decode (ADR-0123) and native PTP USB (ADR-0125 residual).

## Consequences

PRO-INGEST gains an accepted DNG/Smart Preview policy and a testable fail-closed
CLI/service surface. A later Ready may enable Copy-mode conversion and browse
Smart Preview generation only after Packaging records the converter/generator.

## Rejected alternatives

- In-place / Move DNG replacement of camera originals.
- Silent host-OS DNG export as a substitute for a packaged converter.
- Falling back to Smart Preview pixels when Develop decode is slow or missing.
