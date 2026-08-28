# ADR-0054: Leftover RAW denoise maps to `ravo.raw.denoise`

- Status: Accepted
- Date: 2026-08-29
- Extends: [ADR-0053](0053-rgbcurve-middle-grey-uncompensate.md)

## Context

P2 after RGB curve is leftover `iop/rawdenoise.c`: a pre-demosaic wavelet
denoise on each Bayer plane after a square-root variance-stabilizing
transform. Frozen fixture `0049-rawdenoise` stores v2 164-byte gzipped
payloads (threshold plus four 5-band curves). X-Trans uses a separate path.

## Decision

- Leftover rawdenoise v2 unmasked singleton history maps to `ravo.raw.denoise`
  when band x positions are the frozen uniform grid `0, 0.25, 0.5, 0.75, 1`.
  Threshold 0 is identity. Band y values become `y_all*`/`y_red*`/`y_green*`/
  `y_blue*`.
- CPU follows leftover `wavelet_denoise` + `dwt_denoise`: per 2×2 CFA plane,
  `sqrt`, five-level hat transform, soft-threshold with
  `noise_all[i] * force_all^4 * force_channel^4 * 16 * 16 * threshold` using
  reversed band indices, then square back onto the owned Bayer copy. Band y is
  used as leftover `force` because the fixture samples Catmull-Rom at the
  node x positions.
- X-Trans, non-Bayer, and incomplete frames reject. Raster RGB recipes that
  still contain the operation fail-fast. Nonlinear ICC is outside this
  contract.

## Consequences

`0049` imports. Catalog preview keys include the denoise parameters. Studio
persists threshold and bands through Develop round-trip; a dedicated wavelet
UI is later P2 work. Leftover G1/G3 pull-together after denoise stays later.

## Rejected alternatives

- Reusing `ravo.detail.denoiseprofile`. That is a post-demosaic Y0U0V0 owner.
- Porting X-Trans wavelet in this tranche.
