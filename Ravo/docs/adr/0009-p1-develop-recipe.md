# ADR-0009: P1 Develop uses one canonical recipe per asset

- Status: Accepted
- Date: 2026-08-24
- Relates to: ADR-0008, SPEC.md P1

## Context

P1 needs non-destructive global edits that survive catalog reopen without
writing into originals or storing QML control state. The engine already has a
versioned recipe/operation contract and a CPU exposure path.

## Decision

- Catalog schema version is 3.
- Each asset may have one active canonical recipe in `asset_recipe`.
  Identity/default edits store no row (`has_edits = false`).
- `DevelopParams` is a service/desktop mapping onto the recipe. Persistence
  is recipe JSON, not slider positions or a UI blob.
- CPU operations for white balance, light, color, rotate, straighten, crop, flip, sharpen,
  vignette, grain, velvia, color balance, a linked RGB tone curve, and the other
  registered global operations execute on a linear RGB working buffer, converting
  to Lab when the frozen `legacy/src/iop` `process()` path is Lab. The tone curve
  schema records `working_space` (`srgb` or `linear_rgb`) and interpolates
  monotone Hermite on a 0–1 point list; it is not the leftover Lab L/a/b widget. Raster inputs are linearized
  from sRGB8; RAW continues through the existing Bayer path before the same ops.
  The math follows the CPU kernels (USM on L, simplex grain, lift/gamma/gain,
  velvia, superellipse vignette, Orton soften) without GTK, OpenCL, or blend
  ABI.
- Interactive previews include a recipe digest in the cache key. Before/after
  requests the identity preview without changing stored edits. Crop-overlay
  previews keep 90° rotate and flip, strip `ravo.geometry.crop` and
  `ravo.geometry.straighten` (`ignore_crop` + `ignore_straighten`), and rotate
  the working image in Qt Quick so the edge stroke and photo share one GPU
  transform. Live crop-box and straighten drags stay in memory; straighten no
  longer re-renders pixels while the crop tool is open. Release persists the
  recipe. Changing straighten fits the crop to the largest axis-aligned
  rectangle inside the rotated photo. The overlay waits for that uncropped,
  unstraightened preview.
- Studio coalesces develop work to one in-flight render plus at most one pending
  save and one pending preview. Stale results are discarded; render failure
  keeps the last verified preview.
- Undo/redo is in-session only.

## Consequences

- Opening a v1 or v2 catalog migrates to v3 in one transaction.
- Grid thumbnails may remain the unedited cache; loupe/develop show the edited
  preview and an edit badge.
- Unknown future recipe/operation versions stay fail-fast.

## Rejected alternatives

- Serializing QML or Qt object state into catalog rows.
- Applying QML graphical effects as product edits.
- Building a full version/history graph in P1.
