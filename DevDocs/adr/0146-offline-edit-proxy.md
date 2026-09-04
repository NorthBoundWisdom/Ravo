# ADR-0146: Offline-edit proxy class (distinct from Smart Preview)

- Status: Accepted
- Date: 2026-09-04
- Relates: OFFLINE-01 in [TODO.md](../TODO.md)
- Extends: [ADR-0141](0141-dng-conversion-smart-preview-policy.md),
  [ADR-0047](0047-first-frame-raw-cache-lifecycle.md),
  [ADR-0136](0136-derived-tree-backup-restore.md)
- Supersedes: the OFFLINE-01 decision deferral that treated Smart Preview as the
  offline-edit vehicle. ADR-0141 Smart Preview remains **browse-only**.

## Context

ADR-0141 Smart Previews under `{catalog}.ravo/smart-previews/` accelerate Library
browse and must never feed Develop, loupe, scopes, or export. Photographers still
need an explicit **offline-edit proxy** so Develop/history/metadata can continue
while originals are on offline/removable volumes, without making a lossy browse
raster the export authority.

## Decision

### Distinct offline-edit proxy class

- Offline-edit proxies are a **separate class** from ADR-0141 Smart Previews.
- Storage may share the catalog support root `{catalog}.ravo/` but must use a
  distinct tree: `{catalog}.ravo/offline-edit-proxies/<asset-id>/`.
- A proxy never becomes edit authority for the original URI. Recipes remain
  catalog-owned and re-render from the original after verified reconnect.
- Smart Preview paths must not be reused as offline-edit proxies.

### Manifest contract (`ravo.offline-edit-proxy/v1`)

Each proxy directory holds at least:

- `proxy.tif` — bounded raster (max edge / profile declared at create)
- `manifest.json` with:
  - `schema` / `schema_version`
  - `asset_id`
  - `source_sha256`, `source_size_bytes`, `source_mtime_unix_ms`
  - `recipe_cache_key` (digest of the recipe used when creating, or `baseline`)
  - `max_edge`, `profile` (e.g. `srgb`)
  - `proxy_sha256`, `width`, `height`
  - `created_unix_ms`

### Lifecycle (first Ready)

- **Create / list / verify** for explicit assets only (user-initiated; no
  background quota/eviction policy in this tranche).
- **Develop / history / metadata** while the original is offline: recipe writes
  stay on the catalog; Develop render may consume the offline-edit proxy when
  the original file is absent.
- **Export fail-closed** when the original is missing (v1: no proxy export).
  Structured reason `proxy_export_forbidden` when a proxy exists;
  `original_missing` otherwise.
- **Reconnect / relink** verifies the restored original against the manifest
  source hash (and catalog identity), then returns to original-backed rendering.
- Machine-visible media states: `original` | `proxy` | `placeholder` | `missing`.

### Pixel provenance and Develop double-grade guard (COR-01)

Create-time proxies are **8-bit sRGB TIFF presentation rasters** with the
then-current recipe already baked (`pixel_provenance=recipe_baked_srgb8`). While
`media_state=proxy`, Develop/Loupe `request_preview` applies an **identity**
recipe to those pixels so the catalog recipe is not double-applied. Recipes
remain catalog-owned and re-render from the original after verified reconnect.
Further offline delta-preview on top of a baked proxy is deferred to OFFLINE-01
C2.

Publication builds into a unique staging tree, verifies bytes/manifest, then
atomically replaces the previous good proxy directory. Manifest parsing uses
bounded structured number parsing (no throwing `stoll`/`stoull`); schema, asset
id, hash format, dimensions, profile, and path-under-support-root are validated
fail-closed. Corrupt manifests surface in list/status results.

### Non-goals (explicit)

- Using Smart Previews for Develop/export.
- Silent full-resolution export from a proxy.
- Background generation, quota, pinning, or eviction policy (later Ready).
- Replacing originals with proxies.

## Consequences

OFFLINE-01 gains an accepted offline-edit proxy owner and a testable
service/CLI surface. ADR-0141 Smart Preview policy is unchanged.

## Rejected alternatives

- Promoting Smart Preview to Develop fallback.
- Treating proxy pixels as a second live original URI.
- Silent export from proxy when originals are offline.
